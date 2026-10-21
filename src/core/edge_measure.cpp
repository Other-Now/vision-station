#include "core/edge_measure.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace vs {
namespace {

// Sub-sample offset in [-0.5, 0.5] of the peak through (a, b, c) at -1, 0, +1.
double peak_offset(double a, double b, double c, PeakFit fit) {
  if (fit == PeakFit::Integer) return 0.0;
  if (fit == PeakFit::Gaussian) {
    if (a <= 0 || b <= 0 || c <= 0) return peak_offset(a, b, c, PeakFit::Parabolic);
    a = std::log(a);
    b = std::log(b);
    c = std::log(c);
  }
  const double den = a - 2 * b + c;
  if (den >= 0) return 0.0;  // not a maximum
  return std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
}

}  // namespace

EdgeResult measure_edges(const uint8_t* img, int width, int height, const EdgeRoi& roi,
                         PeakFit fit, double min_contrast) {
  EdgeResult r;
  const int r0 = std::max(1, roi.row0), r1 = std::min(height, roi.row1);  // row 0 = truth stamp
  const int c0 = std::max(1, roi.col0), c1 = std::min(width - 1, roi.col1);
  if (r1 - r0 < 1 || c1 - c0 < 4) return r;

  std::vector<double> prof(static_cast<size_t>(width), 0.0);
  for (int y = r0; y < r1; ++y) {
    const uint8_t* row = img + static_cast<size_t>(y) * static_cast<size_t>(width);
    for (int x = 0; x < width; ++x) prof[static_cast<size_t>(x)] += row[x];
  }
  for (double& v : prof) v /= (r1 - r0);

  // prof[i] is the mean of pixel i, centred at i + 0.5. The central
  // difference g[i] is the slope at that same centre, so a peak at index i
  // plus sub-sample offset d is an edge at i + 0.5 + d.
  std::vector<double> g(static_cast<size_t>(width), 0.0);
  for (int x = c0; x < c1; ++x)
    g[static_cast<size_t>(x)] = 0.5 * (prof[static_cast<size_t>(x + 1)] - prof[static_cast<size_t>(x - 1)]);

  int il = -1, ir = -1;
  double gl = 0, gr = 0;
  for (int x = c0 + 1; x < c1 - 1; ++x) {
    const double v = g[static_cast<size_t>(x)];
    if (v > gl) { gl = v; il = x; }
    if (-v > gr) { gr = -v; ir = x; }
  }
  r.contrast = std::min(gl, gr);
  if (il < 0 || ir < 0 || il >= ir || r.contrast < min_contrast) return r;

  auto at = [&](int i, double sign) { return sign * g[static_cast<size_t>(i)]; };
  r.left_px = il + 0.5 + peak_offset(at(il - 1, 1), at(il, 1), at(il + 1, 1), fit);
  r.right_px = ir + 0.5 + peak_offset(at(ir - 1, -1), at(ir, -1), at(ir + 1, -1), fit);
  r.ok = true;
  return r;
}

}  // namespace vs
