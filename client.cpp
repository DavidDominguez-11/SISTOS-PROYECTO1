// ─────────────────────────────────────────────────────────────────────────────
//  client.cpp
//
//  Architecture:
//    • inputThread  – reads stdin, parses commands, sends framed protobuf
//    • recvThread   – reads framed messages, prints formatted output
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
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "framing.h"
#include "proto/chat.pb.h"

using namespace chat;

// ── Shared state ──────────────────────────────────────────────────────────────
static socket_t          gSock    = INVALID_SOCKET_FD;
static std::mutex        gSockMutex; // Protects socket for multi-threaded sends

static std::string       gUsername;
static std::string       gIp;
static std::atomic<bool> gRunning {true};

// Status and activity tracking
static std::mutex                             gStatusMutex;
static StatusEnum                             gCurrentStatus = StatusEnum::ACTIVE;
static std::chrono::steady_clock::time_point  gLastActivityTime;

// ── Helper: obtain this machine's outward-facing IP ──────────────────────────
static std::string getLocalIp()
{
    socket_t s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET_FD) return "127.0.0.1";

    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port   = htons(80);
    ::inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);

    if (::connect(s, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) < 0) {
        closeSocket(s);
        return "127.0.0.1";
    }

    sockaddr_in local{};
    socklen_t   len = sizeof(local);
    ::getsockname(s, reinterpret_cast<sockaddr*>(&local), &len);
    closeSocket(s);
    return ::inet_ntoa(local.sin_addr);
}

// ── Helper: status string ─────────────────────────────────────────────────────
static std::string statusStr(StatusEnum s)
{
    switch (s) {
    case StatusEnum::ACTIVE:         return "ACTIVE";
    case StatusEnum::DO_NOT_DISTURB: return "DO_NOT_DISTURB";
    case StatusEnum::INVISIBLE:      return "INVISIBLE";
    default:                         return "UNKNOWN";
    }
}

