# C++ Protobuf Chat System

A fully compliant client-server chat system in C++17 using POSIX TCP sockets
and Protocol Buffers (proto3).

---

## Directory Structure

```
chat_system/
├── proto/
│   └── chat.proto          # All message definitions
├── framing.h               # TCP framing helpers (shared)
├── server.cpp              # Multi-threaded chat server
├── client.cpp              # Two-threaded chat client
├── Makefile
└── README.md
```

---

## 1. Install Dependencies

### Ubuntu / Debian / WSL

```bash
sudo apt-get update
sudo apt-get install -y \
    g++ \
    protobuf-compiler \
    libprotobuf-dev
```

Verify:
```bash
protoc --version   # libprotoc 3.x
g++ --version      # g++ 9+ recommended
```

---

## 2. Generate Protobuf Sources & Build

```bash
# From inside the chat_system/ directory:
make
```

This runs three steps automatically:
1. `protoc` generates `proto/chat.pb.cc` and `proto/chat.pb.h`
2. Compiles `server`
3. Compiles `client`

---

## 3. Run the Server

```bash
./server          # default port 8080
./server 9090     # custom port
```

Output:
```
[Server] Listening on port 8080 …
```

---

## 4. Run Clients (each in a separate terminal)

```bash
./client                     # connects to 127.0.0.1:8080
./client 127.0.0.1 8080      # explicit host and port
./client 192.168.1.10 9090   # remote server
```

---

## 5. Client Commands

| Command | Description |
|---|---|
| `/register <username>` | Register with the server |
| `/all <message>` | Send a broadcast message to all users |
| `/dm <username> <message>` | Send a direct message |
| `/list` | List all online users and their statuses |
| `/info <username>` | Get detailed info about a user |
| `/status <ACTIVE\|DO_NOT_DISTURB\|INVISIBLE>` | Change your status |
| `/quit` | Gracefully disconnect |

---

## 6. Example Session

**Terminal 1 – Server**
```
$ ./server
[Server] Listening on port 8080 …
[Server] Connection from 127.0.0.1 (fd=4)
[Server] Registered: alice (127.0.0.1)
[Server] Connection from 127.0.0.1 (fd=5)
[Server] Registered: bob (127.0.0.1)
```

**Terminal 2 – Client (alice)**
```
$ ./client
  Connected to 127.0.0.1:8080

/register alice
[Server]: Welcome, alice!  (code=200)

/all Hello everyone!
[Broadcast] alice: Hello everyone!

/dm bob Hey Bob, private message!

/list
[Server]: Online users (2):
  alice  [ACTIVE]
  bob    [ACTIVE]

/status DO_NOT_DISTURB
[Server]: Status updated.  (code=200)

/quit
[Client] Goodbye.
```

**Terminal 3 – Client (bob)**
```
$ ./client
/register bob
[Server]: Welcome, bob!  (code=200)

[Broadcast] alice: Hello everyone!
[DM] alice: Hey Bob, private message!

/info alice
[Server]: User info
  username : alice
  ip       : 127.0.0.1
  status   : DO_NOT_DISTURB
```

---

## 7. Wire Protocol Summary

```
[1 byte TYPE][4 bytes LENGTH (big-endian)][LENGTH bytes protobuf payload]
```

| Type | Direction | Message |
|------|-----------|---------|
| 1  | C→S | Register |
| 2  | C→S | MessageGeneral |
| 3  | C→S | MessageDM |
| 4  | C→S | ChangeStatus |
| 5  | C→S | ListUsers |
| 6  | C→S | GetUserInfo |
| 7  | C→S | Quit |
| 10 | S→C | ServerResponse |
| 11 | S→C | AllUsers |
| 12 | S→C | ForDm |
| 13 | S→C | BroadcastDelivery |
| 14 | S→C | GetUserInfoResponse |

---

## 8. Clean Build Artifacts

```bash
make clean
```
