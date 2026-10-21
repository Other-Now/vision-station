#include "main_window.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include "live_view.hpp"

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  build_ui();
  timer_ = new QTimer(this);
  connect(timer_, &QTimer::timeout, this, &MainWindow::refresh_status);
  timer_->start(200);
}

MainWindow::~MainWindow() {
  if (pipe_) pipe_->stop();
}

void MainWindow::build_ui() {
  setWindowTitle("vision-station");
  auto* central = new QWidget(this);
  auto* h = new QHBoxLayout(central);
  view_ = new LiveView(central);
  h->addWidget(view_, 1);

  auto* side = new QVBoxLayout;
  h->addLayout(side);

  auto* conn = new QGroupBox("Connection");
  auto* cf = new QFormLayout(conn);
  camera_id_ = new QLineEdit("127.0.0.1");
  stage_port_ = new QSpinBox;
  stage_port_->setRange(1, 65535);
  stage_port_->setValue(5555);
  connect_btn_ = new QPushButton("Connect");
  cf->addRow("Camera (GigE)", camera_id_);
  cf->addRow("Stage TCP port", stage_port_);
  cf->addRow(connect_btn_);
  side->addWidget(conn);

  auto* cam = new QGroupBox("Camera (GenICam)");
  auto* camf = new QFormLayout(cam);
  exposure_ = new QDoubleSpinBox;
  exposure_->setRange(10, 100000);
  exposure_->setValue(2000);
  exposure_->setSuffix(" us");
  gain_ = new QSpinBox;
  gain_->setRange(0, 10);
  pixfmt_ = new QComboBox;
  fps_ = new QDoubleSpinBox;
  fps_->setRange(1, 1000);
  fps_->setValue(30);
  auto* apply = new QPushButton("Apply");
  camf->addRow("Exposure", exposure_);
  camf->addRow("Gain", gain_);
  camf->addRow("PixelFormat", pixfmt_);
  camf->addRow("Live frame rate", fps_);
  camf->addRow(apply);
  side->addWidget(cam);

  auto* stage = new QGroupBox("Stage jog (live view)");
  auto* sg = new QHBoxLayout(stage);
  for (double d : {-1000.0, -100.0, 100.0, 1000.0}) {
    auto* b = new QPushButton(QString("%1%2").arg(d > 0 ? "+" : "").arg(d));
    connect(b, &QPushButton::clicked, this, [this, d] { if (pipe_) pipe_->jog(d); });
    sg->addWidget(b);
    jog_btns_.push_back(b);
  }
  side->addWidget(stage);

  auto* run = new QGroupBox("Run");
  auto* rf = new QFormLayout(run);
  cycles_ = new QSpinBox;
  cycles_->setRange(1, 100000);
  cycles_->setValue(50);
  settle_ = new QComboBox;
  settle_->addItems({"controller", "none", "fixed:20"});
  live_btn_ = new QPushButton("Live (free run)");
  inspect_btn_ = new QPushButton("Run inspection");
  stop_btn_ = new QPushButton("Stop");
  reset_btn_ = new QPushButton("Reset error");
  auto* truth = new QCheckBox("Show ground-truth edges");
  truth->setChecked(true);
  rf->addRow("Cycles", cycles_);
  rf->addRow("Settle", settle_);
  rf->addRow(live_btn_);
  rf->addRow(inspect_btn_);
  rf->addRow(stop_btn_);
  rf->addRow(reset_btn_);
  rf->addRow(truth);
  side->addWidget(run);

  auto* st = new QGroupBox("Status");
  auto* sl = new QVBoxLayout(st);
  status_ = new QLabel("disconnected");
  status_->setTextFormat(Qt::RichText);
  status_->setMinimumWidth(300);
  QFont mono("Monospace");
  mono.setStyleHint(QFont::TypeWriter);
  status_->setFont(mono);
  sl->addWidget(status_);
  side->addWidget(st);
  side->addStretch(1);

  setCentralWidget(central);

  connect(connect_btn_, &QPushButton::clicked, this, &MainWindow::connect_station);
  connect(apply, &QPushButton::clicked, this, &MainWindow::apply_camera_controls);
  connect(live_btn_, &QPushButton::clicked, this, &MainWindow::start_live);
  connect(inspect_btn_, &QPushButton::clicked, this, &MainWindow::start_inspection);
  connect(stop_btn_, &QPushButton::clicked, this, &MainWindow::stop);
  connect(reset_btn_, &QPushButton::clicked, this, [this] { if (pipe_) pipe_->reset_error(); });
  connect(truth, &QCheckBox::toggled, view_, &LiveView::set_show_truth);
  set_running_ui(false);
}

vs::PipelineConfig MainWindow::config() const {
  vs::PipelineConfig c;
  c.camera_id = camera_id_->text().toStdString();
  c.stage_port = static_cast<uint16_t>(stage_port_->value());
  c.exposure_us = exposure_->value();
  return c;
}

bool MainWindow::connect_station() {
  if (pipe_ && pipe_->running()) return true;
  QApplication::setOverrideCursor(Qt::WaitCursor);
  pipe_ = std::make_unique<vs::Pipeline>(config());
  const bool ok = pipe_->connect();
  QApplication::restoreOverrideCursor();
  if (!ok) {
    status_->setText("connect failed: " + QString::fromStdString(pipe_->error()).toHtmlEscaped());
    pipe_.reset();
    return false;
  }
  // The processing thread calls this; it only queues an event to the GUI
  // thread, and the pipeline guarantees at most one is outstanding.
  pipe_->set_on_result([this] { QMetaObject::invokeMethod(this, "on_result", Qt::QueuedConnection); });
  auto& camera = pipe_->camera();
  pixfmt_->clear();
  for (const auto& f : camera.pixel_formats()) pixfmt_->addItem(QString::fromStdString(f));
  pixfmt_->setCurrentText(QString::fromStdString(camera.pixel_format()));
  exposure_->setValue(camera.exposure_us());
  gain_->setValue(static_cast<int>(camera.gain()));
  set_running_ui(false);
  return true;
}

