// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#pragma once
#include "appearance.h"
#include "windowcontrols.h"
#include "windowcontour.h"
#include <QApplication>
#include <QDialog>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QWindow>

// Shared presentation around existing Qt/KDE dialogs and menus. Contents,
// cancellation, default actions, conflicts and authentication stay native.
namespace FinderPopupPresentation {
inline QString controlsStyle() {
    const auto p = FinderAppearance::palette();
    return QStringLiteral(
        "QDialog { background:transparent;color:palette(window-text); }"
        "QDialog QLabel { background:transparent; }"
        "QDialog QPushButton { min-height:20px;padding:4px 14px;border:1px solid %1;border-radius:14px;background:%2;color:%3; }"
        "QDialog QPushButton:hover { background:%4; } QDialog QPushButton:pressed { background:%5; }"
        "QDialog QPushButton:default { background:%6;color:%7;border-color:%6; }"
        "QDialog QPushButton:disabled { color:%9;background:palette(window); }"
        "QDialog QPushButton[finderKeyboardFocus=true]:focus { border:2px solid %8; }"
        "QDialog QLineEdit,QDialog QComboBox,QDialog QSpinBox,QDialog QDoubleSpinBox { min-height:20px;padding:3px 8px;border:1px solid %1;border-radius:6px;background:palette(base);color:palette(text); }"
        "QDialog QLineEdit:focus,QDialog QComboBox:focus,QDialog QSpinBox:focus { border-color:%8; }"
        "QDialog QComboBox::drop-down { border:0;width:20px; }"
        "QDialog QGroupBox { border:0;font-weight:600;margin-top:16px; } QDialog QGroupBox::title { subcontrol-origin:margin; }"
        "QDialog QTabWidget::pane { border:0; }"
        "QMenu { background:palette(window);color:palette(window-text);border:1px solid %1;border-radius:12px;padding:6px; }"
        "QMenu::item { min-height:18px;padding:3px 14px;border-radius:8px;background:transparent; }"
        "QMenu::item:selected { background:%6;color:%7; } QMenu::item:disabled { color:%9; }"
        "QMenu::separator { height:1px;margin:5px 8px;background:%1; }")
        .arg(p.color(QPalette::Mid).name(),p.color(QPalette::Button).name(),p.color(QPalette::ButtonText).name(),
             p.color(QPalette::Button).lighter(110).name(),p.color(QPalette::Button).darker(110).name(),
             p.color(QPalette::Highlight).name(),p.color(QPalette::HighlightedText).name(),FinderAppearance::color(FinderAppearance::Focus).name(),p.color(QPalette::Disabled,QPalette::Text).name());
}

class Surface final : public QWidget {
public:
    explicit Surface(QDialog *dialog) : QWidget(dialog) {
        setObjectName("finderDialogSurface");
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        connect(FinderAppearance::instance(),&FinderAppearance::changed,this,qOverload<>(&QWidget::update));
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);painter.setBrush(FinderAppearance::palette().color(QPalette::Window));
        painter.drawPath(windowContour(QRectF(rect()),16));
    }
};

class Header final : public QWidget {
public:
    explicit Header(QDialog *dialog) : QWidget(dialog), target(dialog) {
        setObjectName("finderDialogHeader");setFixedHeight(40);
        auto *row = new QHBoxLayout(this);row->setContentsMargins(14,4,14,4);row->setSpacing(10);
        caption = new QLabel(dialog->windowTitle(),this);caption->setObjectName("finderDialogTitle");
        caption->setTextFormat(Qt::PlainText);caption->setAlignment(Qt::AlignCenter);caption->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
        QFont font=caption->font();font.setWeight(QFont::DemiBold);caption->setFont(font);
        controls = new WindowControls(this);row->addWidget(caption,1);row->addWidget(controls);
        connect(controls->findChild<QToolButton *>("window-close"),&QToolButton::clicked,dialog,&QDialog::close);
        controls->findChild<QToolButton *>("window-minimize")->setEnabled(false);
        controls->findChild<QToolButton *>("window-maximize")->setEnabled(false);
        const auto reorder=[this,row] {row->removeWidget(controls);row->insertWidget(FinderAppearance::leftWindowControls()?0:row->count(),controls);};
        reorder();connect(FinderAppearance::instance(),&FinderAppearance::changed,this,reorder);
        connect(dialog,&QWidget::windowTitleChanged,caption,&QLabel::setText);
    }
protected:
    void mousePressEvent(QMouseEvent *event) override {
        if(event->button()==Qt::LeftButton && target && target->windowHandle()) {target->windowHandle()->startSystemMove();event->accept();}
        else QWidget::mousePressEvent(event);
    }
private:
    QPointer<QDialog> target;QLabel *caption;WindowControls *controls;
};

