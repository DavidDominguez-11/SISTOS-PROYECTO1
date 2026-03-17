# ─────────────────────────────────────────────────────────────────────────────
#  Makefile  –  C++ Protobuf Chat System
# ─────────────────────────────────────────────────────────────────────────────

CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -I.
LDFLAGS  := -lprotobuf -pthread

PROTO_DIR    := proto
PROTO_FILE   := $(PROTO_DIR)/chat.proto
PROTO_CC     := $(PROTO_DIR)/chat.pb.cc
PROTO_H      := $(PROTO_DIR)/chat.pb.h

SERVER_SRC   := server.cpp $(PROTO_CC)
CLIENT_SRC   := client.cpp $(PROTO_CC)

.PHONY: all proto server client clean

# ── Default target ─────────────────────────────────────────────────────────────
all: proto server client

# ── Generate protobuf C++ sources ─────────────────────────────────────────────
proto: $(PROTO_CC)

$(PROTO_CC) $(PROTO_H): $(PROTO_FILE)
	protoc --cpp_out=. $(PROTO_FILE)

# ── Build server ───────────────────────────────────────────────────────────────
server: $(SERVER_SRC) framing.h
	$(CXX) $(CXXFLAGS) -o server $(SERVER_SRC) $(LDFLAGS)

# ── Build client ───────────────────────────────────────────────────────────────
client: $(CLIENT_SRC) framing.h
	$(CXX) $(CXXFLAGS) -o client $(CLIENT_SRC) $(LDFLAGS)

# ── Clean up ──────────────────────────────────────────────────────────────────
clean:
	rm -f server client \
	      $(PROTO_CC) $(PROTO_H)
