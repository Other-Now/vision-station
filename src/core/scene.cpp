#include "core/scene.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <cstring>
#include <random>

namespace vs {
namespace {

// Antiderivative of the standard normal CDF: G(u) = u*Phi(u) + phi(u).
double big_g(double u) {
  const double phi = std::exp(-0.5 * u * u) / std::sqrt(2.0 * std::numbers::pi);
  const double cdf = 0.5 * std::erfc(-u / std::sqrt(2.0));
  return u * cdf + phi;
}

// Mean over pixel [i, i+1) of a unit step at `e` blurred by N(0, sigma).
double pixel_step(int i, double e, double sigma) {
  return sigma * (big_g((i + 1 - e) / sigma) - big_g((i - e) / sigma));
}

}  // namespace

EdgeTruth edges_at(const SceneParams& p, double stage_x_um) {
  const double center_px = p.width / 2.0 + (p.part_center_um - stage_x_um) / p.um_per_px;
  const double half = p.part_width_um / p.um_per_px / 2.0;
  return {center_px - half, center_px + half};
}

void render_part(const SceneParams& p, std::span<const double> stage_samples_um, uint64_t seed,
                 uint8_t* out) {
  // Edges are vertical, so one row profile describes the whole part band.
  std::vector<double> profile(static_cast<size_t>(p.width), 0.0);
  // Only ~6 sigma around each edge is fractional; the inside is exactly 1.
  const double reach = 6.0 * p.psf_sigma_px + 1.0;
  auto clampi = [&](double v) { return std::clamp(static_cast<int>(std::floor(v)), 0, p.width); };
  for (double sx : stage_samples_um) {
    const EdgeTruth e = edges_at(p, sx);
    const int l0 = clampi(e.left_px - reach), l1 = clampi(e.left_px + reach);
    const int r0 = clampi(e.right_px - reach), r1 = clampi(e.right_px + reach);
    if (l1 > r0) {  // part narrower than the blur: no exact-1 interior
      for (int i = 0; i < p.width; ++i)
        profile[static_cast<size_t>(i)] +=
            pixel_step(i, e.left_px, p.psf_sigma_px) - pixel_step(i, e.right_px, p.psf_sigma_px);
      continue;
    }
    for (int i = l0; i < l1; ++i) profile[static_cast<size_t>(i)] += pixel_step(i, e.left_px, p.psf_sigma_px);
    for (int i = l1; i < r0; ++i) profile[static_cast<size_t>(i)] += 1.0;
    for (int i = r0; i < r1; ++i) profile[static_cast<size_t>(i)] += 1.0 - pixel_step(i, e.right_px, p.psf_sigma_px);
  }
  const double n = static_cast<double>(std::max<size_t>(1, stage_samples_um.size()));

  // Noise: a 1M-entry table of N(0,1) samples read at a per-frame random
  // offset. Drawing 300k normals per frame cost ~4 ms and capped the rig
  // below the frame rates the overload tests need.
  static const std::vector<float> table = [] {
    std::vector<float> t(1u << 20);
    std::mt19937_64 g(12345);
    std::normal_distribution<float> nd(0.f, 1.f);
    for (float& v : t) v = nd(g);
    return t;
  }();
  constexpr size_t mask = (1u << 20) - 1;
  std::mt19937_64 rng(seed);
  size_t k = static_cast<size_t>(rng()) & mask;
  const float sigma = static_cast<float>(p.noise_sigma);

  std::vector<float> part_row(static_cast<size_t>(p.width));
  for (int x = 0; x < p.width; ++x)
    part_row[static_cast<size_t>(x)] = static_cast<float>(
        p.background + (p.foreground - p.background) * profile[static_cast<size_t>(x)] / n);
  const float bg = static_cast<float>(p.background);
  for (int y = 0; y < p.height; ++y) {
    uint8_t* row = out + static_cast<size_t>(y) * static_cast<size_t>(p.width);
    const bool in_part = y >= p.part_row0 && y < p.part_row1;
    for (int x = 0; x < p.width; ++x, k = (k + 1) & mask) {
      float v = in_part ? part_row[static_cast<size_t>(x)] : bg;
      v += sigma * table[k];
      row[x] = static_cast<uint8_t>(std::clamp(v + 0.5f, 0.f, 255.f));
    }
  }
}

void write_stamp(const TruthStamp& s, uint8_t* row0, size_t row_bytes) {
  if (row_bytes >= sizeof s) std::memcpy(row0, &s, sizeof s);
}

bool read_stamp(const uint8_t* row0, size_t row_bytes, TruthStamp* out) {
  if (row_bytes < sizeof *out) return false;
  std::memcpy(out, row0, sizeof *out);
  return out->magic == TruthStamp{}.magic && out->version == 1;
}

}  // namespace vs
