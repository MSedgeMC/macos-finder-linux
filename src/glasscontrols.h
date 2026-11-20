// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#pragma once
#include "appearance.h"
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QToolButton>
#include <QStyleOptionToolButton>

// Original content-backed Qt coating for shared toolbar controls. Finder's
// opaque header is the backdrop here; sidebar material separately uses KWin.
// No wallpaper holes, captures, refractive claims or idle timers.
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
    const QColor fill = FinderAppearance::color(active ? FinderAppearance::ToolbarActive : FinderAppearance::ToolbarSurface);
    QLinearGradient body(bounds.topLeft(), bounds.bottomLeft());
    body.setColorAt(0, pressed ? fill.darker(110) : fill.lighter(dark ? 124 : 104));
    body.setColorAt(.48, pressed ? fill.darker(115) : fill);
    body.setColorAt(1, pressed ? fill.darker(110) : fill.lighter(dark ? 104 : 101));
    p.fillPath(path, body);
    // A subtle meniscus follows the shared capsule. The selected segment
    // uses the same neutral family rather than a bright nested glass lens.
    const QColor edge = FinderAppearance::color(FinderAppearance::ToolbarEdge);
    QLinearGradient rim(bounds.topLeft(), bounds.bottomLeft());
    rim.setColorAt(0, edge.lighter(dark ? 115 : 105));
    rim.setColorAt(.35, fill.lighter(dark ? 108 : 102));
    rim.setColorAt(.65, fill);
    rim.setColorAt(1, edge.lighter(dark ? 108 : 102));
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
        painter.setPen(QPen(FinderAppearance::color(FinderAppearance::ToolbarEdge), .7));
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
        if (hasFocus() && property("finderKeyboardFocus").toBool()) {
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setBrush(Qt::NoBrush); painter.setPen(QPen(FinderAppearance::color(FinderAppearance::Focus), 1.5));
            painter.drawPath(FinderGlass::contour(QRectF(rect()).adjusted(2, 2, -2, -2)));
        }
    }
    void enterEvent(QEnterEvent *event) override { QToolButton::enterEvent(event); update(); }
    void leaveEvent(QEvent *event) override { QToolButton::leaveEvent(event); update(); }
};
