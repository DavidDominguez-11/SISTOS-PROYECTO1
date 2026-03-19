// ─────────────────────────────────────────────────────────────────────────────
//  server.cpp
//
//  Architecture:
//    • main thread  → accept() loop
//    • per-client   → std::thread (detached)
//
//  Shared state protected by clients_mutex.
// ─────────────────────────────────────────────────────────────────────────────

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "framing.h"
#include "proto/chat.pb.h"

using namespace chat;

// ── Shared data structure (spec §4.2) ─────────────────────────────────────────
struct ClientInfo {
    socket_t    socket;
    std::string username;
    std::string ip;
    StatusEnum  status;
};

static std::unordered_map<std::string, ClientInfo> clients;
static std::mutex clients_mutex;

// ── Utility: remove a registered client by username ──────────────────────────
static void removeClient(const std::string& username)
{
    std::lock_guard<std::mutex> lock(clients_mutex);
    auto it = clients.find(username);
    if (it != clients.end()) {
        closeSocket(it->second.socket);
        clients.erase(it);
        std::cout << "[Server] Removed client: " << username << "\n";
    }
}

// ── Utility: send to all connected clients ────────────────────────────────────
//  Caller MUST NOT hold clients_mutex when calling this.
static void broadcastAll(uint8_t type, const google::protobuf::MessageLite& msg)
{
    std::lock_guard<std::mutex> lock(clients_mutex);
    for (auto& [uname, info] : clients) {
        if (!sendFramed(info.socket, type, msg)) {
            // Non-fatal – the client thread will detect the broken socket.
        }
    }
}

// ── Per-client handler (runs in its own thread) ───────────────────────────────
static void handleClient(socket_t clientSock, std::string clientIp)
{
    std::cout << "[Server] Connection from " << clientIp
              << " (fd=" << clientSock << ")\n";

    std::string registeredUsername; // empty until REGISTER succeeds

    auto disconnectCleanup = [&]() {
        if (!registeredUsername.empty()) {
            removeClient(registeredUsername);
        } else {
            closeSocket(clientSock);
        }
    };

    while (true) {
        uint8_t     type;
        std::string payload;

        if (!recvFramed(clientSock, type, payload)) {
            // EOF or socket error → clean up
            std::cout << "[Server] Client disconnected: "
                      << (registeredUsername.empty() ? clientIp : registeredUsername)
                      << "\n";
            disconnectCleanup();
            return;
        }

        // ── Dispatch ─────────────────────────────────────────────────────────
        switch (type) {

        // ── 1: Register ──────────────────────────────────────────────────────
        case TYPE_REGISTER: {
            Register req;
            if (!req.ParseFromString(payload)) {
                std::cerr << "[Server] Failed to parse Register\n";
                break;
            }

            const std::string& uname = req.username();
            ServerResponse resp;

            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                if (clients.count(uname)) {
                    resp.set_status_code(409);
                    resp.set_message("Username '" + uname + "' is already taken.");
                    resp.set_is_successful(false);
                } else {
                    ClientInfo info;
                    info.socket   = clientSock;
                    info.username = uname;
                    info.ip       = req.ip();
                    info.status   = StatusEnum::ACTIVE;
                    clients[uname]     = info;
                    registeredUsername = uname;

                    resp.set_status_code(200);
                    resp.set_message("Welcome, " + uname + "!");
                    resp.set_is_successful(true);
                    std::cout << "[Server] Registered: " << uname
                              << " (" << req.ip() << ")\n";
                }
            }
            sendFramed(clientSock, TYPE_SERVER_RESPONSE, resp);
            break;
        }

        // ── 2: MessageGeneral → BroadcastDelivery to all ─────────────────────
        case TYPE_MESSAGE_GENERAL: {
            MessageGeneral req;
            if (!req.ParseFromString(payload)) {
                std::cerr << "[Server] Failed to parse MessageGeneral\n";
                break;
            }

            BroadcastDelivery bd;
            bd.set_message(req.message());
            bd.set_username_origin(req.username_origin());
            broadcastAll(TYPE_BROADCAST_DELIVERY, bd);
            break;
        }

        // ── 3: MessageDM → ForDm to target ────────────────────────────────────
        case TYPE_MESSAGE_DM: {
            MessageDM req;
            if (!req.ParseFromString(payload)) {
                std::cerr << "[Server] Failed to parse MessageDM\n";
                break;
            }

            std::lock_guard<std::mutex> lock(clients_mutex);
            auto it = clients.find(req.username_des());
            if (it != clients.end()) {
                ForDm fdm;
                fdm.set_username_des(registeredUsername); // sender's name
                fdm.set_message(req.message());
                sendFramed(it->second.socket, TYPE_FOR_DM, fdm);
            } else {
                ServerResponse resp;
                resp.set_status_code(404);
                resp.set_message("User '" + req.username_des() + "' not found.");
                resp.set_is_successful(false);
                sendFramed(clientSock, TYPE_SERVER_RESPONSE, resp);
            }
            break;
        }

        // ── 4: ChangeStatus ───────────────────────────────────────────────────
        case TYPE_CHANGE_STATUS: {
            ChangeStatus req;
            if (!req.ParseFromString(payload)) {
                std::cerr << "[Server] Failed to parse ChangeStatus\n";
                break;
            }

            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                auto it = clients.find(req.username());
                if (it != clients.end()) {
                    it->second.status = req.status();
                    std::cout << "[Server] " << req.username()
                              << " changed status to " << req.status() << "\n";
                }
            }

            ServerResponse resp;
            resp.set_status_code(200);
            resp.set_message("Status updated.");
            resp.set_is_successful(true);
            sendFramed(clientSock, TYPE_SERVER_RESPONSE, resp);
            break;
        }

        // ── 5: ListUsers → AllUsers ───────────────────────────────────────────
        case TYPE_LIST_USERS: {
            ListUsers req;
            if (!req.ParseFromString(payload)) {
                std::cerr << "[Server] Failed to parse ListUsers\n";
                break;
            }

            AllUsers au;
            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                for (auto& [uname, info] : clients) {
                    au.add_usernames(uname);
                    au.add_status(info.status);
                }
            }
            sendFramed(clientSock, TYPE_ALL_USERS, au);
            break;
        }

        // ── 6: GetUserInfo → GetUserInfoResponse ──────────────────────────────
        case TYPE_GET_USER_INFO: {
            GetUserInfo req;
            if (!req.ParseFromString(payload)) {
                std::cerr << "[Server] Failed to parse GetUserInfo\n";
                break;
            }

            std::lock_guard<std::mutex> lock(clients_mutex);
            auto it = clients.find(req.username_des());
            if (it != clients.end()) {
                GetUserInfoResponse resp;
                resp.set_ip_address(it->second.ip);
                resp.set_username(it->second.username);
                resp.set_status(it->second.status);
                sendFramed(clientSock, TYPE_GET_USER_INFO_RESPONSE, resp);
            } else {
                ServerResponse resp;
                resp.set_status_code(404);
                resp.set_message("User '" + req.username_des() + "' not found.");
                resp.set_is_successful(false);
                sendFramed(clientSock, TYPE_SERVER_RESPONSE, resp);
            }
            break;
        }

        // ── 7: Quit ───────────────────────────────────────────────────────────
        case TYPE_QUIT: {
            Quit req;
            if (!req.ParseFromString(payload)) {
                std::cerr << "[Server] Failed to parse Quit\n";
            }
            std::cout << "[Server] Client quit: "
                      << (registeredUsername.empty() ? clientIp : registeredUsername)
                      << "\n";
            disconnectCleanup();
            return;
        }

        default:
            std::cerr << "[Server] Unknown message type: "
                      << static_cast<int>(type) << " – discarded\n";
            break;
        }
    }
}

