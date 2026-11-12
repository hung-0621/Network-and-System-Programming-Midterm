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
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

#include "protocol.h"
#include "logger.h"

#define DEFAULT_PORT 8888
#define DEFAULT_HOST "127.0.0.1"
#define INPUT_BUFFER_SIZE 2048

static volatile sig_atomic_t g_client_running = 1;
static int g_server_fd = -1;

static void handle_sigint(int sig) {
    (void)sig;
    g_client_running = 0;
}

static void print_help(void) {
    printf("\n=== Available Commands ===\n");
    printf("  /help                : Show this help menu\n");
    printf("  /sysinfo             : Request system information from server\n");
    printf("  /list                : List online users\n");
    printf("  /nick <new_name>     : Change your nickname\n");
    printf("  /msg <user> <text>   : Send private message to user\n");
    printf("  /quit or /exit       : Leave the chat room\n");
    printf("  <text>               : Broadcast message to everyone\n");
    printf("==========================\n\n");
}

static void print_usage(const char *prog) {
    printf("Usage: %s [OPTIONS] [HOST] [PORT]\n", prog);
    printf("Options:\n");
    printf("  -u, --user <nickname>      Nickname to use (default: User_<pid>)\n");
    printf("  -d, --debug <level>        Debug log level (0:ERROR, 1:WARN, 2:INFO, 3:DEBUG)\n");
    printf("  -h, --help                 Show this help message\n");
}

