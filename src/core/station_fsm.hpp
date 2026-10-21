#pragma once
// Station state machine: Idle -> Moving -> Settling -> Acquiring -> Processing -> Idle,
// with any active state able to fall into Error. Table-driven so the same table
// is what docs/uml/state_station.puml is checked against (tests/test_uml_sync.cpp).
#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace vs {

enum class State : uint8_t { Idle, Moving, Settling, Acquiring, Processing, Error };
enum class Event : uint8_t {
  Start,          // begin an inspection cycle (move issued)
  Arrived,        // stage trajectory finished; still ringing
  Settled,        // controller reports in-position (DONE)
  FrameReceived,  // complete frame popped from the GVSP stream
  ResultReady,    // processing thread finished measuring the frame
  Timeout,        // any step exceeded its deadline
  Fault,          // stage error, rejected command, lost connection
  Reset,          // operator / auto-recovery cleared the error
  Stop,           // abort the cycle
};

struct Transition {
  State from;
  Event event;
  State to;
};

std::span<const Transition> transition_table();
std::string_view to_string(State s);
std::string_view to_string(Event e);

class StationFsm {
 public:
  State state() const { return state_; }
  // Applies the event. Returns false and leaves the state unchanged when the
  // event is not legal in the current state (counted in rejected()).
  bool fire(Event e);
  uint64_t rejected() const { return rejected_; }
  uint64_t errors() const { return errors_; }

 private:
  State state_ = State::Idle;
  uint64_t rejected_ = 0;
  uint64_t errors_ = 0;
};

}  // namespace vs