// ── Helper: send framed message (thread-safe) ─────────────────────────────────
static bool sendFramedSafe(uint8_t type, const google::protobuf::MessageLite& msg)
{
    std::lock_guard<std::mutex> lock(gSockMutex);
    return sendFramed(gSock, type, msg);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Inactivity Watchdog
// ─────────────────────────────────────────────────────────────────────────────
static void watchdogLoop()
{
    while (gRunning) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(gStatusMutex);
        
        if (gCurrentStatus == StatusEnum::ACTIVE) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - gLastActivityTime).count();
            if (elapsed >= 60) {
                gCurrentStatus = StatusEnum::INVISIBLE;
                
                ChangeStatus req;
                req.set_status(gCurrentStatus);
                req.set_username(gUsername);
                req.set_ip(gIp);
                
                if (sendFramedSafe(TYPE_CHANGE_STATUS, req)) {
                    std::cout << "\n[Client] Marked as INVISIBLE due to 60s of inactivity.\n> " << std::flush;
                }
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Receive thread
// ─────────────────────────────────────────────────────────────────────────────
static void receiveLoop()
{
    while (gRunning) {
        uint8_t     type;
        std::string payload;

        if (!recvFramed(gSock, type, payload)) {
            if (gRunning) {
                std::cout << "\n[Client] Lost connection to server.\n";
                gRunning = false;
            }
            return;
        }

        switch (type) {

        case TYPE_SERVER_RESPONSE: {
            ServerResponse resp;
            if (!resp.ParseFromString(payload)) {
                std::cerr << "[Client] Failed to parse ServerResponse\n";
                break;
            }
            std::cout << "[Server]: " << resp.message()
                      << "  (code=" << resp.status_code() << ")\n";
            break;
        }

        case TYPE_ALL_USERS: {
            AllUsers au;
            if (!au.ParseFromString(payload)) {
                std::cerr << "[Client] Failed to parse AllUsers\n";
                break;
            }
            std::cout << "[Server]: Online users (" << au.usernames_size() << "):\n";
            for (int i = 0; i < au.usernames_size(); ++i) {
                StatusEnum st = (i < au.status_size())
                                    ? au.status(i)
                                    : StatusEnum::ACTIVE;
                std::cout << "  " << au.usernames(i)
                          << "  [" << statusStr(st) << "]\n";
            }
            break;
        }

        case TYPE_FOR_DM: {
            ForDm fdm;
            if (!fdm.ParseFromString(payload)) {
                std::cerr << "[Client] Failed to parse ForDm\n";
                break;
            }
            std::cout << "[DM] " << fdm.username_des() << ": "
                      << fdm.message() << "\n";
            break;
        }

        case TYPE_BROADCAST_DELIVERY: {
            BroadcastDelivery bd;
            if (!bd.ParseFromString(payload)) {
                std::cerr << "[Client] Failed to parse BroadcastDelivery\n";
                break;
            }
            std::cout << "[Broadcast] " << bd.username_origin() << ": "
                      << bd.message() << "\n";
            break;
        }

        case TYPE_GET_USER_INFO_RESPONSE: {
            GetUserInfoResponse resp;
            if (!resp.ParseFromString(payload)) {
                std::cerr << "[Client] Failed to parse GetUserInfoResponse\n";
                break;
            }
            std::cout << "[Server]: User info\n"
                      << "  username : " << resp.username()       << "\n"
                      << "  ip       : " << resp.ip_address()     << "\n"
                      << "  status   : " << statusStr(resp.status()) << "\n";
            break;
        }

        default:
            std::cerr << "[Client] Unknown message type: "
                      << static_cast<int>(type) << " – discarded\n";
            break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Input thread
// ─────────────────────────────────────────────────────────────────────────────
static void inputLoop()
{
    auto usage = [](const char* msg) { std::cout << "[Usage] " << msg << "\n"; };

    std::string line;
    while (gRunning) {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line)) break;

        if (!gRunning) break;
        
        // Update activity
        {
            std::lock_guard<std::mutex> lock(gStatusMutex);
            gLastActivityTime = std::chrono::steady_clock::now();
        }

        if (line.empty()) continue;

        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        // ── /all <message> ───────────────────────────────────────────────────
        if (cmd == "/all") {
            std::string msg;
            std::getline(iss, msg);
            // trim leading space
            if (!msg.empty() && msg.front() == ' ') msg.erase(0, 1);
            if (msg.empty()) { usage("/all <message>"); continue; }

            MessageGeneral req;
            req.set_message(msg);
            req.set_status(StatusEnum::ACTIVE);
            req.set_username_origin(gUsername);
            req.set_ip(gIp);
            if (!sendFramedSafe(TYPE_MESSAGE_GENERAL, req)) {
                std::cerr << "[Client] Send error on /all\n";
            }

        // ── /dm <username> <message> ─────────────────────────────────────────
        } else if (cmd == "/dm") {
            std::string target;
            if (!(iss >> target)) { usage("/dm <username> <message>"); continue; }
            std::string msg;
            std::getline(iss, msg);
            if (!msg.empty() && msg.front() == ' ') msg.erase(0, 1);
            if (msg.empty()) { usage("/dm <username> <message>"); continue; }

            MessageDM req;
            req.set_message(msg);
            req.set_status(StatusEnum::ACTIVE);
            req.set_username_des(target);
            req.set_ip(gIp);
            if (!sendFramedSafe(TYPE_MESSAGE_DM, req)) {
                std::cerr << "[Client] Send error on /dm\n";
            }

        // ── /list ────────────────────────────────────────────────────────────
        } else if (cmd == "/list") {
            ListUsers req;
            req.set_username(gUsername);
            req.set_ip(gIp);
            if (!sendFramedSafe(TYPE_LIST_USERS, req)) {
                std::cerr << "[Client] Send error on /list\n";
            }

        // ── /info <username> ─────────────────────────────────────────────────
        } else if (cmd == "/info") {
            std::string target;
            if (!(iss >> target)) { usage("/info <username>"); continue; }

            GetUserInfo req;
            req.set_username_des(target);
            req.set_username(gUsername);
            req.set_ip(gIp);
            if (!sendFramedSafe(TYPE_GET_USER_INFO, req)) {
                std::cerr << "[Client] Send error on /info\n";
            }

        // ── /status <ACTIVE|DO_NOT_DISTURB|INVISIBLE> ────────────────────────
        } else if (cmd == "/status") {
            std::string statusArg;
            if (!(iss >> statusArg)) {
                usage("/status <ACTIVE|DO_NOT_DISTURB|INVISIBLE>");
                continue;
            }

            StatusEnum st;
            if      (statusArg == "ACTIVE")          st = StatusEnum::ACTIVE;
            else if (statusArg == "DO_NOT_DISTURB")  st = StatusEnum::DO_NOT_DISTURB;
            else if (statusArg == "INVISIBLE")        st = StatusEnum::INVISIBLE;
            else {
                usage("/status <ACTIVE|DO_NOT_DISTURB|INVISIBLE>");
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(gStatusMutex);
                gCurrentStatus = st;
            }

            ChangeStatus req;
            req.set_status(st);
            req.set_username(gUsername);
            req.set_ip(gIp);
            if (!sendFramedSafe(TYPE_CHANGE_STATUS, req)) {
                std::cerr << "[Client] Send error on /status\n";
            }

        // ── /quit ────────────────────────────────────────────────────────────
        } else if (cmd == "/quit") {
            Quit req;
            req.set_quit(true);
            req.set_ip(gIp);
            sendFramedSafe(TYPE_QUIT, req);   // best-effort
            gRunning = false;
            break;

        // ── unknown ──────────────────────────────────────────────────────────
        } else {
            std::cout << "[Client] Unknown command.  Available commands:\n"
                      << "  /all <message>\n"
                      << "  /dm <username> <message>\n"
                      << "  /list\n"
                      << "  /info <username>\n"
                      << "  /status <ACTIVE|DO_NOT_DISTURB|INVISIBLE>\n"
                      << "  /quit\n";
        }
    }
    gRunning = false;
}

// ── main ──────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
    GOOGLE_PROTOBUF_VERIFY_VERSION;

#ifdef _WIN32
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
#endif

    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <username> <host> <port>\n";
        return 1;
    }

    gUsername = argv[1];
    std::string host = argv[2];
    int         port = 8080;

    try { port = std::stoi(argv[3]); }
    catch (...) { std::cerr << "Invalid port\n"; return 1; }

    // ── Connect ───────────────────────────────────────────────────────────────
    gSock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (gSock == INVALID_SOCKET_FD) { std::perror("socket"); return 1; }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port   = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &serverAddr.sin_addr) <= 0) {
        std::cerr << "Invalid address: " << host << "\n";
        return 1;
    }

    if (::connect(gSock, reinterpret_cast<sockaddr*>(&serverAddr),
                  sizeof(serverAddr)) < 0) {
        std::perror("connect");
        return 1;
    }

    gIp = getLocalIp();

    // Initialize activity
    gLastActivityTime = std::chrono::steady_clock::now();

    // ── Automatic Registration ────────────────────────────────────────────────
    Register regReq;
    regReq.set_username(gUsername);
    regReq.set_ip(gIp);
    if (!sendFramedSafe(TYPE_REGISTER, regReq)) {
        std::cerr << "[Client] Failed to send automatic registration\n";
        closeSocket(gSock);
        return 1;
    }

    std::cout << "╔══════════════════════════════════════════╗\n"
              << "║        C++ Protobuf Chat Client          ║\n"
              << "╚══════════════════════════════════════════╝\n"
              << "  Connected to " << host << ":" << port << "\n"
              << "  Username     : " << gUsername << "\n"
              << "  Your IP      : " << gIp << "\n\n"
              << "  Commands:\n"
              << "    /all <message>\n"
              << "    /dm <username> <message>\n"
              << "    /list\n"
              << "    /info <username>\n"
              << "    /status <ACTIVE|DO_NOT_DISTURB|INVISIBLE>\n"
              << "    /quit\n\n";

    // ── Start threads ─────────────────────────────────────────────────────────
    std::thread recvThread(receiveLoop);
    std::thread watchdogThread(watchdogLoop);

    // ── Input loop runs on main thread ────────────────────────────────────────
    inputLoop();

    // ── Shutdown ──────────────────────────────────────────────────────────────
    gRunning = false;
    // Unblock the recv thread if it is waiting on recv()
#ifdef _WIN32
    ::shutdown(gSock, SD_BOTH);
#else
    ::shutdown(gSock, SHUT_RDWR);
#endif
    closeSocket(gSock);

    if (recvThread.joinable()) recvThread.join();
    if (watchdogThread.joinable()) watchdogThread.join();

    std::cout << "[Client] Goodbye.\n";
    google::protobuf::ShutdownProtobufLibrary();
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
