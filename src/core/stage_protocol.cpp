#include "core/stage_protocol.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <vector>

namespace vs {
namespace {

std::vector<std::string_view> split(std::string_view s) {
  std::vector<std::string_view> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    size_t j = i;
    while (j < s.size() && s[j] != ' ' && s[j] != '\t' && s[j] != '\r' && s[j] != '\n') ++j;
    if (j > i) out.push_back(s.substr(i, j - i));
    i = j;
  }
  return out;
}

std::optional<double> number(std::string_view s) {
  double v = 0;
  auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc{} || p != s.data() + s.size() || !std::isfinite(v)) return std::nullopt;
  return v;
}

std::string fmt(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.3f", v);
  return buf;
}

}  // namespace

std::optional<StageRequest> parse_request(std::string_view line) {
  auto t = split(line);
  if (t.empty()) return std::nullopt;
  StageRequest r;
  auto need_num = [&](StageCmd c) -> std::optional<StageRequest> {
    if (t.size() != 2) return std::nullopt;
    auto v = number(t[1]);
    if (!v) return std::nullopt;
    if (c != StageCmd::Move && *v < 0) return std::nullopt;  // timeouts
    r.cmd = c;
    r.value = *v;
    return r;
  };
  if (t[0] == "MOVE") return need_num(StageCmd::Move);
  if (t[0] == "ARRIVE") return need_num(StageCmd::Arrive);
  if (t[0] == "SETTLE") return need_num(StageCmd::Settle);
  if (t.size() == 1 && t[0] == "POS") return StageRequest{StageCmd::Pos, 0, {}};
  if (t.size() == 1 && t[0] == "RESET") return StageRequest{StageCmd::Reset, 0, {}};
  if (t.size() == 2 && t[0] == "FAULT" && (t[1] == "stall" || t[1] == "drop"))
    return StageRequest{StageCmd::Fault, 0, std::string(t[1])};
  return std::nullopt;
}

std::string format_request(const StageRequest& r) {
  switch (r.cmd) {
    case StageCmd::Move: return "MOVE " + fmt(r.value) + "\n";
    case StageCmd::Arrive: return "ARRIVE " + std::to_string(std::lround(r.value)) + "\n";
    case StageCmd::Settle: return "SETTLE " + std::to_string(std::lround(r.value)) + "\n";
    case StageCmd::Pos: return "POS\n";
    case StageCmd::Reset: return "RESET\n";
    case StageCmd::Fault: return "FAULT " + r.arg + "\n";
  }
  return "\n";
}

std::optional<StageReply> parse_reply(std::string_view line) {
  auto t = split(line);
  if (t.empty()) return std::nullopt;
  if (t.size() == 1 && t[0] == "OK") return StageReply{ReplyKind::Ok, 0, {}};
  if (t.size() == 1 && t[0] == "ARRIVED") return StageReply{ReplyKind::Arrived, 0, {}};
  if (t.size() == 1 && t[0] == "TIMEOUT") return StageReply{ReplyKind::Timeout, 0, {}};
  if (t[0] == "ERR") {
    std::string why;
    for (size_t i = 1; i < t.size(); ++i) why += (i > 1 ? " " : "") + std::string(t[i]);
    return StageReply{ReplyKind::Err, 0, why};
  }
  if (t.size() == 2 && t[0] == "DONE") {
    if (auto v = number(t[1])) return StageReply{ReplyKind::Done, *v, {}};
  }
  if (t.size() == 3 && t[0] == "POS") {
    if (auto v = number(t[1])) return StageReply{ReplyKind::Pos, *v, std::string(t[2])};
  }
  return std::nullopt;
}

std::string format_reply(const StageReply& r) {
  switch (r.kind) {
    case ReplyKind::Ok: return "OK\n";
    case ReplyKind::Err: return "ERR " + r.text + "\n";
    case ReplyKind::Arrived: return "ARRIVED\n";
    case ReplyKind::Done: return "DONE " + fmt(r.x_um) + "\n";
    case ReplyKind::Timeout: return "TIMEOUT\n";
    case ReplyKind::Pos: return "POS " + fmt(r.x_um) + " " + r.text + "\n";
  }
  return "\n";
}

}  // namespace vs
