// vs_calibrate: camera calibration from checkerboard images.
//
//   vs_calibrate --synthetic [--views 20] [--holdout 5] [--noise 2] [--save DIR] [--json f]
//   vs_calibrate --images DIR [--board 9x6] [--square-mm 20] [--holdout 3]
//
// --synthetic renders views with known intrinsics and distortion, so the
// fitted parameters and detected corners are scored against ground truth.
#include <opencv2/imgcodecs.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "core/calibration.hpp"

using namespace vs;

int main(int argc, char** argv) {
  bool synthetic = false;
  std::string dir, save, json;
  int views = 20, holdout = 5;
  bool fix_k3 = true;
  double noise = 2.0;
  BoardSpec board;
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    auto v = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
    if (k == "--synthetic") synthetic = true;
    else if (k == "--images") dir = v();
    else if (k == "--views") views = std::stoi(v());
    else if (k == "--holdout") holdout = std::stoi(v());
    else if (k == "--noise") noise = std::stod(v());
    else if (k == "--save") save = v();
    else if (k == "--free-k3") fix_k3 = false;
    else if (k == "--json") json = v();
    else if (k == "--square-mm") board.square_mm = std::stod(v());
    else if (k == "--board") {
      std::string b = v();
      board.inner = {std::stoi(b.substr(0, b.find('x'))), std::stoi(b.substr(b.find('x') + 1))};
    } else { std::fprintf(stderr, "unknown option %s\n", k.c_str()); return 2; }
  }

  Intrinsics truth;
  truth.K = cv::Matx33d(820, 0, 322.5, 0, 815, 238.0, 0, 0, 1);
  truth.D = cv::Vec<double, 5>(-0.21, 0.09, 0.0008, -0.0005, 0.0);
  std::vector<cv::Mat> imgs;
  std::vector<std::vector<cv::Point2f>> truth_corners;

  if (synthetic) {
    auto sv = make_synthetic_views(board, truth, {640, 480}, views, 42, noise);
    for (size_t i = 0; i < sv.size(); ++i) {
      imgs.push_back(sv[i].image);
      truth_corners.push_back(sv[i].true_corners);
      if (!save.empty()) cv::imwrite(save + "/view_" + std::to_string(i) + ".png", sv[i].image);
    }
  } else if (!dir.empty()) {
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
      cv::Mat m = cv::imread(e.path().string(), cv::IMREAD_GRAYSCALE);
      if (!m.empty()) imgs.push_back(m);
    }
  } else {
    std::fprintf(stderr, "need --synthetic or --images DIR\n");
    return 2;
  }

  const CalibReport r = calibrate(imgs, board, holdout, synthetic ? &truth_corners : nullptr, fix_k3);
  if (!r.ok) {
    std::fprintf(stderr, "calibration failed: board found in %d/%d views\n", r.views_detected, r.views_total);
    return 1;
  }
  const auto& K = r.fitted.K;
  const auto& D = r.fitted.D;
  std::printf("board found in %d/%d views; fit on %d, held out %d\n", r.views_detected, r.views_total, r.views_train,
              r.views_holdout);
  std::printf("RMS reprojection error: train %.4f px, held-out %.4f px\n", r.rms_train_px, r.rms_holdout_px);
  std::printf("fx %.2f fy %.2f cx %.2f cy %.2f | k1 %.4f k2 %.4f p1 %.5f p2 %.5f k3 %.4f\n", K(0, 0), K(1, 1), K(0, 2),
              K(1, 2), D[0], D[1], D[2], D[3], D[4]);
  std::ostringstream j;
  j << "{\"views\":" << r.views_total << ",\"detected\":" << r.views_detected << ",\"train\":" << r.views_train
    << ",\"holdout\":" << r.views_holdout << ",\"rms_train_px\":" << r.rms_train_px
    << ",\"rms_holdout_px\":" << r.rms_holdout_px << ",\"fx\":" << K(0, 0) << ",\"fy\":" << K(1, 1)
    << ",\"cx\":" << K(0, 2) << ",\"cy\":" << K(1, 2) << ",\"k1\":" << D[0] << ",\"k2\":" << D[1]
    << ",\"k3\":" << D[4] << ",\"fix_k3\":" << fix_k3;
  if (synthetic) {
    std::printf("truth: fx %.2f fy %.2f cx %.2f cy %.2f k1 %.4f k2 %.4f\n", truth.K(0, 0), truth.K(1, 1),
                truth.K(0, 2), truth.K(1, 2), truth.D[0], truth.D[1]);
    std::printf("errors: fx %+.3f%%  fy %+.3f%%  cx %+.2f px  cy %+.2f px\n",
                100 * (K(0, 0) / truth.K(0, 0) - 1), 100 * (K(1, 1) / truth.K(1, 1) - 1), K(0, 2) - truth.K(0, 2),
                K(1, 2) - truth.K(1, 2));
    std::printf("detected corners vs true projections: mean %.4f px, max %.4f px\n", r.corner_err_mean_px,
                r.corner_err_max_px);
    j << ",\"fx_err_pct\":" << 100 * (K(0, 0) / truth.K(0, 0) - 1) << ",\"fy_err_pct\":"
      << 100 * (K(1, 1) / truth.K(1, 1) - 1) << ",\"cx_err_px\":" << K(0, 2) - truth.K(0, 2)
      << ",\"cy_err_px\":" << K(1, 2) - truth.K(1, 2) << ",\"corner_err_mean_px\":" << r.corner_err_mean_px
      << ",\"corner_err_max_px\":" << r.corner_err_max_px;
  }
  j << "}";
  if (!json.empty()) std::ofstream(json) << j.str() << "\n";
  return 0;
}
