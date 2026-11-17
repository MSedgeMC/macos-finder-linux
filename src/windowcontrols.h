// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#pragma once
#include "appearance.h"
#include <QToolButton>
#include <QPainter>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QMenu>

// Original vector traffic lights. Native window actions are supplied by the
// host; no image assets, backdrop capture or animation timer are needed.
class WindowControl : public QToolButton {
public:
    WindowControl(QString kind, QWidget *parent) : QToolButton(parent), kind(std::move(kind)) {
        setFixedSize(20,28); setObjectName("window-" + this->kind);
        setAccessibleName(this->kind == "close" ? "Close" : this->kind == "minimize" ? "Minimize" : "Zoom");
        setToolTip(accessibleName()); setFocusPolicy(Qt::StrongFocus);
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        const bool active=window()->isActiveWindow() && isEnabled();
        const bool contrast=FinderAppearance::highContrast();
        QColor base(kind=="close" ? "#ff5f57" : kind=="minimize" ? "#febc2e" : "#28c840");
        if(!active)base=QColor(FinderAppearance::dark() ? "#747478" : "#b5b5b9");
        if(isDown())base=base.darker(118);
        const QRectF circle(1,7,14,14);
        QLinearGradient body(circle.topLeft(),circle.bottomLeft());
        body.setColorAt(0,base.lighter(123));body.setColorAt(.45,base);body.setColorAt(1,base.darker(116));
        p.setBrush(contrast ? QBrush(base) : QBrush(body));
        p.setPen(QPen(base.darker(contrast ? 170 : 132),contrast ? 1.0 : .65));p.drawEllipse(circle);
        if(!contrast) {
            p.save(); QPainterPath clip;clip.addEllipse(circle.adjusted(.65,.65,-.65,-.65));p.setClipPath(clip);
            QRadialGradient glint(QPointF(6.5,8),10);glint.setColorAt(0,QColor(255,255,255,145));glint.setColorAt(.6,QColor(255,255,255,24));glint.setColorAt(1,Qt::transparent);
            p.setPen(Qt::NoPen);p.setBrush(glint);p.drawEllipse(QRectF(2,7,12,7));
            p.setPen(QPen(QColor(255,255,255,active ? 90 : 38),.65));p.setBrush(Qt::NoBrush);p.drawArc(circle.adjusted(1,1,-1,-1),25*16,130*16);p.restore();
        }
        // Persistent, crisp symbols also identify controls without color.
        p.setPen(QPen(QColor(25,25,25,contrast ? 255 : active ? 220 : 140),1.2,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
        if(kind=="close") {p.drawLine(QPointF(5.4,11.4),QPointF(10.6,16.6));p.drawLine(QPointF(10.6,11.4),QPointF(5.4,16.6));}
        else if(kind=="minimize")p.drawLine(QPointF(5,14),QPointF(11,14));
        else {
            // Diagonal expand/restore corners, rather than an unrelated plus.
            const bool restore=window()->isMaximized() || window()->isFullScreen();
            const qreal s=restore ? -1 : 1;
            const QPointF a(6,12),b(10,16);
            p.drawLine(a,QPointF(a.x()-2*s,a.y()));p.drawLine(a,QPointF(a.x(),a.y()-2*s));
            p.drawLine(b,QPointF(b.x()+2*s,b.y()));p.drawLine(b,QPointF(b.x(),b.y()+2*s));
        }
        if(hasFocus()) {p.setPen(QPen(FinderAppearance::color(FinderAppearance::Focus),1.3));p.setBrush(Qt::NoBrush);p.drawEllipse(circle.adjusted(-1.8,-1.8,1.8,1.8));}
    }
    void enterEvent(QEnterEvent *event) override {QToolButton::enterEvent(event);update();}
    void leaveEvent(QEvent *event) override {QToolButton::leaveEvent(event);update();}
    void contextMenuEvent(QContextMenuEvent *event) override {
        if(menu()){menu()->exec(event->globalPos());event->accept();}else QToolButton::contextMenuEvent(event);
    }
    void mousePressEvent(QMouseEvent *event) override {
        if(kind=="maximize" && menu() && event->button()==Qt::LeftButton) {
            for(auto *action:menu()->actions())
                if(action->objectName()==(event->modifiers().testFlag(Qt::AltModifier) ? "zoom" : "fullscreen")) {
                    action->trigger();event->accept();return;
                }
        }
        QToolButton::mousePressEvent(event);
    }
private:
    QString kind;
};
