#include "protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>

int send_all(int fd, const void *buf, size_t len) {
    const char *ptr = (const char *)buf;
    size_t total_sent = 0;

    while (total_sent < len) {
        ssize_t sent = write(fd, ptr + total_sent, len - total_sent);
        if (sent < 0) {
            if (errno == EINTR) {
                continue; // Interrupted by signal, retry
            }
            return -1; // Write error (e.g. EPIPE, ECONNRESET)
        }
        if (sent == 0) {
            return -1; // Unexpected disconnect
        }
        total_sent += (size_t)sent;
    }
    return 0;
}

int recv_all(int fd, void *buf, size_t len) {
    char *ptr = (char *)buf;
    size_t total_recv = 0;

    while (total_recv < len) {
        ssize_t n = read(fd, ptr + total_recv, len - total_recv);
        if (n < 0) {
            if (errno == EINTR) {
                continue; // Interrupted by signal, retry
            }
            return -1; // Read error
        }
        if (n == 0) {
            // Peer closed connection
            return (total_recv == 0) ? 0 : -1;
        }
        total_recv += (size_t)n;
    }
    return 1;
}

int send_packet(int fd, uint16_t type, const char *payload, uint32_t len) {
    PacketHeader hdr;
    hdr.magic = htonl(PROTO_MAGIC);
    hdr.msg_type = htons(type);
    hdr.reserved = 0;
    hdr.body_len = htonl(len);

    // Send header first
    if (send_all(fd, &hdr, sizeof(hdr)) < 0) {
        return -1;
    }

    // Send payload if any
    if (len > 0 && payload != NULL) {
        if (send_all(fd, payload, len) < 0) {
            return -1;
        }
    }

    return 0;
}

int recv_packet(int fd, PacketHeader *header, char **payload_out) {
    PacketHeader raw_hdr;
    int ret = recv_all(fd, &raw_hdr, sizeof(raw_hdr));
    if (ret <= 0) {
        return ret; // 0 for EOF, -1 for error
    }

    header->magic = ntohl(raw_hdr.magic);
    header->msg_type = ntohs(raw_hdr.msg_type);
    header->reserved = ntohs(raw_hdr.reserved);
    header->body_len = ntohl(raw_hdr.body_len);

    // Validate magic
    if (header->magic != PROTO_MAGIC) {
        return -1;
    }

    // Validate body length to avoid huge allocations
    if (header->body_len > MAX_PAYLOAD_LEN) {
        return -1;
    }

    if (payload_out == NULL) {
        return 1;
    }

    // Allocate memory with null-terminator for string safety
    char *body = (char *)malloc(header->body_len + 1);
    if (!body) {
        return -1;
    }

    if (header->body_len > 0) {
        int r = recv_all(fd, body, header->body_len);
        if (r <= 0) {
            free(body);
            *payload_out = NULL;
            return -1;
        }
    }
    body[header->body_len] = '\0';
    *payload_out = body;

    return 1;
}
