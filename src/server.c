#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

#include "protocol.h"
#include "logger.h"
#include "sysinfo.h"

#define DEFAULT_PORT 8888
#define MAX_CLIENTS 64
#define BUFFER_SIZE 4096

// Client tracking table in parent process
typedef struct {
    pid_t pid;
    int ipc_fd;             // Parent's end of socketpair (sv[0])
    char nickname[32];
    char ip_str[INET_ADDRSTRLEN];
    int port;
} ClientEntry;

static ClientEntry g_clients[MAX_CLIENTS];
static int g_client_count = 0;
static volatile sig_atomic_t g_server_running = 1;

// Robustness toggle flags (default enabled, can be disabled via CLI for testing)
static int g_opt_robust_sigchld = 1;
static int g_opt_robust_sigpipe = 1;
static int g_opt_robust_reuseaddr = 1;

// Forward declarations
static void run_worker(int client_tcp_fd, int parent_ipc_fd, const char *client_ip, int client_port);
static void broadcast_from_parent(uint16_t type, const char *payload, uint32_t len, int exclude_ipc_fd);
static void remove_client_by_ipc_fd(int ipc_fd);

// Signal handler for server graceful shutdown
static void handle_sigint(int sig) {
    (void)sig;
    g_server_running = 0;
}

// Signal handler for reaping child processes (SIGCHLD)
static void handle_sigchld(int sig) {
    (void)sig;
    int saved_errno = errno;
    pid_t pid;
    int status;
    // Non-blocking wait to harvest all exited child processes
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        LOG_DEBUG("Reaped terminated child process PID: %d", pid);
    }
    errno = saved_errno;
}

