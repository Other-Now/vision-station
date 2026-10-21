#pragma once
// Minimal blocking TCP line socket (POSIX + Winsock). Every read has a deadline.
#include <cstdint>
#include <string>

namespace vs::net {

using socket_t = std::intptr_t;
constexpr socket_t kInvalid = -1;

bool init();  // WSAStartup on Windows, no-op elsewhere
socket_t connect_tcp(const std::string& host, uint16_t port, int timeout_ms);
socket_t listen_tcp(uint16_t port);  // 127.0.0.1 only
socket_t accept_one(socket_t listener, int timeout_ms);  // kInvalid on timeout
void close(socket_t s);
bool send_all(socket_t s, const std::string& data);

// Reads one '\n'-terminated line. `buf` carries bytes received past the line
// between calls. Returns false on timeout, EOF or error (see `timed_out`).
bool read_line(socket_t s, std::string& buf, std::string& line, int timeout_ms,
               bool* timed_out = nullptr);
void set_nodelay(socket_t s);

}  // namespace vs::net