// ── main ──────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
    // Verify that the Protobuf library version matches the headers.
    GOOGLE_PROTOBUF_VERIFY_VERSION;

#ifdef _WIN32
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
#endif

    int port = 8080;
    if (argc > 1) {
        try { port = std::stoi(argv[1]); }
        catch (...) { std::cerr << "Invalid port\n"; return 1; }
    }

    // ── Create listening socket ───────────────────────────────────────────────
    socket_t serverSock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (serverSock == INVALID_SOCKET_FD) {
        std::perror("socket");
        return 1;
    }

    int opt = 1;
    ::setsockopt(serverSock, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<uint16_t>(port));

    if (::bind(serverSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        return 1;
    }
    if (::listen(serverSock, 16) < 0) {
        std::perror("listen");
        return 1;
    }

    std::cout << "[Server] Listening on port " << port << " …\n";

    // ── Accept loop ───────────────────────────────────────────────────────────
    while (true) {
        sockaddr_in clientAddr{};
        socklen_t   clientLen = sizeof(clientAddr);

        socket_t clientSock = ::accept(serverSock,
                                       reinterpret_cast<sockaddr*>(&clientAddr),
                                       &clientLen);
        if (clientSock == INVALID_SOCKET_FD) {
            std::perror("[Server] accept");
            continue;
        }

        std::string clientIp = ::inet_ntoa(clientAddr.sin_addr);

        // Spawn a detached thread for this client.
        std::thread(handleClient, clientSock, std::move(clientIp)).detach();
    }

    closeSocket(serverSock);
#ifdef _WIN32
    WSACleanup();
#endif
    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}
