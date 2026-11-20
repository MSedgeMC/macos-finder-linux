// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#pragma once
#include "appearance.h"
#include "windowcontour.h"
#include <KWindowEffects>
#include <QGuiApplication>
#include <QWindow>

// Region-limited native blur shared by Finder and standalone Preview. The
// cached surface contour preserves coverage/scale/alpha at antialiased edges.
class SidebarMaterial final {
public:
  static bool supported(QWidget *window) {
    return window->windowHandle() && KWindowEffects::isEffectAvailable(KWindowEffects::BlurBehind)
        && !window->property("finderDisableBlur").toBool()
        && !FinderAppearance::reduceTransparency() && !FinderAppearance::highContrast()
        && FinderAppearance::materialTint().alpha() < 255;
  }
  void update(QWidget *window, QRegion region, bool available) {
    if(!window->windowHandle()) return;
    // The KDE Wayland API takes wl_surface units. QT_SCALE_FACTOR adds
    // application zoom above the native output scale; convert that once.
    qreal surfaceScale = 1;
    if (QGuiApplication::platformName() == "wayland") {
      bool valid = false; const qreal scale = qEnvironmentVariable("QT_SCALE_FACTOR").toDouble(&valid);
      if (valid && std::isfinite(scale) && scale > 0) surfaceScale = scale;
    }
    const QTransform surfaceTransform = QTransform::fromScale(surfaceScale, surfaceScale);
    region = surfaceTransform.map(region);
    // KWin can change fully transparent client pixels. Cache a matching
    // contour in surface units, without rounding each scaled row separately.
    const bool square = window->isMaximized() || window->isFullScreen();
    const int coatingAlpha = FinderAppearance::materialTint().alpha();
    if (materialContourSize != window->size() || materialContourSquare != square || materialContourScale != surfaceScale || materialContourAlpha != coatingAlpha) {
      const int sw = qRound(window->width() * surfaceScale), sh = qRound(window->height() * surfaceScale);
      materialContour = QRegion(QRect(0, 0, sw, sh));
      if (!square) {
        // Match the client antialias raster, including partial coverage. A
        // whole-cell geometric test left a sharp backdrop fringe at corners.
        QImage coverage(QSize(sw, sh), QImage::Format_ARGB32_Premultiplied);
        coverage.setDevicePixelRatio(surfaceScale); coverage.fill(Qt::transparent);
        QPainter maskPainter(&coverage); maskPainter.setRenderHint(QPainter::Antialiasing);
        maskPainter.setPen(Qt::NoPen); maskPainter.setBrush(Qt::white);
        maskPainter.drawPath(windowContour(QRectF(window->rect()), 16)); maskPainter.end();
        const int span = qMin(int(std::ceil(26 * surfaceScale)), qMin(sw, sh) / 2);
        materialContour = QRegion(QRect(0, span, sw, sh - 2 * span));
        for (int row = 0; row < span; ++row) for (int y : {row, sh - row - 1}) {
          int left = 0, right = 0;
          while (left < span && (qAlpha(coverage.pixel(left, y)) * coatingAlpha + 127) / 255 == 0) ++left;
          while (right < span && (qAlpha(coverage.pixel(sw - right - 1, y)) * coatingAlpha + 127) / 255 == 0) ++right;
          materialContour += QRegion(QRect(left, y, sw - left - right, 1));
        }
      }
      materialContourSize = window->size(); materialContourSquare = square; materialContourScale = surfaceScale; materialContourAlpha = coatingAlpha;
    }
    region &= materialContour;
    KWindowEffects::enableBlurBehind(window->windowHandle(), available && !region.isEmpty(), region);
    // Saturation reduction applies only to the genuinely translucent sidebar.
    KWindowEffects::enableBackgroundContrast(window->windowHandle(), available && !region.isEmpty(), 1.0, 1.0,
                                             FinderAppearance::materialBackdropSaturation(), region);
  }
private:
  QSize materialContourSize;
  QRegion materialContour;
  bool materialContourSquare = false;
  qreal materialContourScale = 0;
  int materialContourAlpha = 0;
 };