static void print_usage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("Options:\n");
    printf("  -p, --port <port>          Port to listen on (default: %d)\n", DEFAULT_PORT);
    printf("  -d, --debug <level>        Debug log level (0:ERROR, 1:WARN, 2:INFO, 3:DEBUG)\n");
    printf("      --disable-sigchld      Disable SIGCHLD handler (zombie testing)\n");
    printf("      --disable-sigpipe      Disable SIGPIPE ignore (broken pipe testing)\n");
    printf("      --disable-reuseaddr    Disable SO_REUSEADDR (port reuse testing)\n");
    printf("      --no-robustness        Disable all robustness mechanisms\n");
    printf("  -h, --help                 Show this help message\n");
}

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    int debug_lvl = LOG_LVL_INFO;

    static struct option long_options[] = {
        {"port",              required_argument, 0, 'p'},
        {"debug",             required_argument, 0, 'd'},
        {"disable-sigchld",   no_argument,       &g_opt_robust_sigchld, 0},
        {"disable-sigpipe",   no_argument,       &g_opt_robust_sigpipe, 0},
        {"disable-reuseaddr", no_argument,       &g_opt_robust_reuseaddr, 0},
        {"no-robustness",     no_argument,       0, 'N'},
        {"help",              no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:d:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'p':
                port = atoi(optarg);
                break;
            case 'd':
                debug_lvl = atoi(optarg);
                break;
            case 'N':
                g_opt_robust_sigchld = 0;
                g_opt_robust_sigpipe = 0;
                g_opt_robust_reuseaddr = 0;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            case 0:
                // Handled long option flag
                break;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    log_set_level((LogLevel)debug_lvl);
    LOG_INFO("Starting Chat Server on port %d...", port);
    LOG_INFO("Robustness status: SIGCHLD=%s, SIGPIPE=%s, SO_REUSEADDR=%s",
             g_opt_robust_sigchld ? "ENABLED" : "DISABLED",
             g_opt_robust_sigpipe ? "ENABLED" : "DISABLED",
             g_opt_robust_reuseaddr ? "ENABLED" : "DISABLED");

    // Initialize client tracking table
    memset(g_clients, 0, sizeof(g_clients));
    for (int i = 0; i < MAX_CLIENTS; i++) {
        g_clients[i].ipc_fd = -1;
    }

    // 1. Setup Signal Handlers
    struct sigaction sa_int;
    memset(&sa_int, 0, sizeof(sa_int));
    sa_int.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa_int, NULL);
    sigaction(SIGTERM, &sa_int, NULL);

    if (g_opt_robust_sigpipe) {
        // Robustness 2: Ignore SIGPIPE so server does not abruptly crash on write to disconnected client
        signal(SIGPIPE, SIG_IGN);
        LOG_DEBUG("SIGPIPE ignored for robustness");
    } else {
        signal(SIGPIPE, SIG_DFL);
        LOG_WARN("SIGPIPE left as DEFAULT (Server may crash if client abruptly disconnects)");
    }

    if (g_opt_robust_sigchld) {
        // Robustness 1: Handle SIGCHLD to automatically reap zombie child processes
        struct sigaction sa_chld;
        memset(&sa_chld, 0, sizeof(sa_chld));
        sa_chld.sa_handler = handle_sigchld;
        sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
        sigaction(SIGCHLD, &sa_chld, NULL);
        LOG_DEBUG("SIGCHLD handler installed for zombie prevention");
    } else {
        signal(SIGCHLD, SIG_DFL);
        LOG_WARN("SIGCHLD handler DISABLED (Zombie processes will accumulate)");
    }

    // 2. Create Listening TCP Socket
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        LOG_ERROR("Failed to create socket: %s", strerror(errno));
        return 1;
    }

    // Robustness 3: SO_REUSEADDR to avoid "Address already in use" on fast restart
    if (g_opt_robust_reuseaddr) {
        int opt_val = 1;
        if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt_val, sizeof(opt_val)) < 0) {
            LOG_WARN("setsockopt SO_REUSEADDR failed: %s", strerror(errno));
        } else {
            LOG_DEBUG("SO_REUSEADDR enabled");
        }
    } else {
        LOG_WARN("SO_REUSEADDR DISABLED (Server may fail to restart while in TIME_WAIT)");
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons((uint16_t)port);

    if (bind(listen_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        LOG_ERROR("bind() failed on port %d: %s", port, strerror(errno));
        close(listen_fd);
        return 1;
    }

    if (listen(listen_fd, MAX_CLIENTS) < 0) {
        LOG_ERROR("listen() failed: %s", strerror(errno));
        close(listen_fd);
        return 1;
    }

    LOG_INFO("Server listening on 0.0.0.0:%d (Parent PID: %d)", port, getpid());

    // 3. Main Server Event Loop (select on listen_fd + child IPC fds)
    while (g_server_running) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listen_fd, &read_fds);
        int max_fd = listen_fd;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (g_clients[i].ipc_fd != -1) {
                FD_SET(g_clients[i].ipc_fd, &read_fds);
                if (g_clients[i].ipc_fd > max_fd) {
                    max_fd = g_clients[i].ipc_fd;
                }
            }
        }

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int activity = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
        if (activity < 0) {
            if (errno == EINTR) {
                continue; // Interrupted by signal
            }
            LOG_ERROR("select() error: %s", strerror(errno));
            break;
        }

        // New client incoming
        if (FD_ISSET(listen_fd, &read_fds)) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
            if (client_fd < 0) {
                if (errno != EINTR) {
                    LOG_WARN("accept() failed: %s", strerror(errno));
                }
            } else {
                char client_ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
                int client_port = ntohs(client_addr.sin_port);

                if (g_client_count >= MAX_CLIENTS) {
                    LOG_WARN("Connection rejected from %s:%d (Max clients %d reached)",
                             client_ip, client_port, MAX_CLIENTS);
                    const char *reject_msg = "Server full. Please try again later.\n";
                    send_packet(client_fd, MSG_SERVER_NOTIFY, reject_msg, strlen(reject_msg));
                    close(client_fd);
                } else {
                    // Create full-duplex socketpair for Parent-Child IPC
                    int sv[2];
                    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
                        LOG_ERROR("socketpair() failed: %s", strerror(errno));
                        close(client_fd);
                    } else {
                        // FORK child process (Threads forbidden by instruction!)
                        pid_t pid = fork();
                        if (pid < 0) {
                            LOG_ERROR("fork() failed: %s", strerror(errno));
                            close(sv[0]);
                            close(sv[1]);
                            close(client_fd);
                        } else if (pid == 0) {
                            // CHILD PROCESS (Worker)
                            close(listen_fd); // Close listener in child
                            close(sv[0]);     // Child uses sv[1]
                            run_worker(client_fd, sv[1], client_ip, client_port);
                            exit(0);
                        } else {
                            // PARENT PROCESS (Master)
                            close(client_fd); // Parent does not handle client TCP directly
                            close(sv[1]);     // Parent uses sv[0]

                            // Add to client table
                            for (int i = 0; i < MAX_CLIENTS; i++) {
                                if (g_clients[i].ipc_fd == -1) {
                                    g_clients[i].pid = pid;
                                    g_clients[i].ipc_fd = sv[0];
                                    snprintf(g_clients[i].ip_str, sizeof(g_clients[i].ip_str), "%s", client_ip);
                                    g_clients[i].port = client_port;
                                    snprintf(g_clients[i].nickname, sizeof(g_clients[i].nickname), "User_%d", pid);
                                    g_client_count++;
                                    LOG_INFO("Client connected: %s:%d -> Spawned Worker PID %d (Total: %d)",
                                             client_ip, client_port, pid, g_client_count);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }

        // Check messages from child workers via IPC
        for (int i = 0; i < MAX_CLIENTS; i++) {
            int ipc_fd = g_clients[i].ipc_fd;
            if (ipc_fd != -1 && FD_ISSET(ipc_fd, &read_fds)) {
                PacketHeader hdr;
                char *payload = NULL;
                int ret = recv_packet(ipc_fd, &hdr, &payload);

                if (ret <= 0) {
                    // Worker disconnected or exited
                    LOG_INFO("Worker PID %d disconnected (Client: %s)", g_clients[i].pid, g_clients[i].nickname);
                    char notify_buf[128];
                    snprintf(notify_buf, sizeof(notify_buf), "[*] %s left the chat.", g_clients[i].nickname);
                    broadcast_from_parent(MSG_SERVER_NOTIFY, notify_buf, strlen(notify_buf), ipc_fd);

                    remove_client_by_ipc_fd(ipc_fd);
                } else {
                    // Process message from worker
                    switch (hdr.msg_type) {
                        case MSG_REGISTER: {
                            if (payload && strlen(payload) > 0) {
                                char old_nick[32];
                                strncpy(old_nick, g_clients[i].nickname, sizeof(old_nick) - 1);
                                strncpy(g_clients[i].nickname, payload, sizeof(g_clients[i].nickname) - 1);
                                g_clients[i].nickname[sizeof(g_clients[i].nickname) - 1] = '\0';
                                
                                char notify[128];
                                snprintf(notify, sizeof(notify), "[*] %s is now known as %s", old_nick, g_clients[i].nickname);
                                broadcast_from_parent(MSG_SERVER_NOTIFY, notify, strlen(notify), -1);
                            }
                            break;
                        }
                        case MSG_BROADCAST: {
                            if (payload) {
                                char formatted[BUFFER_SIZE];
                                snprintf(formatted, sizeof(formatted), "[%s]: %s", g_clients[i].nickname, payload);
                                LOG_DEBUG("Broadcast from %s: %s", g_clients[i].nickname, payload);
                                broadcast_from_parent(MSG_BROADCAST, formatted, strlen(formatted), ipc_fd);
                            }
                            break;
                        }
                        case MSG_WHISPER: {
                            // Format: target_name:content
                            if (payload) {
                                char *colon = strchr(payload, ':');
                                if (colon) {
                                    *colon = '\0';
                                    char *target = payload;
                                    char *msg = colon + 1;
                                    int found = 0;
                                    char formatted[BUFFER_SIZE];
                                    snprintf(formatted, sizeof(formatted), "[Whisper from %s]: %s", g_clients[i].nickname, msg);

                                    for (int j = 0; j < MAX_CLIENTS; j++) {
                                        if (g_clients[j].ipc_fd != -1 && strcmp(g_clients[j].nickname, target) == 0) {
                                            send_packet(g_clients[j].ipc_fd, MSG_WHISPER, formatted, strlen(formatted));
                                            found = 1;
                                            break;
                                        }
                                    }
                                    if (!found) {
                                        char err[128];
                                        snprintf(err, sizeof(err), "[!] User '%s' not found.", target);
                                        send_packet(ipc_fd, MSG_SERVER_NOTIFY, err, strlen(err));
                                    } else {
                                        char ack[128];
                                        snprintf(ack, sizeof(ack), "[Whisper to %s]: %s", target, msg);
                                        send_packet(ipc_fd, MSG_WHISPER, ack, strlen(ack));
                                    }
                                }
                            }
                            break;
                        }
                        case MSG_USER_LIST: {
                            char list_buf[BUFFER_SIZE];
                            int offset = snprintf(list_buf, sizeof(list_buf), "--- Online Users (%d) ---\n", g_client_count);
                            for (int j = 0; j < MAX_CLIENTS; j++) {
                                if (g_clients[j].ipc_fd != -1) {
                                    offset += snprintf(list_buf + offset, sizeof(list_buf) - offset,
                                                       " - %s (%s:%d)%s\n",
                                                       g_clients[j].nickname,
                                                       g_clients[j].ip_str,
                                                       g_clients[j].port,
                                                       (j == i) ? " [You]" : "");
                                    if ((size_t)offset >= sizeof(list_buf) - 64) break;
                                }
                            }
                            send_packet(ipc_fd, MSG_USER_LIST, list_buf, strlen(list_buf));
                            break;
                        }
                        case MSG_SYSINFO_REQ: {
                            char info_buf[BUFFER_SIZE];
                            get_system_info_string(info_buf, sizeof(info_buf), g_client_count);
                            send_packet(ipc_fd, MSG_SYSINFO_RESP, info_buf, strlen(info_buf));
                            break;
                        }
                        case MSG_LEAVE: {
                            LOG_INFO("Client %s requested leave", g_clients[i].nickname);
                            break;
                        }
                        default:
                            break;
                    }
                    if (payload) free(payload);
                }
            }
        }
    }

    LOG_INFO("Server shutting down, terminating child processes...");
    // Notify all workers and close
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].ipc_fd != -1) {
            close(g_clients[i].ipc_fd);
            kill(g_clients[i].pid, SIGTERM);
        }
    }
    close(listen_fd);
    // Harvest remaining children
    while (waitpid(-1, NULL, WNOHANG) > 0);

    LOG_INFO("Server shutdown complete.");
    return 0;
}

