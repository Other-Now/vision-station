#pragma once
// Live image + inspection overlay. Paints only on the GUI thread, from an
// immutable shared result, so the processing thread never waits on painting.
#include <QImage>
#include <QWidget>
#include <memory>

#include "pipeline/pipeline.hpp"

class LiveView : public QWidget {
  Q_OBJECT
 public:
  explicit LiveView(QWidget* parent = nullptr);
  void set_result(std::shared_ptr<const vs::InspectionResult> r, const vs::EdgeRoi& roi);
  void set_show_truth(bool on) { show_truth_ = on; update(); }

 protected:
  void paintEvent(QPaintEvent*) override;

 private:
  std::shared_ptr<const vs::InspectionResult> res_;
  QImage img_;
  vs::EdgeRoi roi_;
  bool show_truth_ = true;
};
