# ─────────────────────────────────────────────────────────────────────────────
#  Makefile  –  C++ Protobuf Chat System
# ─────────────────────────────────────────────────────────────────────────────

CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -I.
PROTO_CFLAGS := $(shell pkg-config --cflags protobuf 2>NUL)
PROTO_LIBS   := $(shell pkg-config --libs protobuf 2>NUL)

ifeq ($(OS),Windows_NT)
EXE      := .exe
LDFLAGS  := -lprotobuf -lws2_32 $(PROTO_LIBS)
RM_FILES := del /Q
else
EXE      :=
LDFLAGS  := -lprotobuf -pthread $(PROTO_LIBS)
RM_FILES := rm -f
endif

CXXFLAGS += $(PROTO_CFLAGS)

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
	$(CXX) $(CXXFLAGS) -o server$(EXE) $(SERVER_SRC) $(LDFLAGS)

# ── Build client ───────────────────────────────────────────────────────────────
client: $(CLIENT_SRC) framing.h
	$(CXX) $(CXXFLAGS) -o client$(EXE) $(CLIENT_SRC) $(LDFLAGS)

# ── Clean up ──────────────────────────────────────────────────────────────────
clean:
	$(RM_FILES) "server$(EXE)" "client$(EXE)" "$(PROTO_CC)" "$(PROTO_H)"
