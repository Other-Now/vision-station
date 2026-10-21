#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "core/edge_measure.hpp"
#include "core/scene.hpp"

using namespace vs;

namespace {

struct Sweep {
  double max_abs = 0, mean = 0;
};

// Moves the stage in sub-pixel steps across 2 px and measures both edges at
// each position against the renderer's exact truth.
Sweep sweep(PeakFit fit, double noise, double psf = 0.8) {
  SceneParams p;
  p.noise_sigma = noise;
  p.psf_sigma_px = psf;
  std::vector<uint8_t> img(static_cast<size_t>(p.width * p.height));
  Sweep s;
  int n = 0;
  for (int k = 0; k < 40; ++k) {
    const double stage = -10.0 + k * 0.5;  // 0.05 px steps
    const double samples[] = {stage};
    render_part(p, samples, static_cast<uint64_t>(k + 1), img.data());
    const EdgeResult r = measure_edges(img.data(), p.width, p.height, EdgeRoi{}, fit);
    EXPECT_TRUE(r.ok);
    const EdgeTruth t = edges_at(p, stage);
    for (double e : {r.left_px - t.left_px, r.right_px - t.right_px}) {
      s.max_abs = std::max(s.max_abs, std::abs(e));
      s.mean += e;
      ++n;
    }
  }
  s.mean /= n;
  return s;
}

}  // namespace

TEST(Edge, IntegerBaselineIsOffByUpToHalfAPixel) {
  const Sweep s = sweep(PeakFit::Integer, 0.0);
  EXPECT_GT(s.max_abs, 0.4);
  EXPECT_LE(s.max_abs, 0.5 + 1e-9);
}

TEST(Edge, ParabolicIsSubPixelButBiased) {
  const Sweep s = sweep(PeakFit::Parabolic, 0.0);
  EXPECT_LT(s.max_abs, 0.10);
  EXPECT_GT(s.max_abs, 0.005);  // systematic, position-dependent bias
}

TEST(Edge, GaussianFitNoiseless) {
  const Sweep s = sweep(PeakFit::Gaussian, 0.0);
  EXPECT_LT(s.max_abs, 0.03);
}

TEST(Edge, GaussianFitWithSensorNoise) {
  const Sweep s = sweep(PeakFit::Gaussian, 2.0);
  EXPECT_LT(s.max_abs, 0.05);
  EXPECT_LT(std::abs(s.mean), 0.01);
}

TEST(Edge, NoPartMeansNoMeasurement) {
  SceneParams p;
  std::vector<uint8_t> img(static_cast<size_t>(p.width * p.height), 40);
  EXPECT_FALSE(measure_edges(img.data(), p.width, p.height, EdgeRoi{}).ok);
}

TEST(Edge, TruthStampRoundTripsAndIsIgnoredByMeasurement) {
  SceneParams p;
  p.noise_sigma = 0;
  std::vector<uint8_t> img(static_cast<size_t>(p.width * p.height));
  const double samples[] = {123.4};
  render_part(p, samples, 1, img.data());
  const EdgeResult before = measure_edges(img.data(), p.width, p.height, EdgeRoi{0, 480, 0, 640});

  TruthStamp st;
  st.seq = 99;
  st.left_px_e6 = 42;
  write_stamp(st, img.data(), static_cast<size_t>(p.width));
  TruthStamp back;
  ASSERT_TRUE(read_stamp(img.data(), static_cast<size_t>(p.width), &back));
  EXPECT_EQ(back.seq, 99u);
  EXPECT_EQ(back.left_px_e6, 42);
  // ROI requests row 0, but the measurement clamps it away.
  const EdgeResult after = measure_edges(img.data(), p.width, p.height, EdgeRoi{0, 480, 0, 640});
  EXPECT_DOUBLE_EQ(before.left_px, after.left_px);
}