// Broadcasts a packet to all active clients (optionally excluding one)
static void broadcast_from_parent(uint16_t type, const char *payload, uint32_t len, int exclude_ipc_fd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].ipc_fd != -1 && g_clients[i].ipc_fd != exclude_ipc_fd) {
            send_packet(g_clients[i].ipc_fd, type, payload, len);
        }
    }
}

static void remove_client_by_ipc_fd(int ipc_fd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].ipc_fd == ipc_fd) {
            close(g_clients[i].ipc_fd);
            g_clients[i].ipc_fd = -1;
            g_clients[i].pid = 0;
            g_clients[i].nickname[0] = '\0';
            g_client_count--;
            break;
        }
    }
}

// Worker process handling a single client connection
static void run_worker(int client_tcp_fd, int parent_ipc_fd, const char *client_ip, int client_port) {
    LOG_INFO("Worker PID %d active for client %s:%d", getpid(), client_ip, client_port);

    // Send welcome message
    const char *welcome = "Welcome to Simple Chat Room!\nType /help for command list.\n";
    send_packet(client_tcp_fd, MSG_SERVER_NOTIFY, welcome, strlen(welcome));

    int running = 1;
    while (running) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(client_tcp_fd, &read_fds);
        FD_SET(parent_ipc_fd, &read_fds);
        int max_fd = (client_tcp_fd > parent_ipc_fd) ? client_tcp_fd : parent_ipc_fd;

        int ret = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        if (ret < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("Worker select() error: %s", strerror(errno));
            break;
        }

        // 1. Data from Client TCP Socket
        if (FD_ISSET(client_tcp_fd, &read_fds)) {
            PacketHeader hdr;
            char *payload = NULL;
            int r = recv_packet(client_tcp_fd, &hdr, &payload);
            if (r <= 0) {
                // Client closed socket or error
                LOG_INFO("Client %s:%d disconnected (EOF / Error)", client_ip, client_port);
                running = 0;
            } else {
                // Forward packet to Parent via IPC
                if (send_packet(parent_ipc_fd, hdr.msg_type, payload, hdr.body_len) < 0) {
                    LOG_ERROR("Worker failed to forward to parent IPC: %s", strerror(errno));
                    running = 0;
                }
                if (hdr.msg_type == MSG_LEAVE) {
                    running = 0;
                }
                if (payload) free(payload);
            }
        }

        // 2. Data from Parent IPC Socket
        if (FD_ISSET(parent_ipc_fd, &read_fds)) {
            PacketHeader hdr;
            char *payload = NULL;
            int r = recv_packet(parent_ipc_fd, &hdr, &payload);
            if (r <= 0) {
                // Parent closed IPC (Server shutting down)
                LOG_INFO("Parent IPC closed, worker PID %d exiting", getpid());
                running = 0;
            } else {
                // Forward packet to Client TCP Socket
                if (send_packet(client_tcp_fd, hdr.msg_type, payload, hdr.body_len) < 0) {
                    LOG_WARN("Failed to send to client TCP: %s", strerror(errno));
                    running = 0;
                }
                if (payload) free(payload);
            }
        }
    }

    close(client_tcp_fd);
    close(parent_ipc_fd);
    LOG_INFO("Worker PID %d terminated cleanly", getpid());
}
