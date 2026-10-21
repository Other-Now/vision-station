#include "core/station_fsm.hpp"

namespace vs {
namespace {

using S = State;
using E = Event;

constexpr Transition kTable[] = {
    {S::Idle, E::Start, S::Moving},
    {S::Moving, E::Arrived, S::Settling},
    {S::Settling, E::Settled, S::Acquiring},
    {S::Acquiring, E::FrameReceived, S::Processing},
    {S::Processing, E::ResultReady, S::Idle},

    {S::Moving, E::Timeout, S::Error},
    {S::Settling, E::Timeout, S::Error},
    {S::Acquiring, E::Timeout, S::Error},
    {S::Processing, E::Timeout, S::Error},
    {S::Moving, E::Fault, S::Error},
    {S::Settling, E::Fault, S::Error},
    {S::Acquiring, E::Fault, S::Error},
    {S::Processing, E::Fault, S::Error},

    {S::Moving, E::Stop, S::Idle},
    {S::Settling, E::Stop, S::Idle},
    {S::Acquiring, E::Stop, S::Idle},
    {S::Processing, E::Stop, S::Idle},

    {S::Error, E::Reset, S::Idle},
};

}  // namespace

std::span<const Transition> transition_table() { return kTable; }

std::string_view to_string(State s) {
  switch (s) {
    case S::Idle: return "Idle";
    case S::Moving: return "Moving";
    case S::Settling: return "Settling";
    case S::Acquiring: return "Acquiring";
    case S::Processing: return "Processing";
    case S::Error: return "Error";
  }
  return "?";
}

std::string_view to_string(Event e) {
  switch (e) {
    case E::Start: return "Start";
    case E::Arrived: return "Arrived";
    case E::Settled: return "Settled";
    case E::FrameReceived: return "FrameReceived";
    case E::ResultReady: return "ResultReady";
    case E::Timeout: return "Timeout";
    case E::Fault: return "Fault";
    case E::Reset: return "Reset";
    case E::Stop: return "Stop";
  }
  return "?";
}

bool StationFsm::fire(Event e) {
  for (const Transition& t : kTable) {
    if (t.from == state_ && t.event == e) {
      state_ = t.to;
      if (t.to == S::Error) ++errors_;
      return true;
    }
  }
  ++rejected_;
  return false;
}

}  // namespace vs