int main(int argc, char *argv[]) {
    char host[128] = DEFAULT_HOST;
    int port = DEFAULT_PORT;
    char nickname[32];
    snprintf(nickname, sizeof(nickname), "User_%d", getpid());
    int debug_lvl = LOG_LVL_INFO;

    static struct option long_options[] = {
        {"user",  required_argument, 0, 'u'},
        {"debug", required_argument, 0, 'd'},
        {"help",  no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "u:d:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'u':
                strncpy(nickname, optarg, sizeof(nickname) - 1);
                nickname[sizeof(nickname) - 1] = '\0';
                break;
            case 'd':
                debug_lvl = atoi(optarg);
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    if (optind < argc) {
        strncpy(host, argv[optind], sizeof(host) - 1);
        optind++;
    }
    if (optind < argc) {
        port = atoi(argv[optind]);
        optind++;
    }

    log_set_level((LogLevel)debug_lvl);

    // Setup signal handling
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN); // Ignore broken pipe

    // Create TCP socket
    g_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_fd < 0) {
        LOG_ERROR("Failed to create socket: %s", strerror(errno));
        return 1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, host, &serv_addr.sin_addr) <= 0) {
        LOG_ERROR("Invalid host address: %s", host);
        close(g_server_fd);
        return 1;
    }

    LOG_INFO("Connecting to server at %s:%d as '%s'...", host, port, nickname);
    if (connect(g_server_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        LOG_ERROR("connect() failed: %s", strerror(errno));
        close(g_server_fd);
        return 1;
    }

    LOG_INFO("Connected successfully!");

    // Register nickname
    if (send_packet(g_server_fd, MSG_REGISTER, nickname, strlen(nickname)) < 0) {
        LOG_ERROR("Failed to register nickname: %s", strerror(errno));
        close(g_server_fd);
        return 1;
    }

    print_help();
    printf("\n[%s] > ", nickname);
    fflush(stdout);

    // Main event loop with select() multiplexing stdin and socket
    while (g_client_running) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);
        FD_SET(g_server_fd, &read_fds);
        int max_fd = (STDIN_FILENO > g_server_fd) ? STDIN_FILENO : g_server_fd;

        int ret = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG_ERROR("select() error: %s", strerror(errno));
            break;
        }

        // 1. Incoming message from Server
        if (FD_ISSET(g_server_fd, &read_fds)) {
            PacketHeader hdr;
            char *payload = NULL;
            int r = recv_packet(g_server_fd, &hdr, &payload);
            if (r <= 0) {
                printf("\n\033[1;31m[!] Server closed connection.\033[0m\n");
                if (payload) free(payload);
                break;
            }

            // Print incoming packet cleanly
            printf("\r\033[K"); // Clear current prompt line
            switch (hdr.msg_type) {
                case MSG_BROADCAST:
                    printf("\033[1;37m%s\033[0m\n", payload ? payload : "");
                    break;
                case MSG_WHISPER:
                    printf("\033[1;35m%s\033[0m\n", payload ? payload : "");
                    break;
                case MSG_SERVER_NOTIFY:
                    printf("\033[1;33m%s\033[0m\n", payload ? payload : "");
                    break;
                case MSG_SYSINFO_RESP:
                    printf("\033[1;36m%s\033[0m\n", payload ? payload : "");
                    break;
                case MSG_USER_LIST:
                    printf("\033[1;32m%s\033[0m\n", payload ? payload : "");
                    break;
                default:
                    printf("%s\n", payload ? payload : "");
                    break;
            }
            if (payload) free(payload);

            printf("[%s] > ", nickname);
            fflush(stdout);
        }

        // 2. User input from STDIN
        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            char line[INPUT_BUFFER_SIZE];
            if (fgets(line, sizeof(line), stdin) == NULL) {
                // EOF on stdin (Ctrl+D)
                break;
            }

            // Remove trailing newline
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                line[--len] = '\0';
            }

            if (len == 0) {
                printf("[%s] > ", nickname);
                fflush(stdout);
                continue;
            }

            // Parse commands
            if (strcmp(line, "/help") == 0) {
                print_help();
            } else if (strcmp(line, "/sysinfo") == 0) {
                LOG_DEBUG("Sending SYSINFO request...");
                send_packet(g_server_fd, MSG_SYSINFO_REQ, "", 0);
            } else if (strcmp(line, "/list") == 0 || strcmp(line, "/who") == 0) {
                LOG_DEBUG("Sending USER_LIST request...");
                send_packet(g_server_fd, MSG_USER_LIST, "", 0);
            } else if (strncmp(line, "/nick ", 6) == 0) {
                const char *new_nick = line + 6;
                while (*new_nick == ' ') new_nick++;
                if (strlen(new_nick) > 0) {
                    strncpy(nickname, new_nick, sizeof(nickname) - 1);
                    nickname[sizeof(nickname) - 1] = '\0';
                    send_packet(g_server_fd, MSG_REGISTER, nickname, strlen(nickname));
                }
            } else if (strncmp(line, "/msg ", 5) == 0 || strncmp(line, "/w ", 3) == 0) {
                char *args = (line[1] == 'm') ? (line + 5) : (line + 3);
                while (*args == ' ') args++;
                char *target = strtok(args, " ");
                char *msg = strtok(NULL, "");
                if (target && msg) {
                    while (*msg == ' ') msg++;
                    char whisper_buf[INPUT_BUFFER_SIZE];
                    snprintf(whisper_buf, sizeof(whisper_buf), "%s:%s", target, msg);
                    send_packet(g_server_fd, MSG_WHISPER, whisper_buf, strlen(whisper_buf));
                } else {
                    printf("\033[1;31mUsage: /msg <target_username> <message>\033[0m\n");
                }
            } else if (strcmp(line, "/quit") == 0 || strcmp(line, "/exit") == 0) {
                send_packet(g_server_fd, MSG_LEAVE, "", 0);
                break;
            } else {
                // Public broadcast message
                send_packet(g_server_fd, MSG_BROADCAST, line, strlen(line));
            }

            printf("[%s] > ", nickname);
            fflush(stdout);
        }
    }

    // Graceful exit
    LOG_INFO("Exiting chat room...");
    if (g_server_fd >= 0) {
        send_packet(g_server_fd, MSG_LEAVE, "", 0);
        close(g_server_fd);
    }

    return 0;
}
