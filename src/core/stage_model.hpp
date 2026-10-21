#pragma once
// 1-axis linear stage dynamics: trapezoidal velocity profile to the target,
// then a decaying oscillation (structural ringing) around it. The controller's
// in-position test is |x - target| < tol continuously for `inpos_window_s`.
//
// Pure function of time, so tests can step it and the rig can sample it
// across an exposure window.
#include <cstdint>
#include <mutex>

namespace vs {

struct StageParams {
  double vmax_um_s = 100'000;   // 100 mm/s
  double accel_um_s2 = 1'000'000;
  double ring_ratio = 0.02;     // ringing amplitude = 2% of move distance...
  double ring_max_um = 40;      // ...capped at 40 um
  double ring_hz = 60;
  double ring_tau_s = 0.025;
  double inpos_tol_um = 2.0;    // 0.2 px at 10 um/px
  // Must be longer than the time the ringing spends inside +-tol around a zero
  // crossing; at 60 Hz a 3 ms window declared DONE with the stage still
  // swinging 2.37 um (> 2 um tol). See tests/test_stage_model.cpp.
  double inpos_window_s = 0.005;
  double min_um = -50'000, max_um = 50'000;
};

enum class StagePhase { Idle, Moving, Settling, InPosition, Fault };

class StageModel {
 public:
  explicit StageModel(StageParams p = {}) : p_(p) {}

  // Starts a move at time t (seconds). Returns false if out of range / faulted.
  bool move(double target_um, double t);
  double position(double t) const;
  StagePhase phase(double t) const;
  double target() const { return target_; }
  // Absolute times (s) when the trajectory ends and when the controller will
  // declare in-position. Infinity while stalled.
  double arrive_time() const;
  double settle_time() const { return settle_t_; }
  bool in_position_over(double t0, double t1) const;  // settled for all of [t0,t1]

  // Fault injection: the next move stalls (never arrives) until reset().
  void inject_stall() { stall_next_ = true; }
  void reset(double t);
  bool faulted() const { return faulted_; }
  const StageParams& params() const { return p_; }

 private:
  double profile_offset(double s) const;  // distance travelled s seconds after start
  double ringing(double s_after_arrive) const;
  double compute_settle_time() const;

  StageParams p_;
  double x0_ = 0, target_ = 0, t0_ = 0, dur_ = 0;
  double ring_amp_ = 0, settle_t_ = 0;
  bool stalled_ = false, stall_next_ = false, faulted_ = false;
};

}  // namespace vs
