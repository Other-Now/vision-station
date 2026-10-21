#pragma once
// Stage controller TCP command protocol: one ASCII line per request, one line
// per reply. Request/response only (no unsolicited messages), so a client can
// put a deadline on every exchange.
//
//   MOVE <x_um>          -> OK | ERR <reason>       start a move, return at once
//   ARRIVE <timeout_ms>  -> ARRIVED | TIMEOUT       trajectory finished (may still ring)
//   SETTLE <timeout_ms>  -> DONE <x_um> | TIMEOUT   controller in-position
//   POS                  -> POS <x_um> <phase>
//   RESET                -> OK                      clear fault, hold position
//   FAULT <stall|drop>   -> OK                      test hook: next move stalls /
//                                                   server drops this connection
#include <optional>
#include <string>
#include <string_view>

namespace vs {

enum class StageCmd { Move, Arrive, Settle, Pos, Reset, Fault };

struct StageRequest {
  StageCmd cmd{};
  double value = 0;     // x_um for MOVE, timeout_ms for ARRIVE/SETTLE
  std::string arg;      // FAULT mode
};

std::optional<StageRequest> parse_request(std::string_view line);
std::string format_request(const StageRequest& r);

enum class ReplyKind { Ok, Err, Arrived, Done, Timeout, Pos };

struct StageReply {
  ReplyKind kind{};
  double x_um = 0;
  std::string text;  // ERR reason or POS phase
};

std::optional<StageReply> parse_reply(std::string_view line);
std::string format_reply(const StageReply& r);

}  // namespace vs
