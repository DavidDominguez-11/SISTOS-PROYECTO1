#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  framing.h  –  TCP framing helpers
//
//  Wire format for every message:
//    [1 byte  TYPE  ] uint8_t
//    [4 bytes LENGTH] uint32_t  big-endian  (htonl / ntohl)
//    [LENGTH  bytes ] serialised protobuf payload
// ─────────────────────────────────────────────────────────────────────────────

#include <cstring>
#include <string>
#include <cstdint>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <BaseTsd.h>
using ssize_t = SSIZE_T;
using socklen_t = int;
using socket_t = SOCKET;
constexpr socket_t INVALID_SOCKET_FD = INVALID_SOCKET;
inline int closeSocket(socket_t s) { return ::closesocket(s); }
constexpr int SEND_FLAGS = 0;
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t INVALID_SOCKET_FD = -1;
inline int closeSocket(socket_t s) { return ::close(s); }
constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#endif
#include <google/protobuf/message_lite.h>

// ── Message-type constants ────────────────────────────────────────────────────
// Client → Server
constexpr uint8_t TYPE_REGISTER         = 1;
constexpr uint8_t TYPE_MESSAGE_GENERAL  = 2;
constexpr uint8_t TYPE_MESSAGE_DM       = 3;
constexpr uint8_t TYPE_CHANGE_STATUS    = 4;
constexpr uint8_t TYPE_LIST_USERS       = 5;
constexpr uint8_t TYPE_GET_USER_INFO    = 6;
constexpr uint8_t TYPE_QUIT             = 7;

// Server → Client
constexpr uint8_t TYPE_SERVER_RESPONSE       = 10;
constexpr uint8_t TYPE_ALL_USERS             = 11;
constexpr uint8_t TYPE_FOR_DM                = 12;
constexpr uint8_t TYPE_BROADCAST_DELIVERY    = 13;
constexpr uint8_t TYPE_GET_USER_INFO_RESPONSE = 14;

// ── Helpers ───────────────────────────────────────────────────────────────────

/// Read exactly `n` bytes from `fd` into `buf`.  Returns false on EOF/error.
inline bool recvExact(socket_t fd, char* buf, std::size_t n)
{
    std::size_t received = 0;
    while (received < n) {
        ssize_t r = recv(fd, buf + received, n - received, 0);
        if (r <= 0) return false;
        received += static_cast<std::size_t>(r);
    }
    return true;
}

/// Serialize `msg` and send it as a framed packet.  Returns false on error.
inline bool sendFramed(socket_t fd, uint8_t type,
                       const google::protobuf::MessageLite& msg)
{
    // 1. Serialize
    std::string payload;
    if (!msg.SerializeToString(&payload)) return false;

    // 2. Build header: [type:1][length:4]
    const auto payloadLen = static_cast<uint32_t>(payload.size());
    const uint32_t netLen = htonl(payloadLen);

    std::string frame;
    frame.resize(5 + payloadLen);
    frame[0] = static_cast<char>(type);
    std::memcpy(&frame[1], &netLen, 4);
    if (payloadLen > 0)
        std::memcpy(&frame[5], payload.data(), payloadLen);

    // 3. Send in a loop (handles partial writes)
    const std::size_t total = frame.size();
    std::size_t sent = 0;
    while (sent < total) {
        ssize_t n = send(fd, frame.data() + sent, total - sent, SEND_FLAGS);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

/// Read one complete framed message.
/// On success sets `type`, fills `payload`, returns true.
inline bool recvFramed(socket_t fd, uint8_t& type, std::string& payload)
{
    // 1. Read 5-byte header
    char header[5];
    if (!recvExact(fd, header, 5)) return false;

    type = static_cast<uint8_t>(header[0]);
    const uint32_t length = ntohl(*reinterpret_cast<const uint32_t*>(header + 1));

    // 2. Read payload
    payload.resize(length);
    if (length > 0 && !recvExact(fd, &payload[0], length)) return false;

    return true;
}
