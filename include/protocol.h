#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#define PROTO_MAGIC 0x43484154  // ASCII for 'CHAT'

// Message Types
typedef enum {
    MSG_REGISTER       = 1, // Client registers nickname
    MSG_BROADCAST      = 2, // Public message to all users
    MSG_WHISPER        = 3, // Private message: target:content
    MSG_USER_LIST      = 4, // Request / Response user list
    MSG_SYSINFO_REQ    = 5, // Client requests server system info
    MSG_SYSINFO_RESP   = 6, // Server replies with system info
    MSG_SERVER_NOTIFY  = 7, // Server notification (join/leave/error)
    MSG_LEAVE          = 8  // Client leaves gracefully
} MsgType;

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;      // PROTO_MAGIC
    uint16_t msg_type;   // MsgType
    uint16_t reserved;   // Reserved for alignment / future flags
    uint32_t body_len;   // Payload length in bytes
} PacketHeader;
#pragma pack(pop)

#define MAX_PAYLOAD_LEN (64 * 1024) // 64 KB max payload size

// Reliable low-level I/O helpers
int send_all(int fd, const void *buf, size_t len);
int recv_all(int fd, void *buf, size_t len);

// Packet level helpers
int send_packet(int fd, uint16_t type, const char *payload, uint32_t len);

/**
 * recv_packet:
 * Reads a packet header and dynamically allocates *payload_out buffer
 * (size: body_len + 1, guaranteed null-terminated).
 * 
 * Returns:
 *    1: Success (header populated, *payload_out allocated)
 *    0: Peer closed connection (EOF)
 *   -1: Error or invalid packet
 * 
 * Note: Caller is responsible for free(*payload_out) when return == 1.
 */
int recv_packet(int fd, PacketHeader *header, char **payload_out);

#endif // PROTOCOL_H
