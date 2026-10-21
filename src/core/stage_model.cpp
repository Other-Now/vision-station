#include "core/stage_model.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <limits>

namespace vs {

bool StageModel::move(double target_um, double t) {
  if (faulted_ || target_um < p_.min_um || target_um > p_.max_um) return false;
  x0_ = position(t);
  target_ = target_um;
  t0_ = t;
  const double d = std::abs(target_ - x0_);
  // Trapezoid (or triangle when the move is too short to reach vmax).
  const double t_acc = p_.vmax_um_s / p_.accel_um_s2;
  const double d_acc = 0.5 * p_.accel_um_s2 * t_acc * t_acc;
  dur_ = d >= 2 * d_acc ? 2 * t_acc + (d - 2 * d_acc) / p_.vmax_um_s
                        : 2 * std::sqrt(d / p_.accel_um_s2);
  ring_amp_ = std::min(p_.ring_ratio * d, p_.ring_max_um);
  stalled_ = stall_next_;
  stall_next_ = false;
  settle_t_ = stalled_ ? std::numeric_limits<double>::infinity() : compute_settle_time();
  return true;
}

double StageModel::profile_offset(double s) const {
  const double d = std::abs(target_ - x0_);
  if (s <= 0) return 0;
  if (s >= dur_) return d;
  const double t_acc = std::min(p_.vmax_um_s / p_.accel_um_s2, dur_ / 2);
  const double v_peak = p_.accel_um_s2 * t_acc;
  if (s < t_acc) return 0.5 * p_.accel_um_s2 * s * s;
  const double d_acc = 0.5 * p_.accel_um_s2 * t_acc * t_acc;
  const double t_cruise_end = dur_ - t_acc;
  if (s < t_cruise_end) return d_acc + v_peak * (s - t_acc);
  const double r = dur_ - s;
  return d - 0.5 * p_.accel_um_s2 * r * r;
}

double StageModel::ringing(double s) const {
  if (s <= 0) return 0;
  return ring_amp_ * std::exp(-s / p_.ring_tau_s) * std::sin(2 * std::numbers::pi * p_.ring_hz * s);
}

double StageModel::position(double t) const {
  const double s = t - t0_;
  const double dir = target_ >= x0_ ? 1.0 : -1.0;
  if (stalled_) {
    // Stops dead at 80% of the way (a jammed axis).
    return x0_ + dir * std::min(profile_offset(s), 0.8 * std::abs(target_ - x0_));
  }
  if (s < dur_) return x0_ + dir * profile_offset(s);
  return target_ + dir * ringing(s - dur_);
}

double StageModel::arrive_time() const {
  return stalled_ ? std::numeric_limits<double>::infinity() : t0_ + dur_;
}

double StageModel::compute_settle_time() const {
  // Step the in-position test the way a controller servo loop would (10 kHz).
  const double dt = 1e-4;
  const double start = t0_ + dur_;
  double inside_since = -1;
  for (double t = start; t < start + 5.0; t += dt) {
    const bool inside = std::abs(position(t) - target_) < p_.inpos_tol_um;
    if (!inside) {
      inside_since = -1;
    } else {
      if (inside_since < 0) inside_since = t;
      if (t - inside_since >= p_.inpos_window_s) return t;
    }
  }
  return start + 5.0;
}

StagePhase StageModel::phase(double t) const {
  if (faulted_) return StagePhase::Fault;
  if (dur_ == 0 && ring_amp_ == 0) return StagePhase::Idle;
  if (t < arrive_time()) return StagePhase::Moving;
  if (t < settle_t_) return StagePhase::Settling;
  return StagePhase::InPosition;
}

bool StageModel::in_position_over(double t0, double t1) const {
  return t0 >= settle_t_ && t1 >= t0;
}

void StageModel::reset(double t) {
  // Recovery: freeze the axis where it is and clear the fault/stall.
  const double x = position(t);
  x0_ = target_ = x;
  t0_ = t;
  dur_ = 0;
  ring_amp_ = 0;
  stalled_ = stall_next_ = faulted_ = false;
  settle_t_ = t;
}

}  // namespace vs
