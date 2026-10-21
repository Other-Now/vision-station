#pragma once
#include <QMainWindow>
#include <memory>

#include "core/latency.hpp"
#include "pipeline/pipeline.hpp"

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTimer;
class LiveView;

class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

  bool connect_station();
  bool start_live();
  bool start_inspection();
  uint64_t frames_shown() const { return shown_; }

 private slots:
  void on_result();      // queued from the processing thread
  void refresh_status(); // 5 Hz timer
  void stop();

 private:
  void build_ui();
  void apply_camera_controls();
  void set_running_ui(bool running);
  vs::PipelineConfig config() const;

  std::unique_ptr<vs::Pipeline> pipe_;
  LiveView* view_ = nullptr;
  QLineEdit* camera_id_ = nullptr;
  QSpinBox* stage_port_ = nullptr;
  QDoubleSpinBox* exposure_ = nullptr;
  QSpinBox* gain_ = nullptr;
  QComboBox* pixfmt_ = nullptr;
  QDoubleSpinBox* fps_ = nullptr;
  QSpinBox* cycles_ = nullptr;
  QComboBox* settle_ = nullptr;
  QPushButton *connect_btn_ = nullptr, *live_btn_ = nullptr, *inspect_btn_ = nullptr, *stop_btn_ = nullptr,
              *reset_btn_ = nullptr;
  QList<QPushButton*> jog_btns_;
  QLabel* status_ = nullptr;
  QTimer* timer_ = nullptr;

  vs::LatencyRecorder display_lat_;  // processed -> painted
  uint64_t shown_ = 0, last_processed_ = 0;
  double last_t_ = 0;
};
