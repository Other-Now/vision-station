#include "pipeline/stage_client.hpp"

namespace vs {
namespace {
constexpr int kIoSlackMs = 250;  // socket deadline beyond the controller-side timeout
}

const char* to_string(StageClient::Status s) {
  switch (s) {
    case StageClient::Status::Ok: return "ok";
    case StageClient::Status::Timeout: return "timeout";
    case StageClient::Status::Rejected: return "rejected";
    case StageClient::Status::Disconnected: return "disconnected";
    case StageClient::Status::Protocol: return "protocol";
  }
  return "?";
}

bool StageClient::connect(const std::string& host, uint16_t port) {
  disconnect();
  host_ = host;
  port_ = port;
  net::init();
  s_ = net::connect_tcp(host, port, 1000);
  if (s_ == net::kInvalid) err_ = "connect " + host + ":" + std::to_string(port) + " failed";
  return s_ != net::kInvalid;
}

void StageClient::disconnect() {
  net::close(s_);
  s_ = net::kInvalid;
  buf_.clear();
}

StageClient::Status StageClient::exchange(const StageRequest& r, int read_timeout_ms, StageReply* rep) {
  if (s_ == net::kInvalid) return Status::Disconnected;
  if (!net::send_all(s_, format_request(r))) {
    disconnect();
    err_ = "send failed";
    return Status::Disconnected;
  }
  std::string line;
  bool timed_out = false;
  if (!net::read_line(s_, buf_, line, read_timeout_ms, &timed_out)) {
    // A late reply would desynchronise request/response pairing, so a socket
    // timeout drops the connection rather than leaving a stale reply queued.
    disconnect();
    err_ = timed_out ? "no reply within deadline" : "connection closed";
    return timed_out ? Status::Timeout : Status::Disconnected;
  }
  auto p = parse_reply(line);
  if (!p) {
    err_ = "bad reply: " + line;
    return Status::Protocol;
  }
  *rep = *p;
  if (rep->kind == ReplyKind::Err) {
    err_ = rep->text;
    return Status::Rejected;
  }
  if (rep->kind == ReplyKind::Timeout) {
    err_ = "controller timeout";
    return Status::Timeout;
  }
  return Status::Ok;
}

StageClient::Status StageClient::move(double x_um) {
  StageReply rep;
  return exchange({StageCmd::Move, x_um, {}}, kIoSlackMs, &rep);
}

StageClient::Status StageClient::wait_arrived(int timeout_ms) {
  StageReply rep;
  return exchange({StageCmd::Arrive, static_cast<double>(timeout_ms), {}}, timeout_ms + kIoSlackMs, &rep);
}

StageClient::Status StageClient::wait_settled(int timeout_ms, double* x_um) {
  StageReply rep;
  auto st = exchange({StageCmd::Settle, static_cast<double>(timeout_ms), {}}, timeout_ms + kIoSlackMs, &rep);
  if (st == Status::Ok && x_um) *x_um = rep.x_um;
  return st;
}

StageClient::Status StageClient::position(double* x_um, std::string* phase) {
  StageReply rep;
  auto st = exchange({StageCmd::Pos, 0, {}}, kIoSlackMs, &rep);
  if (st == Status::Ok) {
    if (x_um) *x_um = rep.x_um;
    if (phase) *phase = rep.text;
  }
  return st;
}

StageClient::Status StageClient::reset() {
  StageReply rep;
  return exchange({StageCmd::Reset, 0, {}}, kIoSlackMs, &rep);
}

StageClient::Status StageClient::inject(const std::string& mode) {
  StageReply rep;
  if (mode == "drop") {
    // The rig closes the connection without replying.
    if (s_ != net::kInvalid) net::send_all(s_, format_request({StageCmd::Fault, 0, mode}));
    return Status::Ok;
  }
  return exchange({StageCmd::Fault, 0, mode}, kIoSlackMs, &rep);
}

}  // namespace vs
