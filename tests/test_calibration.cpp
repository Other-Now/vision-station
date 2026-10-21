#include <gtest/gtest.h>

#include "core/calibration.hpp"

using namespace vs;

TEST(Calibration, RecoversKnownIntrinsicsFromSyntheticViews) {
  BoardSpec board;
  Intrinsics truth;
  truth.K = cv::Matx33d(820, 0, 322.5, 0, 815, 238.0, 0, 0, 1);
  truth.D = cv::Vec<double, 5>(-0.21, 0.09, 0.0008, -0.0005, 0.0);
  auto views = make_synthetic_views(board, truth, {640, 480}, 14, 7, 2.0);
  std::vector<cv::Mat> imgs;
  std::vector<std::vector<cv::Point2f>> corners;
  for (auto& v : views) {
    imgs.push_back(v.image);
    corners.push_back(v.true_corners);
  }
  const CalibReport r = calibrate(imgs, board, 3, &corners);
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.views_detected, 14);
  EXPECT_LT(r.rms_train_px, 0.2);
  EXPECT_LT(r.rms_holdout_px, 0.3);
  EXPECT_LT(r.corner_err_mean_px, 0.1);
  EXPECT_NEAR(r.fitted.K(0, 0), 820, 820 * 0.01);
  EXPECT_NEAR(r.fitted.K(1, 1), 815, 815 * 0.01);
  EXPECT_NEAR(r.fitted.K(0, 2), 322.5, 3.0);
  EXPECT_NEAR(r.fitted.K(1, 2), 238.0, 3.0);
  EXPECT_NEAR(r.fitted.D[0], -0.21, 0.03);
}

TEST(Calibration, FailsCleanlyWithoutBoards) {
  std::vector<cv::Mat> imgs(4, cv::Mat(480, 640, CV_8UC1, cv::Scalar(128)));
  const CalibReport r = calibrate(imgs, BoardSpec{}, 1);
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.views_detected, 0);
}
