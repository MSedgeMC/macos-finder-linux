// SPDX-License-Identifier: MIT
#pragma once
#include <QGraphicsEffect>
#include <QPainter>
#include <QPainterPath>
#include <QWidget>
#include <cmath>
// Continuous corner geometry shared by alpha clipping and compositor regions.
// The 16px circular radius has a 60% smooth transition into the straight edges;
// a superellipse exponent is not equivalent to that corner-smoothing value.
// Parameter construction adapted from figma-squircle, Copyright 2021 Tien Pham
// (MIT; see LICENSES/figma-squircle.txt).
inline QPainterPath windowContour(QRectF rect, qreal radius) {
  QPainterPath path;
  if (radius <= 0) { path.addRect(rect); return path; }
  const qreal budget = qMin(rect.width(), rect.height()) / 2;
  const qreal r = qMin(radius, budget);
  const qreal smoothing = qMin(qreal(.6), budget / r - 1);
  const qreal span = (1 + smoothing) * r;
  const qreal angle = 45 * smoothing;
  const qreal radians = angle * M_PI / 180;
  const qreal arcLength = std::sin((90 - 2 * angle) * M_PI / 360) * r * std::sqrt(2.0);
  const qreal tangent = r * std::tan(radians / 2);
  const qreal c = tangent * std::cos(radians), d = tangent * std::sin(radians);
  const qreal b = (span - arcLength - c - d) / 3, a = 2 * b;
  QPainterPath corner;
  corner.moveTo(0, 0);
  corner.cubicTo(a, 0, a + b, 0, a + b + c, d);
  corner.arcTo(QRectF(span - 2 * r, 0, 2 * r, 2 * r), 90 - angle, -(90 - 2 * angle));
  const QPointF end = corner.currentPosition();
  corner.cubicTo(end + QPointF(d, c), end + QPointF(d, b + c), QPointF(span, span));
  const QTransform transforms[] = {
      {1, 0, 0, 1, rect.right() - span, rect.top()},
      {0, 1, -1, 0, rect.right(), rect.bottom() - span},
      {-1, 0, 0, -1, rect.left() + span, rect.bottom()},
      {0, -1, 1, 0, rect.left(), rect.top() + span}};
  for (const auto &transform : transforms) path.connectPath(transform.map(corner));
  path.closeSubpath();
  return path;
}
class ContourEffect : public QGraphicsEffect {
public:
  using QGraphicsEffect::QGraphicsEffect;

protected:
  void draw(QPainter *p) override {
    QPoint origin;
    QPixmap source = sourcePixmap(Qt::LogicalCoordinates, &origin, NoPad);
    if (source.isNull())
      return;
    const auto *widget = qobject_cast<QWidget *>(parent());
    const bool square = widget && (widget->window()->isMaximized() || widget->window()->isFullScreen());
    if (mask.size() != source.size() || mask.devicePixelRatio() != source.devicePixelRatio() || square != squareMask) {
      mask = QPixmap(source.size());
      mask.setDevicePixelRatio(source.devicePixelRatio());
      mask.fill(Qt::transparent);
      QPainter m(&mask);
      m.setRenderHint(QPainter::Antialiasing);
      m.setPen(Qt::NoPen); m.setBrush(Qt::white);
      m.drawPath(windowContour(QRectF(QPointF(0, 0), source.deviceIndependentSize()), square ? 0 : 16));
      squareMask = square;
    }
    QPainter clipped(&source);
    clipped.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    clipped.drawPixmap(0, 0, mask);
    clipped.end();
    p->drawPixmap(origin, source);
  }
private:
  QPixmap mask;
  bool squareMask = false;
};

