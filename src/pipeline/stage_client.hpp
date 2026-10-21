#pragma once
// Blocking client for the stage controller protocol. Every exchange has a
// deadline; a broken connection is reported, not retried here (the sequencer
// owns recovery).
#include <optional>
#include <string>

#include "core/net.hpp"
#include "core/stage_protocol.hpp"

namespace vs {

class StageClient {
 public:
  ~StageClient() { disconnect(); }
  bool connect(const std::string& host, uint16_t port);
  void disconnect();
  bool connected() const { return s_ != net::kInvalid; }

  enum class Status { Ok, Timeout, Rejected, Disconnected, Protocol };
  Status move(double x_um);
  Status wait_arrived(int timeout_ms);
  Status wait_settled(int timeout_ms, double* x_um = nullptr);
  Status position(double* x_um, std::string* phase);
  Status reset();
  Status inject(const std::string& mode);  // "stall" | "drop"
  const std::string& last_error() const { return err_; }

 private:
  Status exchange(const StageRequest& r, int read_timeout_ms, StageReply* rep);
  net::socket_t s_ = net::kInvalid;
  std::string buf_, err_;
  std::string host_;
  uint16_t port_ = 0;
};

const char* to_string(StageClient::Status s);

}  // namespace vs
