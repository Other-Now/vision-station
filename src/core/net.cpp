#include "core/net.hpp"

#include <chrono>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_len = int;
static int poll_fd(SOCKET s, short ev, int ms) {
  WSAPOLLFD p{s, ev, 0};
  return WSAPoll(&p, 1, ms);
}
#define VS_SEND_FLAGS 0
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using SOCKET = int;
using sock_len = socklen_t;
static int poll_fd(int s, short ev, int ms) {
  pollfd p{s, ev, 0};
  return ::poll(&p, 1, ms);
}
#define VS_SEND_FLAGS MSG_NOSIGNAL
#endif

namespace vs::net {
namespace {
SOCKET raw(socket_t s) { return static_cast<SOCKET>(s); }
int ms_left(std::chrono::steady_clock::time_point deadline) {
  auto d = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
  return d.count() < 0 ? 0 : static_cast<int>(d.count());
}
}  // namespace

bool init() {
#ifdef _WIN32
  WSADATA w;
  return WSAStartup(MAKEWORD(2, 2), &w) == 0;
#else
  return true;
#endif
}

void close(socket_t s) {
  if (s == kInvalid) return;
#ifdef _WIN32
  ::closesocket(raw(s));
#else
  ::close(raw(s));
#endif
}

void set_nodelay(socket_t s) {
  int one = 1;
  ::setsockopt(raw(s), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
}

socket_t connect_tcp(const std::string& host, uint16_t port, int timeout_ms) {
  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) return kInvalid;
  SOCKET s = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  // Blocking connect is fine on loopback; the deadline matters for reads.
  (void)timeout_ms;
  if (::connect(s, res->ai_addr, static_cast<sock_len>(res->ai_addrlen)) != 0) {
    ::freeaddrinfo(res);
    close(static_cast<socket_t>(s));
    return kInvalid;
  }
  ::freeaddrinfo(res);
  set_nodelay(static_cast<socket_t>(s));
  return static_cast<socket_t>(s);
}

socket_t listen_tcp(uint16_t port) {
  SOCKET s = ::socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(s, 8) != 0) {
    close(static_cast<socket_t>(s));
    return kInvalid;
  }
  return static_cast<socket_t>(s);
}

socket_t accept_one(socket_t listener, int timeout_ms) {
  if (poll_fd(raw(listener), POLLIN, timeout_ms) <= 0) return kInvalid;
  SOCKET c = ::accept(raw(listener), nullptr, nullptr);
  if (c == static_cast<SOCKET>(-1)) return kInvalid;
  set_nodelay(static_cast<socket_t>(c));
  return static_cast<socket_t>(c);
}

bool send_all(socket_t s, const std::string& data) {
  size_t off = 0;
  while (off < data.size()) {
    auto n = ::send(raw(s), data.data() + off, static_cast<int>(data.size() - off), VS_SEND_FLAGS);
    if (n <= 0) return false;
    off += static_cast<size_t>(n);
  }
  return true;
}

bool read_line(socket_t s, std::string& buf, std::string& line, int timeout_ms, bool* timed_out) {
  if (timed_out) *timed_out = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    if (auto nl = buf.find('\n'); nl != std::string::npos) {
      line = buf.substr(0, nl);
      buf.erase(0, nl + 1);
      return true;
    }
    const int left = ms_left(deadline);
    const int pr = poll_fd(raw(s), POLLIN, left);
    if (pr == 0) {
      if (timed_out) *timed_out = true;
      return false;
    }
    if (pr < 0) return false;
    char tmp[512];
    auto n = ::recv(raw(s), tmp, sizeof tmp, 0);
    if (n <= 0) return false;  // EOF or error
    buf.append(tmp, static_cast<size_t>(n));
    if (buf.size() > 4096) return false;  // no sane line is this long
  }
}

}  // namespace vs::net
