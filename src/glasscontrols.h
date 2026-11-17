// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#pragma once
#include "appearance.h"
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QToolButton>
#include <QStyleOptionToolButton>

// Original Qt painting for a shared glass capsule. The host supplies the
// genuine compositor blur underneath; this coating does not invent a backdrop
// or claim refractive displacement. No captures, animations or idle timers.
namespace FinderGlass {
inline QPainterPath contour(QRectF bounds) {
    QPainterPath path;
    const qreal radius = qMin(bounds.width(), bounds.height()) / 2;
    path.addRoundedRect(bounds, radius, radius);
    return path;
}
inline void paint(QPainter &p, QRectF bounds, bool active = false, bool pressed = false) {
    p.save(); p.setRenderHint(QPainter::Antialiasing);
    bounds.adjust(.6, .6, -.6, -.6);
    const auto path = contour(bounds);
    const bool dark = FinderAppearance::dark();
    if (FinderAppearance::highContrast() || FinderAppearance::reduceTransparency()) {
        p.fillPath(path, FinderAppearance::palette().color(active ? QPalette::Highlight : QPalette::Button));
        p.setPen(QPen(FinderAppearance::color(FinderAppearance::Border), 1));
        p.drawPath(path); p.restore(); return;
    }
    const int lift = pressed ? 25 : active ? 38 : 0;
    QLinearGradient body(bounds.topLeft(), bounds.bottomLeft());
    body.setColorAt(0, QColor(dark ? 255 : 255, 255, 255, (dark ? 16 : 82) + lift));
    body.setColorAt(.48, QColor(255, 255, 255, (dark ? 6 : 36) + lift));
    body.setColorAt(1, QColor(255, 255, 255, (dark ? 10 : 58) + lift));
    p.fillPath(path, body);
    // A directional rim, not a uniform black outline. Keep the light at the
    // upper edge and a restrained lower glint, including over dark backdrops.
    QLinearGradient rim(bounds.topLeft(), bounds.bottomRight());
    rim.setColorAt(0, QColor(255, 255, 255, dark ? 76 : 180));
    rim.setColorAt(.35, QColor(255, 255, 255, dark ? 26 : 95));
    rim.setColorAt(.65, QColor(dark ? 255 : 45, dark ? 255 : 45, dark ? 255 : 50, dark ? 12 : 28));
    rim.setColorAt(1, QColor(255, 255, 255, dark ? 40 : 130));
    p.setPen(QPen(QBrush(rim), .85)); p.drawPath(path);
    p.restore();
}
}

class GlassCapsule final : public QWidget {
public:
    explicit GlassCapsule(QWidget *parent = nullptr) : QWidget(parent) {
        setProperty("finderGlassSurface", true);
        setAttribute(Qt::WA_StyledBackground, false);
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this); FinderGlass::paint(painter, rect());
        // Quiet separators share one material surface, never stacked blur.
        const auto children = findChildren<QToolButton *>(QString(), Qt::FindDirectChildrenOnly);
        if (children.size() != 2 && children.size() != 4) return;
        painter.setPen(QPen(FinderAppearance::color(FinderAppearance::MaterialEdge), 1));
        for (int i = 1; i < children.size(); ++i) {
            if (!children[i]->isVisible() || !children[i-1]->isVisible()) continue;
            const int x = children[i]->geometry().left();
            painter.drawLine(x, height()/2 - 8, x, height()/2 + 8);
        }
    }
};

class GlassToolButton final : public QToolButton {
public:
    using QToolButton::QToolButton;
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        const bool ownSurface = property("finderGlassSurface").toBool();
        const bool hover = underMouse() && isEnabled();
        if (ownSurface || isChecked() || hover || isDown())
            FinderGlass::paint(painter, rect(), isChecked() || hover, isDown());
        QStyleOptionToolButton option; initStyleOption(&option);
        option.state &= ~(QStyle::State_MouseOver | QStyle::State_Sunken | QStyle::State_On | QStyle::State_HasFocus);
        option.features &= ~QStyleOptionToolButton::HasMenu;
        if (FinderAppearance::highContrast() && isChecked())
            option.palette.setColor(QPalette::ButtonText, FinderAppearance::palette().color(QPalette::HighlightedText));
        style()->drawControl(QStyle::CE_ToolButtonLabel, &option, &painter, this);
        if (hasFocus()) {
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setBrush(Qt::NoBrush); painter.setPen(QPen(FinderAppearance::color(FinderAppearance::Focus), 1.5));
            painter.drawPath(FinderGlass::contour(QRectF(rect()).adjusted(2, 2, -2, -2)));
        }
    }
    void enterEvent(QEnterEvent *event) override { QToolButton::enterEvent(event); update(); }
    void leaveEvent(QEvent *event) override { QToolButton::leaveEvent(event); update(); }
};
