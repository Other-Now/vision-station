#pragma once
// Pinhole + radial/tangential camera calibration from checkerboard views
// (OpenCV calibrateCamera), plus a synthetic view generator with known
// intrinsics so every reported number can be checked against ground truth.
#include <opencv2/core.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace vs {

struct BoardSpec {
  cv::Size inner{9, 6};    // inner corners
  double square_mm = 20.0;
};

struct Intrinsics {
  cv::Matx33d K = cv::Matx33d::eye();
  cv::Vec<double, 5> D{0, 0, 0, 0, 0};  // k1 k2 p1 p2 k3
};

struct SyntheticView {
  cv::Mat image;                          // CV_8UC1
  std::vector<cv::Point2f> true_corners;  // exact projections, row-major
};

std::vector<SyntheticView> make_synthetic_views(const BoardSpec& board, const Intrinsics& truth,
                                                cv::Size image_size, int count, uint64_t seed,
                                                double noise_sigma = 2.0, double blur_sigma = 0.7);

struct CalibReport {
  bool ok = false;
  int views_total = 0, views_detected = 0, views_train = 0, views_holdout = 0;
  double rms_train_px = 0;     // what calibrateCamera returns (optimistic: fit on these)
  double rms_holdout_px = 0;   // PnP on unseen views with the fitted intrinsics
  Intrinsics fitted;
  double corner_err_mean_px = -1, corner_err_max_px = -1;  // vs truth, if given
};

// The last `holdout` detected views are not used for fitting.
// fix_k3: hold the 6th-order radial term at 0. With a moderate lens and
// boards that do not reach the image corners, k2 and k3 are not separately
// identifiable and trade off against each other while the RMS stays tiny.
CalibReport calibrate(const std::vector<cv::Mat>& images, const BoardSpec& board, int holdout,
                      const std::vector<std::vector<cv::Point2f>>* truth_corners = nullptr, bool fix_k3 = true);

}  // namespace vs
