// SPDX-License-Identifier: MIT
#include "windowframe.h"
#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QWidget>
#include <QWindow>

namespace {
class Frame : public QObject {
public:
    explicit Frame(QWidget *window) : QObject(window), target(window) {
        qApp->installEventFilter(this);
        watch(window);
        for (auto *widget : window->findChildren<QWidget *>()) watch(widget);
    }
    ~Frame() override { restoreCursor(); }
private:
    QPointer<QWidget> target, cursorWidget;
    QCursor previousCursor;
    bool hadCursor = false;
    void watch(QWidget *widget) {
        if (widget->window() == target) widget->setAttribute(Qt::WA_Hover);
    }
    void restoreCursor() {
        if (cursorWidget) {
            if (hadCursor) cursorWidget->setCursor(previousCursor);
            else cursorWidget->unsetCursor();
        }
        cursorWidget.clear();
    }
    Qt::Edges edgesAt(QPoint point) const {
        if (!target || target->isMaximized() || target->isFullScreen() ||
            !target->rect().contains(point)) return {};
        Qt::Edges edges;
        if (point.x() < 5) edges |= Qt::LeftEdge;
        if (point.x() >= target->width() - 5) edges |= Qt::RightEdge;
        if (point.y() < 5) edges |= Qt::TopEdge;
        if (point.y() >= target->height() - 5) edges |= Qt::BottomEdge;
        return edges;
    }
    bool eventFilter(QObject *object, QEvent *event) override {
        auto *widget = qobject_cast<QWidget *>(object);
        if (!target || !widget || widget->window() != target) return false;
        if (event->type() == QEvent::Show || event->type() == QEvent::ChildPolished) watch(widget);
        if (event->type() == QEvent::WindowStateChange || event->type() == QEvent::Hide ||
            event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Leave) restoreCursor();
        if (QApplication::activeModalWidget() || QApplication::activePopupWidget()) { restoreCursor(); return false; }
        if (event->type() == QEvent::MouseButtonPress) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() != Qt::LeftButton) return false;
            const auto edges = edgesAt(target->mapFromGlobal(mouse->globalPosition().toPoint()));
            return edges && target->windowHandle() && target->windowHandle()->startSystemResize(edges);
        }
        if (event->type() == QEvent::HoverMove || event->type() == QEvent::MouseMove) {
            const QPoint point = event->type() == QEvent::HoverMove
                ? widget->mapTo(target, static_cast<QHoverEvent *>(event)->position().toPoint())
                : target->mapFromGlobal(static_cast<QMouseEvent *>(event)->globalPosition().toPoint());
            const auto edges = edgesAt(point);
            if (!edges) { restoreCursor(); return false; }
            Qt::CursorShape shape = Qt::SizeVerCursor;
            if ((edges.testFlag(Qt::TopEdge) && edges.testFlag(Qt::LeftEdge)) ||
                (edges.testFlag(Qt::BottomEdge) && edges.testFlag(Qt::RightEdge))) shape = Qt::SizeFDiagCursor;
            else if ((edges.testFlag(Qt::TopEdge) && edges.testFlag(Qt::RightEdge)) ||
                     (edges.testFlag(Qt::BottomEdge) && edges.testFlag(Qt::LeftEdge))) shape = Qt::SizeBDiagCursor;
            else if (edges.testFlag(Qt::LeftEdge) || edges.testFlag(Qt::RightEdge)) shape = Qt::SizeHorCursor;
            if (cursorWidget != widget) {
                restoreCursor(); cursorWidget = widget;
                hadCursor = widget->testAttribute(Qt::WA_SetCursor); previousCursor = widget->cursor();
            }
            widget->setCursor(shape);
        }
        return false;
    }
};
}
void FinderWindowFrame::install(QWidget *window) {
    if (!window || window->property("finderWindowFrameInstalled").toBool()) return;
    window->setProperty("finderWindowFrameInstalled", true);
    new Frame(window);
}
