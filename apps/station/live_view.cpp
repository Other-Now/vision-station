#include "live_view.hpp"

#include <QPainter>

LiveView::LiveView(QWidget* parent) : QWidget(parent) {
  setMinimumSize(640, 480);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void LiveView::set_result(std::shared_ptr<const vs::InspectionResult> r, const vs::EdgeRoi& roi) {
  res_ = std::move(r);
  roi_ = roi;
  if (res_ && res_->image && res_->width > 0) {
    // QImage wraps the shared buffer without copying; res_ keeps it alive.
    img_ = QImage(res_->image->data(), res_->width, res_->height, res_->width, QImage::Format_Grayscale8);
  }
  update();
}

void LiveView::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.fillRect(rect(), QColor(24, 24, 28));
  if (img_.isNull()) {
    p.setPen(Qt::gray);
    p.drawText(rect(), Qt::AlignCenter, "no image: connect and start live view");
    return;
  }
  // Fit the image, keep aspect, and map image pixel coordinates to widget.
  const double s = std::min(width() / double(img_.width()), height() / double(img_.height()));
  const QRectF dst((width() - img_.width() * s) / 2, (height() - img_.height() * s) / 2, img_.width() * s,
                   img_.height() * s);
  p.drawImage(dst, img_);
  auto X = [&](double px) { return dst.left() + px * s; };
  auto Y = [&](double py) { return dst.top() + py * s; };

  p.setPen(QPen(QColor(80, 160, 255), 1, Qt::DashLine));
  p.drawRect(QRectF(X(roi_.col0), Y(roi_.row0), (roi_.col1 - roi_.col0) * s, (roi_.row1 - roi_.row0) * s));

  const auto& r = *res_;
  if (show_truth_ && r.has_truth) {
    p.setPen(QPen(QColor(255, 80, 80), 1, Qt::DotLine));
    for (double e : {r.truth.left_px_e6 / 1e6, r.truth.right_px_e6 / 1e6}) p.drawLine(QPointF(X(e), Y(0)), QPointF(X(e), Y(img_.height())));
  }
  QFont f = p.font();
  f.setPointSize(11);
  p.setFont(f);
  if (r.measured) {
    p.setPen(QPen(QColor(60, 230, 120), 2));
    for (double e : {r.edges.left_px, r.edges.right_px}) p.drawLine(QPointF(X(e), Y(roi_.row0 - 20)), QPointF(X(e), Y(roi_.row1 + 20)));
    const QString txt = QString("width %1 px   width err %2 um   pos err %3 um")
                            .arg(r.edges.width_px(), 0, 'f', 3)
                            .arg(r.width_err_um, 0, 'f', 2)
                            .arg(r.pos_err_um, 0, 'f', 2);
    const QRectF box(dst.left() + 8, dst.bottom() - 30, p.fontMetrics().horizontalAdvance(txt) + 16, 24);
    p.fillRect(box, QColor(0, 0, 0, 170));
    p.drawText(box, Qt::AlignCenter, txt);
  } else {
    p.setPen(QColor(255, 170, 0));
    p.drawText(QPointF(dst.left() + 10, dst.bottom() - 10), "no part found in ROI");
  }
  p.setPen(Qt::white);
  p.drawText(QPointF(dst.left() + 8, dst.top() + 18),
             QString("seq %1%2").arg(r.truth.seq).arg(r.cycle >= 0 ? QString("   cycle %1").arg(r.cycle) : QString()));
}
