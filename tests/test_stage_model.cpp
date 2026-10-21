#include <gtest/gtest.h>

#include <cmath>

#include "core/stage_model.hpp"

using namespace vs;

TEST(StageModel, TrapezoidArrivesAtTarget) {
  StageModel m;
  ASSERT_TRUE(m.move(10'000, 0.0));  // 10 mm: 0.1 s accel phases + cruise
  // t_acc = 0.1 s, d_acc = 5 mm each side -> exactly a triangle at 10 mm.
  EXPECT_NEAR(m.arrive_time(), 0.2, 1e-9);
  EXPECT_NEAR(m.position(0.1), 5'000, 1e-6);
  EXPECT_NEAR(m.position(m.arrive_time()), 10'000, 1e-6);
}

TEST(StageModel, RingsAfterArrivalThenSettles) {
  StageModel m;
  ASSERT_TRUE(m.move(2'000, 0.0));
  const double ta = m.arrive_time(), ts = m.settle_time();
  EXPECT_GT(ts, ta + 0.01);
  double worst_before = 0;
  for (double t = ta; t < ts - 0.004; t += 1e-4) worst_before = std::max(worst_before, std::abs(m.position(t) - 2'000));
  EXPECT_GT(worst_before, m.params().inpos_tol_um) << "should still be out of tolerance before settle";
  for (double t = ts; t < ts + 0.5; t += 1e-5) ASSERT_LT(std::abs(m.position(t) - 2'000), m.params().inpos_tol_um);
  EXPECT_EQ(m.phase(ta + 1e-4), StagePhase::Settling);
  EXPECT_EQ(m.phase(ts + 1e-4), StagePhase::InPosition);
  EXPECT_FALSE(m.in_position_over(ta, ta + 0.002));
  EXPECT_TRUE(m.in_position_over(ts, ts + 0.002));
}

namespace {
double max_excursion_after_done(double window_s) {
  StageParams p;
  p.inpos_window_s = window_s;
  StageModel m(p);
  m.move(2'000, 0.0);
  double worst = 0;
  for (double t = m.settle_time(); t < m.settle_time() + 0.5; t += 1e-5)
    worst = std::max(worst, std::abs(m.position(t) - 2'000));
  return worst;
}
}  // namespace

// The controller's DONE means "inside tolerance for one window". If the window
// is shorter than the time the ringing spends near a zero crossing, DONE fires
// while the stage still swings outside tolerance.
TEST(StageModel, ShortInPositionWindowDeclaresDoneTooEarly) {
  EXPECT_GT(max_excursion_after_done(0.003), StageParams{}.inpos_tol_um);
  EXPECT_LT(max_excursion_after_done(0.005), StageParams{}.inpos_tol_um);
}

TEST(StageModel, StallNeverArrivesUntilReset) {
  StageModel m;
  m.inject_stall();
  ASSERT_TRUE(m.move(1'000, 0.0));
  EXPECT_TRUE(std::isinf(m.arrive_time()));
  EXPECT_NEAR(m.position(10.0), 800, 1e-6);
  m.reset(10.0);
  EXPECT_NEAR(m.position(11.0), 800, 1e-6);
  ASSERT_TRUE(m.move(1'000, 11.0));
  EXPECT_FALSE(std::isinf(m.arrive_time()));
}

TEST(StageModel, OutOfRangeRejected) {
  StageModel m;
  EXPECT_FALSE(m.move(60'000, 0.0));
  EXPECT_FALSE(m.move(-60'000, 0.0));
}

TEST(StageModel, ShortMoveIsTriangular) {
  StageModel m;
  ASSERT_TRUE(m.move(100, 0.0));
  EXPECT_NEAR(m.arrive_time(), 2 * std::sqrt(100 / m.params().accel_um_s2), 1e-12);
}
