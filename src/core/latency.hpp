#pragma once
// Latency samples -> percentiles. Samples are kept (runs are minutes, not days)
// so p99/p99.9 are exact rather than bucketed.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

namespace vs {

inline uint64_t now_ns() {
  // steady_clock is CLOCK_MONOTONIC on Linux, so it is comparable across
  // processes on the same host (the rig stamps fill time with the same clock).
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

struct Percentiles {
  size_t n = 0;
  double p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0, mean = 0;
};

class LatencyRecorder {
 public:
  void add(double v) {
    std::lock_guard lk(m_);
    v_.push_back(v);
  }
  void clear() {
    std::lock_guard lk(m_);
    v_.clear();
  }
  Percentiles compute() const {
    std::vector<double> s;
    {
      std::lock_guard lk(m_);
      s = v_;
    }
    return compute(std::move(s));
  }
  // Percentiles of the most recent `last` samples (for the live GUI panel).
  Percentiles compute_recent(size_t last) const {
    std::vector<double> s;
    {
      std::lock_guard lk(m_);
      size_t from = v_.size() > last ? v_.size() - last : 0;
      s.assign(v_.begin() + static_cast<std::ptrdiff_t>(from), v_.end());
    }
    return compute(std::move(s));
  }
  static Percentiles compute(std::vector<double> s) {
    Percentiles p;
    p.n = s.size();
    if (s.empty()) return p;
    std::sort(s.begin(), s.end());
    auto at = [&](double q) {
      size_t i = static_cast<size_t>(q * static_cast<double>(s.size() - 1) + 0.5);
      return s[std::min(i, s.size() - 1)];
    };
    p.p50 = at(0.50);
    p.p90 = at(0.90);
    p.p99 = at(0.99);
    p.p999 = at(0.999);
    p.max = s.back();
    double sum = 0;
    for (double x : s) sum += x;
    p.mean = sum / static_cast<double>(s.size());
    return p;
  }

 private:
  mutable std::mutex m_;
  std::vector<double> v_;
};

}  // namespace vs
