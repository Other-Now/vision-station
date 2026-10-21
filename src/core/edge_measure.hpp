#pragma once
// Sub-pixel edge measurement of the part: average the rows of a band into a 1-D
// profile, take its gradient, and locate the strongest rising and falling
// edges with a 3-point peak fit on the gradient.
//
//   Integer   : argmax of |gradient| (pixel-level baseline)
//   Parabolic : vertex of a parabola through the 3 gradient samples
//   Gaussian  : vertex of a parabola through ln(gradient) -- exact when the
//               gradient is Gaussian-shaped, which a PSF-blurred step is.
#include <cstdint>

namespace vs {

enum class PeakFit { Integer, Parabolic, Gaussian };

struct EdgeRoi {
  int row0 = 150, row1 = 330;  // rows averaged into the profile
  int col0 = 2, col1 = 638;    // columns searched
};

struct EdgeResult {
  bool ok = false;
  double left_px = 0, right_px = 0;  // pixel i covers [i, i+1)
  double width_px() const { return right_px - left_px; }
  double center_px() const { return 0.5 * (left_px + right_px); }
  double contrast = 0;  // peak gradient magnitude (DN/px)
};

EdgeResult measure_edges(const uint8_t* img, int width, int height, const EdgeRoi& roi,
                         PeakFit fit = PeakFit::Gaussian, double min_contrast = 10.0);

}  // namespace vs
