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
#include <QHBoxLayout>
#include <QApplication>

// Original vector traffic lights. Native window actions are supplied by the
// host; no image assets, backdrop capture or animation timer are needed.
class WindowControl : public QToolButton {
public:
    WindowControl(QString kind, QWidget *parent) : QToolButton(parent), kind(std::move(kind)) {
        setFixedSize(20,28); setObjectName("window-" + this->kind);
        setAccessibleName(this->kind == "close" ? "Close" : this->kind == "minimize" ? "Minimize" : "Zoom");
        setToolTip(accessibleName()); setFocusPolicy(Qt::StrongFocus);
    }
    bool symbolsVisible() const {
        return !FinderAppearance::hoverWindowSymbols() || FinderAppearance::highContrast()
            || (hasFocus() && property("finderKeyboardFocus").toBool())
            || underMouse() || (parentWidget() && parentWidget()->property("windowControlsHovered").toBool());
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        const bool active=window()->isActiveWindow() && isEnabled();
        const bool contrast=FinderAppearance::highContrast();
        QColor base(kind=="close" ? "#ff5f57" : kind=="minimize" ? "#febc2e" : "#28c840");
        if(!active)base=QColor(FinderAppearance::dark() ? "#747478" : "#b5b5b9");
        if(isDown())base=base.darker(118);
        const QRectF circle(3,7,14,14);
        const bool glossy=FinderAppearance::glossyWindowControls() && !contrast;
        QLinearGradient body(circle.topLeft(),circle.bottomLeft());
        body.setColorAt(0,base.lighter(112));body.setColorAt(.42,base);body.setColorAt(.76,base.darker(107));body.setColorAt(1,base.lighter(104));
        p.setBrush(glossy ? QBrush(body) : QBrush(base));
        p.setPen(QPen(base.darker(contrast ? 170 : 132),contrast ? 1.0 : .65));p.drawEllipse(circle);
        if(glossy) {
            p.save(); QPainterPath clip;clip.addEllipse(circle.adjusted(.65,.65,-.65,-.65));p.setClipPath(clip);
            QRadialGradient glint(QPointF(8.5,8),9);glint.setColorAt(0,QColor(255,255,255,70));glint.setColorAt(.6,QColor(255,255,255,12));glint.setColorAt(1,Qt::transparent);
            p.setPen(Qt::NoPen);p.setBrush(glint);p.drawEllipse(QRectF(4,7,12,6));
            p.setPen(QPen(QColor(255,255,255,active ? 55 : 25),.65));p.setBrush(Qt::NoBrush);p.drawArc(circle.adjusted(1,1,-1,-1),25*16,130*16);p.restore();
        }
        // Hover mode reveals the whole group; keyboard focus and high contrast
        // retain non-color identification.
        if(symbolsVisible()) {
        p.save(); p.translate(2,0);
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
        p.restore();
        }
        if(hasFocus() && property("finderKeyboardFocus").toBool()) {p.setPen(QPen(FinderAppearance::color(FinderAppearance::Focus),1.3));p.setBrush(Qt::NoBrush);p.drawEllipse(circle.adjusted(-1.8,-1.8,1.8,1.8));}
    }
    void enterEvent(QEnterEvent *event) override {QToolButton::enterEvent(event);refreshGroup();}
    void leaveEvent(QEvent *event) override {QToolButton::leaveEvent(event);refreshGroup();}
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
    void refreshGroup() {
        update();
        if(parentWidget()) for(auto *button:parentWidget()->findChildren<WindowControl *>(QString(),Qt::FindDirectChildrenOnly)) button->update();
    }
    QString kind;
};

// The shared group changes order without replacing controls or their native
// signal/menu connections. Hosts place this widget at the selected edge.
class WindowControls final : public QWidget {
public:
    explicit WindowControls(QWidget *parent=nullptr) : QWidget(parent) {
        setObjectName("finderWindowControls");
        auto *row=new QHBoxLayout(this);row->setContentsMargins(0,0,0,0);row->setSpacing(3);
        for(const auto &kind:{"close","minimize","maximize"}) row->addWidget(new WindowControl(kind,this));
        setFixedSize(66,28); reorder();
        connect(FinderAppearance::instance(), &FinderAppearance::changed,this,[this] {reorder();});
    }
protected:
    void enterEvent(QEnterEvent *event) override {QWidget::enterEvent(event);setProperty("windowControlsHovered",true);repaintControls();}
    void leaveEvent(QEvent *event) override {QWidget::leaveEvent(event);setProperty("windowControlsHovered",false);repaintControls();}
private:
    void repaintControls() {for(auto *button:findChildren<WindowControl *>())button->update();}
    void reorder() {
        const bool left=FinderAppearance::leftWindowControls();
        if (ordered && lastLeft==left) {repaintControls();return;}
        ordered=true;lastLeft=left;
        auto *row=qobject_cast<QHBoxLayout *>(layout());
        const QStringList order=FinderAppearance::leftWindowControls()
            ? QStringList{"close","minimize","maximize"}:QStringList{"minimize","maximize","close"};
        for(const auto &kind:order) {auto *button=findChild<WindowControl *>("window-"+kind);row->removeWidget(button);row->addWidget(button);}
        QWidget::setTabOrder(findChild<WindowControl *>("window-"+order[0]),findChild<WindowControl *>("window-"+order[1]));
        QWidget::setTabOrder(findChild<WindowControl *>("window-"+order[1]),findChild<WindowControl *>("window-"+order[2]));
        repaintControls();
    }
    bool ordered=false,lastLeft=false;
};
