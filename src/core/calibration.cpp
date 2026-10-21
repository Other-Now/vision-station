#include "core/calibration.hpp"

#include <opencv2/core/version.hpp>
#if CV_VERSION_MAJOR >= 5
#include <opencv2/geometry.hpp>
#include <opencv2/calib.hpp>
#include <opencv2/objdetect.hpp>
#else
#include <opencv2/calib3d.hpp>
#endif
#include <opencv2/imgproc.hpp>

#include <cmath>
#include <random>

namespace vs {
namespace {

std::vector<cv::Point3f> board_points(const BoardSpec& b) {
  std::vector<cv::Point3f> pts;
  for (int y = 0; y < b.inner.height; ++y)
    for (int x = 0; x < b.inner.width; ++x)
      pts.emplace_back(static_cast<float>(x * b.square_mm), static_cast<float>(y * b.square_mm), 0.f);
  return pts;
}

// Renders by inverse mapping: pixel -> undistorted ray -> board plane -> colour.
// 3x3 supersampling gives anti-aliased edges, so corner localisation is
// limited by the detector, not by aliasing in the generator.
cv::Mat render_board(const BoardSpec& b, const Intrinsics& in, cv::Size sz, const cv::Matx33d& R,
                     const cv::Vec3d& t) {
  constexpr int ss = 3;
  std::vector<cv::Point2f> px;
  px.reserve(static_cast<size_t>(sz.area() * ss * ss));
  for (int y = 0; y < sz.height; ++y)
    for (int x = 0; x < sz.width; ++x)
      for (int j = 0; j < ss; ++j)
        for (int i = 0; i < ss; ++i)
          px.emplace_back(static_cast<float>(x + (i + 0.5) / ss - 0.5),
                          static_cast<float>(y + (j + 0.5) / ss - 0.5));
  std::vector<cv::Point2f> norm;
  cv::undistortPoints(px, norm, cv::Mat(in.K), cv::Mat(in.D));

  const cv::Matx33d Rt = R.t();
  const cv::Vec3d n(R(0, 2), R(1, 2), R(2, 2));  // board normal in camera frame
  const double nt = n.dot(t);
  // Squares run from -1 to inner+1 (one full square of border around the corners).
  const double x_min = -b.square_mm, x_max = b.inner.width * b.square_mm;
  const double y_min = -b.square_mm, y_max = b.inner.height * b.square_mm;
  const double margin = 0.6 * b.square_mm;  // white quiet zone

  cv::Mat img(sz, CV_8UC1);
  size_t k = 0;
  for (int y = 0; y < sz.height; ++y) {
    for (int x = 0; x < sz.width; ++x) {
      double acc = 0;
      for (int s = 0; s < ss * ss; ++s, ++k) {
        const cv::Vec3d ray(norm[k].x, norm[k].y, 1.0);
        const double lam = nt / n.dot(ray);
        const cv::Vec3d pb = Rt * (lam * ray - t);
        double v = 90;  // background
        if (pb[0] > x_min - margin && pb[0] < x_max + margin && pb[1] > y_min - margin &&
            pb[1] < y_max + margin) {
          v = 230;
          if (pb[0] > x_min && pb[0] < x_max && pb[1] > y_min && pb[1] < y_max) {
            const int cx = static_cast<int>(std::floor(pb[0] / b.square_mm));
            const int cy = static_cast<int>(std::floor(pb[1] / b.square_mm));
            if (((cx + cy) & 1) == 0) v = 25;
          }
        }
        acc += v;
      }
      img.at<uint8_t>(y, x) = cv::saturate_cast<uint8_t>(acc / (ss * ss));
    }
  }
  return img;
}

double corner_error(const std::vector<cv::Point2f>& a, const std::vector<cv::Point2f>& b, double* max_out) {
  // The detector may return the grid in reverse order; take the better match.
  double best = 1e30, best_max = 0;
  for (int rev = 0; rev < 2; ++rev) {
    double sum = 0, mx = 0;
    for (size_t i = 0; i < a.size(); ++i) {
      const cv::Point2f& q = rev ? b[b.size() - 1 - i] : b[i];
      const double d = std::hypot(a[i].x - q.x, a[i].y - q.y);
      sum += d;
      mx = std::max(mx, d);
    }
    if (sum < best) { best = sum; best_max = mx; }
  }
  *max_out = best_max;
  return best / static_cast<double>(a.size());
}

}  // namespace

std::vector<SyntheticView> make_synthetic_views(const BoardSpec& board, const Intrinsics& truth,
                                                cv::Size sz, int count, uint64_t seed,
                                                double noise_sigma, double blur_sigma) {
  std::mt19937_64 rng(seed);
  std::uniform_real_distribution<double> ang(-0.45, 0.45), roll(-0.3, 0.3), dist(380, 560), off(-60, 60);
  std::normal_distribution<double> noise(0, noise_sigma);
  const auto obj = board_points(board);
  const double cx = (board.inner.width - 1) * board.square_mm / 2;
  const double cy = (board.inner.height - 1) * board.square_mm / 2;

  std::vector<SyntheticView> views;
  while (static_cast<int>(views.size()) < count) {
    cv::Vec3d rvec(ang(rng), ang(rng), roll(rng));
    cv::Matx33d R;
    cv::Rodrigues(rvec, R);
    // Place the board centre at (off, off, dist) in the camera frame.
    const cv::Vec3d centre(off(rng), off(rng), dist(rng));
    const cv::Vec3d t = centre - R * cv::Vec3d(cx, cy, 0);

    std::vector<cv::Point2f> proj;
    cv::projectPoints(obj, rvec, t, cv::Mat(truth.K), cv::Mat(truth.D), proj);
    bool inside = true;
    for (const auto& p : proj)
      inside &= p.x > 30 && p.y > 30 && p.x < sz.width - 30 && p.y < sz.height - 30;
    if (!inside) continue;

    cv::Mat img = render_board(board, truth, sz, R, t);
    if (blur_sigma > 0) cv::GaussianBlur(img, img, cv::Size(0, 0), blur_sigma);
    if (noise_sigma > 0) {
      for (int y = 0; y < img.rows; ++y)
        for (int x = 0; x < img.cols; ++x)
          img.at<uint8_t>(y, x) = cv::saturate_cast<uint8_t>(img.at<uint8_t>(y, x) + noise(rng));
    }
    views.push_back({img, proj});
  }
  return views;
}

CalibReport calibrate(const std::vector<cv::Mat>& images, const BoardSpec& board, int holdout,
                      const std::vector<std::vector<cv::Point2f>>* truth) {
  CalibReport r;
  r.views_total = static_cast<int>(images.size());
  const auto obj = board_points(board);
  std::vector<std::vector<cv::Point2f>> found;
  std::vector<int> found_idx;
  cv::Size sz;
  for (size_t i = 0; i < images.size(); ++i) {
    const cv::Mat& g = images[i];
    sz = g.size();
    std::vector<cv::Point2f> c;
    if (!cv::findChessboardCorners(g, board.inner, c, cv::CALIB_CB_ADAPTIVE_THRESH | cv::CALIB_CB_NORMALIZE_IMAGE))
      continue;
    cv::cornerSubPix(g, c, cv::Size(5, 5), cv::Size(-1, -1),
                     cv::TermCriteria(cv::TermCriteria::EPS | cv::TermCriteria::COUNT, 50, 1e-4));
    found.push_back(c);
    found_idx.push_back(static_cast<int>(i));
  }
  r.views_detected = static_cast<int>(found.size());
  holdout = std::clamp(holdout, 0, std::max(0, r.views_detected - 3));
  r.views_train = r.views_detected - holdout;
  r.views_holdout = holdout;
  if (r.views_train < 3) return r;

  if (truth) {
    double sum = 0, mx = 0;
    for (size_t i = 0; i < found.size(); ++i) {
      double m = 0;
      sum += corner_error(found[i], (*truth)[static_cast<size_t>(found_idx[i])], &m);
      mx = std::max(mx, m);
    }
    r.corner_err_mean_px = sum / static_cast<double>(found.size());
    r.corner_err_max_px = mx;
  }

  std::vector<std::vector<cv::Point3f>> objs(static_cast<size_t>(r.views_train), obj);
  std::vector<std::vector<cv::Point2f>> train(found.begin(), found.begin() + r.views_train);
  cv::Mat K, D;
  std::vector<cv::Mat> rv, tv;
  r.rms_train_px = cv::calibrateCamera(objs, train, sz, K, D, rv, tv);
  r.fitted.K = cv::Matx33d(K);
  for (int i = 0; i < 5 && i < D.cols * D.rows; ++i) r.fitted.D[i] = D.at<double>(i);

  if (holdout > 0) {
    double se = 0;
    size_t n = 0;
    for (int i = r.views_train; i < r.views_detected; ++i) {
      cv::Mat rvec, tvec;
      cv::solvePnP(obj, found[static_cast<size_t>(i)], K, D, rvec, tvec);
      std::vector<cv::Point2f> proj;
      cv::projectPoints(obj, rvec, tvec, K, D, proj);
      for (size_t j = 0; j < proj.size(); ++j) {
        const cv::Point2f d = proj[j] - found[static_cast<size_t>(i)][j];
        se += d.x * d.x + d.y * d.y;
        ++n;
      }
    }
    r.rms_holdout_px = std::sqrt(se / static_cast<double>(n));
  }
  r.ok = true;
  return r;
}

}  // namespace vs
