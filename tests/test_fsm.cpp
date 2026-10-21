#include <gtest/gtest.h>

#include <set>

#include "core/station_fsm.hpp"

using namespace vs;

namespace {
constexpr State kStates[] = {State::Idle, State::Moving, State::Settling, State::Acquiring, State::Processing, State::Error};
constexpr Event kEvents[] = {Event::Start, Event::Arrived, Event::Settled, Event::FrameReceived, Event::ResultReady,
                             Event::Timeout, Event::Fault, Event::Reset, Event::Stop};

StationFsm at(State s) {
  // Drive a fresh machine to `s` through legal events only.
  StationFsm f;
  const Event path[] = {Event::Start, Event::Arrived, Event::Settled, Event::FrameReceived};
  if (s == State::Error) {
    f.fire(Event::Start);
    f.fire(Event::Fault);
    return f;
  }
  for (Event e : path) {
    if (f.state() == s) break;
    f.fire(e);
  }
  return f;
}
}  // namespace

TEST(Fsm, HappyCycleReturnsToIdle) {
  StationFsm f;
  EXPECT_TRUE(f.fire(Event::Start));
  EXPECT_EQ(f.state(), State::Moving);
  EXPECT_TRUE(f.fire(Event::Arrived));
  EXPECT_EQ(f.state(), State::Settling);
  EXPECT_TRUE(f.fire(Event::Settled));
  EXPECT_EQ(f.state(), State::Acquiring);
  EXPECT_TRUE(f.fire(Event::FrameReceived));
  EXPECT_EQ(f.state(), State::Processing);
  EXPECT_TRUE(f.fire(Event::ResultReady));
  EXPECT_EQ(f.state(), State::Idle);
  EXPECT_EQ(f.rejected(), 0u);
}

TEST(Fsm, EveryActiveStateFailsIntoError) {
  for (State s : {State::Moving, State::Settling, State::Acquiring, State::Processing}) {
    for (Event e : {Event::Timeout, Event::Fault}) {
      StationFsm f = at(s);
      ASSERT_EQ(f.state(), s);
      EXPECT_TRUE(f.fire(e));
      EXPECT_EQ(f.state(), State::Error) << to_string(s) << " + " << to_string(e);
      EXPECT_EQ(f.errors(), 1u);
    }
  }
}

TEST(Fsm, ErrorOnlyLeavesViaReset) {
  for (Event e : kEvents) {
    StationFsm f = at(State::Error);
    const bool ok = f.fire(e);
    if (e == Event::Reset) {
      EXPECT_TRUE(ok);
      EXPECT_EQ(f.state(), State::Idle);
    } else {
      EXPECT_FALSE(ok) << to_string(e);
      EXPECT_EQ(f.state(), State::Error);
    }
  }
}

TEST(Fsm, StopAbortsAnyActiveState) {
  for (State s : {State::Moving, State::Settling, State::Acquiring, State::Processing}) {
    StationFsm f = at(s);
    EXPECT_TRUE(f.fire(Event::Stop));
    EXPECT_EQ(f.state(), State::Idle);
  }
}

TEST(Fsm, IllegalEventsAreRejectedAndCounted) {
  std::set<std::pair<int, int>> legal;
  for (const auto& t : transition_table()) legal.insert({int(t.from), int(t.event)});
  int illegal = 0;
  for (State s : kStates) {
    for (Event e : kEvents) {
      StationFsm f = at(s);
      ASSERT_EQ(f.state(), s);
      const bool ok = f.fire(e);
      EXPECT_EQ(ok, legal.count({int(s), int(e)}) == 1) << to_string(s) << " + " << to_string(e);
      if (!ok) {
        ++illegal;
        EXPECT_EQ(f.state(), s) << "rejected event must not change state";
        EXPECT_EQ(f.rejected(), 1u);
      }
    }
  }
  EXPECT_EQ(illegal, 6 * 9 - static_cast<int>(transition_table().size()));
}

TEST(Fsm, TableIsDeterministic) {
  std::set<std::pair<int, int>> seen;
  for (const auto& t : transition_table())
    EXPECT_TRUE(seen.insert({int(t.from), int(t.event)}).second) << "duplicate (state, event) pair";
}