void MainWindow::apply_camera_controls() {
  if (!pipe_) return;
  // GVCP register writes: ~1 ms each on loopback. They run on the GUI thread
  // and share a mutex with software_trigger(), not with frame reception.
  auto& c = pipe_->camera();
  bool ok = c.set_exposure_us(exposure_->value()) && c.set_gain(gain_->value());
  if (ok && !pipe_->running()) ok = c.set_pixel_format(pixfmt_->currentText().toStdString());
  if (!ok) QMessageBox::warning(this, "Camera", QString::fromStdString(c.error()));
}

bool MainWindow::start_live() {
  if (!pipe_ && !connect_station()) return false;
  if (pipe_->running()) return true;
  if (!pipe_->start_freerun(fps_->value())) {
    status_->setText("start failed: " + QString::fromStdString(pipe_->error()).toHtmlEscaped());
    return false;
  }
  set_running_ui(true);
  return true;
}

bool MainWindow::start_inspection() {
  if (!pipe_ && !connect_station()) return false;
  if (pipe_->running()) pipe_->stop();
  vs::InspectionPlan plan;
  plan.cycles = cycles_->value();
  const QString s = settle_->currentText();
  if (s == "none") plan.settle = vs::SettleMode::None;
  else if (s.startsWith("fixed:")) {
    plan.settle = vs::SettleMode::FixedDelay;
    plan.settle_delay_ms = s.mid(6).toInt();
  }
  if (!pipe_->start_inspection(plan)) {
    status_->setText("start failed: " + QString::fromStdString(pipe_->error()).toHtmlEscaped());
    return false;
  }
  set_running_ui(true);
  for (auto* b : jog_btns_) b->setEnabled(false);  // the sequencer owns the stage now
  return true;
}

void MainWindow::stop() {
  if (pipe_) pipe_->stop();
  set_running_ui(false);
}

void MainWindow::set_running_ui(bool running) {
  const bool connected = pipe_ != nullptr;
  connect_btn_->setEnabled(!running);
  live_btn_->setEnabled(!running);
  inspect_btn_->setEnabled(!running);
  stop_btn_->setEnabled(running);
  pixfmt_->setEnabled(connected && !running);  // payload size is fixed while streaming
  for (auto* b : jog_btns_) b->setEnabled(running);
}

void MainWindow::on_result() {
  if (!pipe_) return;
  auto r = pipe_->latest();
  pipe_->ack_result();  // the next result may notify again
  if (!r) return;
  view_->set_result(r, vs::EdgeRoi{});
  display_lat_.add(static_cast<double>(vs::now_ns() - r->t_done_ns) / 1e6);
  ++shown_;
}

void MainWindow::refresh_status() {
  if (!pipe_) return;
  if (pipe_->running() && pipe_->finished()) stop();
  const vs::Snapshot s = pipe_->snapshot();
  const double t = vs::now_ns() / 1e9;
  const double fps = last_t_ > 0 ? (s.processed - last_processed_) / (t - last_t_) : 0;
  last_t_ = t;
  last_processed_ = s.processed;
  const vs::Percentiles disp = display_lat_.compute_recent(200);
  const char* col = s.state == vs::State::Error ? "#e05050" : "#50c878";
  QString html;
  html += QString("state <b style='color:%1'>%2</b><br>").arg(col).arg(QString::fromUtf8(vs::to_string(s.state).data()));
  html += QString("fps %1 &nbsp; shown %2<br>").arg(fps, 0, 'f', 1).arg(shown_);
  html += QString("e2e   p50 %1  p99 %2 ms<br>").arg(s.e2e_ms.p50, 0, 'f', 2).arg(s.e2e_ms.p99, 0, 'f', 2);
  html += QString("gvsp  p50 %1  p99 %2 ms<br>").arg(s.transport_ms.p50, 0, 'f', 2).arg(s.transport_ms.p99, 0, 'f', 2);
  html += QString("trig  p50 %1  p99 %2 ms<br>").arg(s.trigger_ms.p50, 0, 'f', 2).arg(s.trigger_ms.p99, 0, 'f', 2);
  html += QString("paint p50 %1  p99 %2 ms<br>").arg(disp.p50, 0, 'f', 2).arg(disp.p99, 0, 'f', 2);
  html += QString("drops: queue %1  incomplete %2  seq-gap %3<br>").arg(s.queue_drops).arg(s.rx_incomplete).arg(s.seq_gap);
  html += QString("gvsp missing pkts %1  underruns %2<br>").arg(s.stream.missing_packets).arg(s.stream.underruns);
  html += QString("cycles ok %1  failed %2  recov %3<br>").arg(s.cycles_ok).arg(s.cycles_failed).arg(s.recoveries);
  html += QString("stage target %1 um<br>").arg(s.stage_target_um, 0, 'f', 1);
  if (!s.last_error.empty()) html += "<span style='color:#e0a050'>last error: " + QString::fromStdString(s.last_error).toHtmlEscaped() + "</span>";
  status_->setText(html);
}