class Manager final : public QObject {
public:
    explicit Manager(QApplication *app) : QObject(app) {app->installEventFilter(this);}
protected:
    bool eventFilter(QObject *object,QEvent *event) override {
        auto *widget=qobject_cast<QWidget *>(object);
        if(!widget)return false;
        if(event->type()==QEvent::Polish) {
            if(auto *menu=qobject_cast<QMenu *>(widget)) {
                menu->setAttribute(Qt::WA_TranslucentBackground);
                if(!menu->property("finderRoundedPopup").toBool()) {
                    menu->setProperty("finderRoundedPopup",true);
                    if(!menu->graphicsEffect())menu->setGraphicsEffect(new ContourEffect(menu));
                }
            } else if(auto *dialog=qobject_cast<QDialog *>(widget);dialog && !dialog->property("finderDialogPresented").toBool()
                      && !dialog->windowFlags().testFlag(Qt::FramelessWindowHint) && !dialog->graphicsEffect()) {
                auto *layout=dialog->layout();if(!layout)return false;
                dialog->setProperty("finderDialogPresented",true);
                const QMargins original=layout->contentsMargins();
                layout->setContentsMargins(original.left(),original.top()+40,original.right(),original.bottom());
                dialog->setWindowFlag(Qt::FramelessWindowHint,true);dialog->setAttribute(Qt::WA_TranslucentBackground);
                // Paint the rounded background in a child. A top-level
                // graphics effect cannot reliably clip QWidget::grab() or
                // the platform window's own background.
                auto *surface=new Surface(dialog);surface->setGeometry(dialog->rect());surface->show();surface->lower();
                auto *header=new Header(dialog);header->setGeometry(0,0,dialog->width(),40);header->show();
            }
        }
        if(event->type()==QEvent::Resize && widget->property("finderDialogPresented").toBool()) {
            if(auto *surface=widget->findChild<QWidget *>("finderDialogSurface",Qt::FindDirectChildrenOnly))surface->setGeometry(widget->rect());
            if(auto *header=widget->findChild<QWidget *>("finderDialogHeader",Qt::FindDirectChildrenOnly))header->setGeometry(0,0,widget->width(),40);
        }
        if(event->type()==QEvent::MouseButtonPress && widget->window()->property("finderDialogPresented").toBool()) {
            auto *window=widget->window();auto *mouse=static_cast<QMouseEvent *>(event);
            const QPoint point=window->mapFromGlobal(mouse->globalPosition().toPoint());Qt::Edges edges;
            if(window->minimumWidth()!=window->maximumWidth()) {
                if(point.x()<5)edges|=Qt::LeftEdge;
                if(point.x()>=window->width()-5)edges|=Qt::RightEdge;
            }
            if(window->minimumHeight()!=window->maximumHeight()) {
                if(point.y()<5)edges|=Qt::TopEdge;
                if(point.y()>=window->height()-5)edges|=Qt::BottomEdge;
            }
            if(mouse->button()==Qt::LeftButton && edges && window->windowHandle()
                && window->windowHandle()->startSystemResize(edges))return true;
        }
        return false;
    }
};
inline void install(QApplication *application) {
    if(!application || application->property("finderPopupPresentationInstalled").toBool())return;
    application->setProperty("finderPopupPresentationInstalled",true);
    const auto restyle=[application] {application->setStyleSheet(controlsStyle());};restyle();
    QObject::connect(FinderAppearance::instance(),&FinderAppearance::changed,application,restyle);
    new Manager(application);
}
}
