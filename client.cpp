// ─────────────────────────────────────────────────────────────────────────────
//  client.cpp
//
//  Architecture:
//    • inputThread  – reads stdin, parses commands, sends framed protobuf
//    • recvThread   – reads framed messages, prints formatted output
// ─────────────────────────────────────────────────────────────────────────────

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "framing.h"
#include "proto/chat.pb.h"

using namespace chat;

// ── Shared state ──────────────────────────────────────────────────────────────
static int               gSock    = -1;
static std::string       gUsername;
static std::string       gIp;
static std::atomic<bool> gRunning {true};

// ── Helper: obtain this machine's outward-facing IP ──────────────────────────
static std::string getLocalIp()
{
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return "127.0.0.1";

    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port   = htons(80);
    ::inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);

    if (::connect(s, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) < 0) {
        ::close(s);
        return "127.0.0.1";
    }

    sockaddr_in local{};
    socklen_t   len = sizeof(local);
    ::getsockname(s, reinterpret_cast<sockaddr*>(&local), &len);
    ::close(s);
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
    while (gRunning && std::getline(std::cin, line)) {

        if (!gRunning) break;
        if (line.empty()) continue;

        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        // ── /register <username> ─────────────────────────────────────────────
        if (cmd == "/register") {
            std::string uname;
            if (!(iss >> uname)) { usage("/register <username>"); continue; }

            gUsername = uname;
            Register req;
            req.set_username(uname);
            req.set_ip(gIp);
            if (!sendFramed(gSock, TYPE_REGISTER, req)) {
                std::cerr << "[Client] Send error on /register\n";
            }

        // ── /all <message> ───────────────────────────────────────────────────
        } else if (cmd == "/all") {
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
            if (!sendFramed(gSock, TYPE_MESSAGE_GENERAL, req)) {
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
            if (!sendFramed(gSock, TYPE_MESSAGE_DM, req)) {
                std::cerr << "[Client] Send error on /dm\n";
            }

        // ── /list ────────────────────────────────────────────────────────────
        } else if (cmd == "/list") {
            ListUsers req;
            req.set_username(gUsername);
            req.set_ip(gIp);
            if (!sendFramed(gSock, TYPE_LIST_USERS, req)) {
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
            if (!sendFramed(gSock, TYPE_GET_USER_INFO, req)) {
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

            ChangeStatus req;
            req.set_status(st);
            req.set_username(gUsername);
            req.set_ip(gIp);
            if (!sendFramed(gSock, TYPE_CHANGE_STATUS, req)) {
                std::cerr << "[Client] Send error on /status\n";
            }

        // ── /quit ────────────────────────────────────────────────────────────
        } else if (cmd == "/quit") {
            Quit req;
            req.set_quit(true);
            req.set_ip(gIp);
            sendFramed(gSock, TYPE_QUIT, req);   // best-effort
            gRunning = false;
            break;

        // ── unknown ──────────────────────────────────────────────────────────
        } else {
            std::cout << "[Client] Unknown command.  Available commands:\n"
                      << "  /register <username>\n"
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

    std::string host = "127.0.0.1";
    int         port = 8080;

    if (argc > 1) host = argv[1];
    if (argc > 2) {
        try { port = std::stoi(argv[2]); }
        catch (...) { std::cerr << "Invalid port\n"; return 1; }
    }

    // ── Connect ───────────────────────────────────────────────────────────────
    gSock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (gSock < 0) { std::perror("socket"); return 1; }

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

    std::cout << "╔══════════════════════════════════════════╗\n"
              << "║        C++ Protobuf Chat Client          ║\n"
              << "╚══════════════════════════════════════════╝\n"
              << "  Connected to " << host << ":" << port << "\n"
              << "  Your IP      : " << gIp << "\n\n"
              << "  Commands:\n"
              << "    /register <username>\n"
              << "    /all <message>\n"
              << "    /dm <username> <message>\n"
              << "    /list\n"
              << "    /info <username>\n"
              << "    /status <ACTIVE|DO_NOT_DISTURB|INVISIBLE>\n"
              << "    /quit\n\n";

    // ── Start receive thread ──────────────────────────────────────────────────
    std::thread recvThread(receiveLoop);

    // ── Input loop runs on main thread ────────────────────────────────────────
    inputLoop();

    // ── Shutdown ──────────────────────────────────────────────────────────────
    gRunning = false;
    // Unblock the recv thread if it is waiting on recv()
    ::shutdown(gSock, SHUT_RDWR);
    ::close(gSock);

    if (recvThread.joinable()) recvThread.join();

    std::cout << "[Client] Goodbye.\n";
    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}
