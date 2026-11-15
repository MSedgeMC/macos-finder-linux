// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
// Standalone Qt6::Widgets test: gestures.cpp/.h require AUTOMOC, no QtTest/KDE.
#include "gestures.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QColumnView>
#include <QEventLoop>
#include <QFile>
#include <QKeyEvent>
#include <QListView>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPointer>
#include <QProxyStyle>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QScroller>
#include <QSettings>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTimer>
#include <QTouchEvent>
#include <QWheelEvent>

#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace {
using Controller = FinderGestures::Controller;
using Direction = Controller::HistoryDirection;

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void pump(int milliseconds = 25) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

bool native(QWidget *target, const QPointingDevice *device, Qt::NativeGestureType type,
            qreal value = 0, int fingers = 2, QPointF delta = {}, QPointF local = {30, 40}) {
    QNativeGestureEvent event(type, device, fingers, local,
                              target->window()->mapFromGlobal(target->mapToGlobal(local)),
                              target->mapToGlobal(local), value, delta);
    event.ignore();
    QApplication::sendEvent(target, &event);
    return event.isAccepted();
}

bool touch(QWidget *target, const QPointingDevice *device, QEvent::Type type, QPointF local,
           ulong timestamp, int count = 1, int id = 1, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    const auto state = type == QEvent::TouchBegin ? QEventPoint::Pressed :
                       type == QEvent::TouchEnd ? QEventPoint::Released : QEventPoint::Updated;
    QList<QEventPoint> points;
    for (int i = 0; i < count; ++i) {
        const QPointF global = target->mapToGlobal(local + QPointF(i * 20, 0));
        points.append(QEventPoint(id + i, state, target->window()->mapFromGlobal(global), global));
    }
    QTouchEvent event(type, device, modifiers, points);
    event.setTimestamp(timestamp);
    event.ignore();
    QApplication::sendEvent(target, &event);
    return event.isAccepted();
}

void cancelTouch(QWidget *target, const QPointingDevice *device) {
    QTouchEvent event(QEvent::TouchCancel, device);
    QApplication::sendEvent(target, &event);
}

bool wheel(QWidget *target, const QPointingDevice *device, QPoint pixels, QPoint angle,
           Qt::ScrollPhase phase, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    const QPointF position(100, 80);
    QWheelEvent event(position, target->mapToGlobal(position), pixels, angle,
                      Qt::NoButton, modifiers, phase, false, Qt::MouseEventNotSynthesized, device);
    event.ignore();
    QApplication::sendEvent(target, &event);
    return event.isAccepted();
}

void click(QWidget *target, QPointF position, const QPointingDevice *device) {
    QMouseEvent press(QEvent::MouseButtonPress, position, target->mapToGlobal(position),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier, device);
    QMouseEvent release(QEvent::MouseButtonRelease, position, target->mapToGlobal(position),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier, device);
    QApplication::sendEvent(target, &press);
    QApplication::sendEvent(target, &release);
}

class NativeWidget : public QWidget {
public:
    using QWidget::QWidget;
    int natives = 0;
protected:
    bool event(QEvent *event) override {
        if (event->type() == QEvent::NativeGesture) {
            ++natives;
            event->ignore();
            return true;
        }
        return QWidget::event(event);
    }
};

class WheelArea : public QAbstractScrollArea {
public:
    using QAbstractScrollArea::QAbstractScrollArea;
    int wheels = 0;
    QPoint pixels;
    QPoint angle;
    Qt::ScrollPhase phase = Qt::NoScrollPhase;
    Qt::KeyboardModifiers modifiers;
protected:
    void wheelEvent(QWheelEvent *event) override {
        ++wheels;
        pixels = event->pixelDelta();
        angle = event->angleDelta();
        phase = event->phase();
        modifiers = event->modifiers();
        event->accept();
    }
};

class AnimationStyle : public QProxyStyle {
public:
    bool reduced = false;
    int styleHint(StyleHint hint, const QStyleOption *option = nullptr,
                  const QWidget *widget = nullptr, QStyleHintReturn *result = nullptr) const override {
        if (hint == QStyle::SH_Widget_Animation_Duration) return reduced ? 0 : 180;
        return QProxyStyle::styleHint(hint, option, widget, result);
    }
};

void pass(const char *message) { std::printf("PASS %s\n", message); }

void show(QWidget *widget) {
    widget->show();
    widget->activateWindow();
    pump();
}

} // namespace

int main(int argc, char **argv) {
    // Refuse to run without the repository's private offscreen runner (or an
    // equally isolated session). All shown widgets remain in that test screen.
    if (qEnvironmentVariable("QT_QPA_PLATFORM") != QStringLiteral("offscreen") ||
        qEnvironmentVariableIsSet("DISPLAY") || qEnvironmentVariableIsSet("WAYLAND_DISPLAY")) {
        std::fputs("Run via tests/run-headless.py; no working GUI access permitted.\n", stderr);
        return 2;
    }
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("FinderGestureTest"));
    QCoreApplication::setApplicationName(QStringLiteral("FinderGestureTest"));
    auto *style = new AnimationStyle;
    app.setStyle(style);
    QPointingDevice pad(QStringLiteral("test pad"), 201, QInputDevice::DeviceType::TouchPad,
                        QPointingDevice::PointerType::Finger, QInputDevice::Capability::Position, 10, 0);
    QPointingDevice otherPad(QStringLiteral("second pad"), 202, QInputDevice::DeviceType::TouchPad,
                             QPointingDevice::PointerType::Finger, QInputDevice::Capability::Position, 10, 0);
    QPointingDevice screen(QStringLiteral("test screen"), 203, QInputDevice::DeviceType::TouchScreen,
                           QPointingDevice::PointerType::Finger, QInputDevice::Capability::Position, 10, 0);
    QPointingDevice mouse(QStringLiteral("test mouse"), 204, QInputDevice::DeviceType::Mouse,
                          QPointingDevice::PointerType::Generic, QInputDevice::Capability::Position, 1, 3);
    try {
        QWidget root;
        root.resize(700, 600);
        NativeWidget surface(&root);
        surface.setGeometry(40, 50, 400, 300);
        NativeWidget inner(&surface);
        inner.setGeometry(10, 20, 100, 100);
        show(&root);
        auto *controller = Controller::install(&root);
        require(controller == Controller::install(&root), "installation is not idempotent");
        require(!Controller::install(nullptr) && controller->parent() == &root, "root ownership contract failed");
        int back = 0, forward = 0, zooms = 0;
        QWidget *zoomTarget = nullptr;
        qreal factor = 0;
        QPointF zoomPosition;
        QObject::connect(controller, &Controller::historyRequested, &root, [&](Direction direction) {
            direction == Direction::Back ? ++back : ++forward;
        });
        QObject::connect(controller, &Controller::zoomRequested, &root,
                         [&](QWidget *target, qreal scale, const QPointF &position) {
            ++zooms; zoomTarget = target; factor = scale; zoomPosition = position;
        });

        native(&surface, &pad, Qt::SwipeNativeGesture, 180);
        require(back == 0, "history was enabled without host opt-in");
        controller->setHistoryEnabled(true);
        native(&inner, &pad, Qt::SwipeNativeGesture, 180);
        native(&inner, &pad, Qt::SwipeNativeGesture, 0);
        native(&inner, &pad, Qt::SwipeNativeGesture, -180);
        require(back == 2 && forward == 1, "Qt swipe angle direction is wrong");
        for (qreal angle : {31., 90., 149., 270., -45.}) native(&inner, &pad, Qt::SwipeNativeGesture, angle);
        native(&inner, &pad, Qt::SwipeNativeGesture, 180, 0);
        native(&inner, &pad, Qt::SwipeNativeGesture, 180, 3);
        native(&inner, &screen, Qt::SwipeNativeGesture, 180);
        native(&inner, &mouse, Qt::SwipeNativeGesture, 180);
        native(&inner, &pad, Qt::SwipeNativeGesture, std::nan(""));
        require(back == 2 && forward == 1, "nonhorizontal/unknown-finger/unsupported swipe navigated");
        native(&inner, &pad, Qt::SwipeNativeGesture, 30);
        native(&inner, &pad, Qt::SwipeNativeGesture, 150);
        require(back == 3 && forward == 2, "horizontal angle acceptance boundary failed");
        pass("discrete native swipe directions, angular threshold and exact two-finger guard");

        native(&inner, &pad, Qt::BeginNativeGesture, 0, 0);
        native(&inner, &pad, Qt::SwipeNativeGesture, 180);
        native(&inner, &pad, Qt::SwipeNativeGesture, 180);
        require(back == 4, "one native sequence navigated twice");
        native(&inner, &pad, Qt::EndNativeGesture, 0, 0);
        native(&inner, &pad, Qt::BeginNativeGesture);
        controller->cancel();
        native(&inner, &pad, Qt::SwipeNativeGesture, 180);
        native(&inner, &pad, Qt::EndNativeGesture);
        require(back == 4, "cancelled native sequence navigated");
        native(&inner, &pad, Qt::BeginNativeGesture, 0, 3);
        native(&inner, &pad, Qt::SwipeNativeGesture, 180);
        native(&inner, &pad, Qt::EndNativeGesture);
        native(&inner, &pad, Qt::BeginNativeGesture);
        native(&inner, &otherPad, Qt::SwipeNativeGesture, 180);
        native(&inner, &otherPad, Qt::EndNativeGesture);
        controller->cancel();
        native(&inner, &pad, Qt::EndNativeGesture);
        require(back == 4, "wrong device/finger count escaped sequence guard");
        native(&inner, &pad, Qt::BeginNativeGesture);
        QEvent deactivate(QEvent::WindowDeactivate);
        QApplication::sendEvent(&root, &deactivate);
        native(&inner, &pad, Qt::SwipeNativeGesture, 180);
        native(&inner, &pad, Qt::EndNativeGesture);
        require(back == 4, "deactivated window navigated from stale input");
        pass("native Begin/End, duplicate prevention, explicit cancel and deactivation");

        controller->setZoomTarget(&surface);
        native(&inner, &pad, Qt::ZoomNativeGesture, .2);
        require(zooms == 0, "orphan zoom update was accepted");
        native(&inner, &pad, Qt::BeginNativeGesture, 0, 0);
        require(native(&inner, &pad, Qt::ZoomNativeGesture, .2, 0), "Wayland unknown-finger pinch was rejected");
        require(zooms == 1 && zoomTarget == &surface && std::abs(factor - 1.2) < 1e-9 &&
                zoomPosition == QPointF(40, 60), "zoom factor/ancestor-local coordinates failed");
        native(&inner, &pad, Qt::ZoomNativeGesture, -.25);
        require(zooms == 2 && std::abs(factor - .75) < 1e-9, "negative pinch delta did not reduce zoom");
        native(&inner, &pad, Qt::ZoomNativeGesture, 0);
        native(&inner, &pad, Qt::ZoomNativeGesture, -1);
        native(&inner, &pad, Qt::ZoomNativeGesture, -2);
        native(&inner, &pad, Qt::ZoomNativeGesture, std::nan(""));
        native(&inner, &pad, Qt::ZoomNativeGesture, INFINITY);
        native(&inner, &pad, Qt::ZoomNativeGesture, .1, 3);
        native(&inner, &pad, Qt::RotateNativeGesture, 45);
        native(&inner, &pad, Qt::PanNativeGesture, 0, 2, {200, 0});
        native(&inner, &pad, Qt::SwipeNativeGesture, 180);
        require(zooms == 2 && back == 4, "invalid pinch/interleaved native types triggered an action");
        controller->cancel();
        native(&inner, &pad, Qt::ZoomNativeGesture, .2);
        native(&inner, &pad, Qt::EndNativeGesture);
        native(&inner, &pad, Qt::ZoomNativeGesture, .2);
        require(zooms == 2, "cancelled or ended pinch kept applying updates");
        controller->setZoomTarget(&inner);
        native(&inner, &screen, Qt::BeginNativeGesture);
        native(&inner, &screen, Qt::ZoomNativeGesture, .1);
        native(&inner, &screen, Qt::EndNativeGesture);
        require(zooms == 3 && zoomTarget == &inner && zoomPosition == QPointF(30, 40), "nearest zoom target failed");
        native(&inner, &pad, Qt::BeginNativeGesture);
        controller->setZoomTarget(&inner, false);
        native(&inner, &pad, Qt::ZoomNativeGesture, .1);
        native(&inner, &pad, Qt::EndNativeGesture);
        require(zooms == 3, "disabled zoom target fell through to ancestor during a stream");
        pass("native pinch factors, phases, target locking, coordinates and invalid deltas");

        WheelArea wheels(&root);
        wheels.setGeometry(0, 350, 450, 200);
        wheels.show();
        pump();
        require(controller->setTouchPanning(&wheels), "wheel fixture panning registration failed");
        native(wheels.viewport(), &pad, Qt::BeginNativeGesture);
        for (auto phase : {Qt::ScrollBegin, Qt::ScrollUpdate, Qt::ScrollMomentum, Qt::ScrollEnd}) {
            wheel(wheels.viewport(), &pad, {160, 0}, {}, phase);
            require(wheels.pixels == QPoint(160, 0) && wheels.angle.isNull() && wheels.phase == phase,
                    "pixel/momentum wheel metadata changed");
        }
        wheel(wheels.viewport(), &mouse, {}, {120, 0}, Qt::NoScrollPhase, Qt::ControlModifier);
        require(wheels.wheels == 5 && wheels.angle == QPoint(120, 0) &&
                wheels.modifiers == Qt::ControlModifier && back == 4 && forward == 2,
                "wheel input was stolen or interpreted as navigation");
        native(wheels.viewport(), &pad, Qt::PanNativeGesture, 0, 2, {-500, 0});
        native(wheels.viewport(), &pad, Qt::EndNativeGesture);
        require(back == 4 && forward == 2, "native pan was mistaken for history");
        pass("unmodified pixel/angle/momentum wheel dispatch and no inferred pan history");

        QWidget outside;
        outside.resize(200, 150);
        show(&outside);
        native(&outside, &pad, Qt::SwipeNativeGesture, 180);
        controller->setZoomTarget(&outside);
        native(&outside, &pad, Qt::BeginNativeGesture);
        native(&outside, &pad, Qt::ZoomNativeGesture, .2);
        native(&outside, &pad, Qt::EndNativeGesture);
        require(back == 4 && zooms == 3, "another window triggered this controller");
        outside.hide();
        root.activateWindow();
        auto *dynamic = new NativeWidget(&root);
        dynamic->setGeometry(450, 30, 150, 150);
        dynamic->show();
        controller->setZoomTarget(dynamic);
        native(dynamic, &pad, Qt::BeginNativeGesture);
        delete dynamic;
        native(&surface, &pad, Qt::ZoomNativeGesture, .2);
        native(&surface, &pad, Qt::EndNativeGesture);
        require(zooms == 3, "deleted target was replaced in an in-progress stream");
        auto *child = new NativeWidget(&root);
        child->show();
        native(child, &pad, Qt::SwipeNativeGesture, 180);
        require(back == 5, "dynamic descendant was not routed");
        delete child;
        pass("root scope, dynamic children and destroyed native gesture targets");

        // Use a real Qt scrolling surface, not a model of scroller state.
        QWidget touchRoot;
        touchRoot.resize(500, 400);
        QScrollArea area(&touchRoot);
        area.setGeometry(0, 0, 450, 350);
        auto *body = new QWidget;
        body->resize(1200, 1200);
        area.setWidget(body);
        show(&touchRoot);
        auto *touchController = Controller::install(&touchRoot);
        const bool originalTouchAttribute = area.viewport()->testAttribute(Qt::WA_AcceptTouchEvents);
        require(touchController->setTouchPanning(&area), "scroll area panning failed to install");
        require(touchController->setTouchPanning(&area), "panning install not idempotent");
        auto *scroller = QScroller::scroller(area.viewport());
        require(QScroller::grabbedGesture(area.viewport()) == Qt::CustomGesture, "a touchpad/mouse recognizer was grabbed");
        area.verticalScrollBar()->setValue(300);
        touch(area.viewport(), &pad, QEvent::TouchBegin, {100, 220}, 100);
        touch(area.viewport(), &pad, QEvent::TouchUpdate, {100, 70}, 150, 2);
        touch(area.viewport(), &pad, QEvent::TouchEnd, {100, 70}, 200, 2);
        require(scroller->state() == QScroller::Inactive && area.verticalScrollBar()->value() == 300,
                "raw touchpad input started touchscreen panning");
        touch(area.viewport(), &screen, QEvent::TouchBegin, {100, 220}, 300, 2);
        touch(area.viewport(), &screen, QEvent::TouchEnd, {100, 220}, 330, 2);
        require(scroller->state() == QScroller::Inactive, "two-finger touchscreen started a one-finger pan");
        require(touch(area.viewport(), &screen, QEvent::TouchBegin, {100, 220}, 400), "real TouchBegin was not accepted");
        require(scroller->state() == QScroller::Pressed, "touchscreen did not press QScroller");
        touch(area.viewport(), &screen, QEvent::TouchUpdate, {100, 219}, 410);
        require(scroller->state() == QScroller::Pressed && area.verticalScrollBar()->value() == 300,
                "subthreshold touch movement scrolled");
        touch(area.viewport(), &screen, QEvent::TouchEnd, {100, 219}, 430);
        require(scroller->state() == QScroller::Inactive, "tap left a kinetic scroll active");
        pass("touchscreen versus touchpad, finger count and real QScroller drag threshold");

        touchController->setReducedMotion(true);
        touch(area.viewport(), &screen, QEvent::TouchBegin, {100, 240}, 500);
        touch(area.viewport(), &screen, QEvent::TouchUpdate, {100, 150}, 530);
        touch(area.viewport(), &screen, QEvent::TouchUpdate, {100, 70}, 570);
        pump(35);
        require(scroller->state() == QScroller::Dragging && area.verticalScrollBar()->value() > 300,
                "direct touch pan did not move real scrollbar");
        const int panned = area.verticalScrollBar()->value();
        touch(area.viewport(), &screen, QEvent::TouchEnd, {100, 65}, 590);
        pump(80);
        require(scroller->state() == QScroller::Inactive && area.verticalScrollBar()->value() == panned,
                "reduced motion left touch inertia running");
        touch(area.viewport(), &screen, QEvent::TouchBegin, {100, 240}, 700);
        touch(area.viewport(), &screen, QEvent::TouchUpdate, {100, 140}, 730);
        cancelTouch(area.viewport(), &screen);
        const int cancelledPosition = area.verticalScrollBar()->value();
        touch(area.viewport(), &screen, QEvent::TouchUpdate, {100, 30}, 760);
        touch(area.viewport(), &screen, QEvent::TouchEnd, {100, 30}, 790);
        pump();
        require(scroller->state() == QScroller::Inactive && area.verticalScrollBar()->value() == cancelledPosition,
                "TouchCancel kept dragging or restarting");
        touch(area.viewport(), &screen, QEvent::TouchBegin, {100, 240}, 800);
        touch(area.viewport(), &screen, QEvent::TouchUpdate, {100, 150}, 830);
        touch(area.viewport(), &screen, QEvent::TouchUpdate, {100, 80}, 850, 2);
        require(scroller->state() == QScroller::Inactive, "additional touch point did not cancel pan");
        touch(area.viewport(), &screen, QEvent::TouchEnd, {100, 80}, 880, 2);
        touch(area.viewport(), &screen, QEvent::TouchBegin, {100, 240}, 900);
        touchController->cancel();
        touch(area.viewport(), &screen, QEvent::TouchUpdate, {100, 80}, 930);
        touch(area.viewport(), &screen, QEvent::TouchEnd, {100, 80}, 950);
        require(scroller->state() == QScroller::Inactive, "explicit cancellation resumed touch pan");
        pass("real touchscreen panning, no reduced-motion tail, TouchCancel and additional-finger cancellation");

        touchController->setReducedMotion(false);
        auto panAndRelease = [&](ulong time) {
            area.verticalScrollBar()->setValue(300);
            touch(area.viewport(), &screen, QEvent::TouchBegin, {100, 240}, time);
            touch(area.viewport(), &screen, QEvent::TouchUpdate, {100, 150}, time + 25);
            touch(area.viewport(), &screen, QEvent::TouchUpdate, {100, 60}, time + 50);
            pump(25);
            touch(area.viewport(), &screen, QEvent::TouchEnd, {100, 55}, time + 55);
        };
        panAndRelease(1000);
        require(scroller->state() == QScroller::Scrolling, "normal touch flick did not allow native QScroller inertia");
        wheel(area.viewport(), &pad, {0, -10}, {}, Qt::ScrollMomentum);
        require(scroller->state() == QScroller::Inactive, "native wheel did not take over from touch inertia");
        QSettings().setValue(QStringLiteral("Input/reduceMotion"), true);
        panAndRelease(1100);
        require(scroller->state() == QScroller::Inactive, "host reduced-motion setting was ignored");
        QSettings().remove(QStringLiteral("Input/reduceMotion"));
        style->reduced = true;
        panAndRelease(1200);
        require(scroller->state() == QScroller::Inactive, "Qt reduced-motion style hint was ignored");
        style->reduced = false;
        const QString globals = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + QStringLiteral("/kdeglobals");
        { QSettings settings(globals, QSettings::IniFormat); settings.setValue(QStringLiteral("KDE/AnimationDurationFactor"), 0); }
        panAndRelease(1300);
        require(scroller->state() == QScroller::Inactive, "KDE reduced-motion setting was ignored");
        QFile::remove(globals);
        pass("native touch inertia, wheel takeover and dynamic host/Qt/KDE reduced-motion settings");

        auto *button = new QPushButton(QStringLiteral("Keep native input"), body);
        button->setGeometry(50, 350, 200, 40);
        button->show();
        area.verticalScrollBar()->setValue(300);
        pump();
        const QPointF onButton = area.viewport()->mapFromGlobal(button->mapToGlobal(QPointF(20, 20)));
        touch(area.viewport(), &screen, QEvent::TouchBegin, onButton, 1500);
        touch(area.viewport(), &screen, QEvent::TouchEnd, onButton, 1520);
        require(scroller->state() == QScroller::Inactive, "touch pan stole interactive child input");
        int clicks = 0;
        QObject::connect(button, &QPushButton::clicked, [&] { ++clicks; });
        click(button, {20, 20}, &mouse);
        require(clicks == 1, "ordinary button input was stolen");
        require(touchController->setTouchPanning(&area, false) && !QScroller::hasScroller(area.viewport()) &&
                area.viewport()->testAttribute(Qt::WA_AcceptTouchEvents) == originalTouchAttribute,
                "panning unregister did not restore viewport");
        auto *external = QScroller::scroller(area.viewport());
        require(!touchController->setTouchPanning(&area), "pre-existing scroller was taken over");
        delete static_cast<QObject *>(external);
        require(touchController->setTouchPanning(&area), "panning could not be reinstalled");
        QPointer<QScroller> oldScroller = QScroller::scroller(area.viewport());
        touch(area.viewport(), &screen, QEvent::TouchBegin, {100, 240}, 1550);
        touchRoot.hide();
        require(oldScroller->state() == QScroller::Inactive, "hiding an ancestor did not stop touch input");
        show(&touchRoot);
        // Replacing the viewport destroys its controller-owned scroller;
        // registering the replacement never accesses that old pointer.
        area.setViewport(new QWidget);
        require(!oldScroller, "viewport replacement retained the old scroller");
        require(touchController->setTouchPanning(&area), "replacement viewport could not register");
        touchController->setTouchPanning(&area, false);
        pass("interactive child and mouse compatibility, unregister restoration and external-scroller ownership");

        QWidget itemRoot;
        itemRoot.resize(600, 450);
        QStandardItemModel model;
        for (int i = 0; i < 100; ++i) model.appendRow(new QStandardItem(QStringLiteral("File %1").arg(i)));
        QListView list(&itemRoot);
        list.setGeometry(0, 0, 300, 350);
        list.setModel(&model);
        list.setSelectionMode(QAbstractItemView::ExtendedSelection);
        list.setDragDropMode(QAbstractItemView::DragDrop);
        list.setEditTriggers(QAbstractItemView::NoEditTriggers);
        show(&itemRoot);
        auto *itemController = Controller::install(&itemRoot);
        require(!itemController->setTouchPanning(&list) && !QScroller::hasScroller(list.viewport()),
                "file item view was given a conflicting pan recognizer");
        const auto index = model.index(2, 0);
        click(list.viewport(), list.visualRect(index).center(), &mouse);
        require(list.currentIndex() == index && list.selectionModel()->isSelected(index) &&
                list.dragDropMode() == QAbstractItemView::DragDrop && list.dragEnabled(),
                "native selection or file dragging configuration changed");
        QListView gallery(&itemRoot);
        gallery.setGeometry(300, 0, 280, 160);
        gallery.setModel(&model);
        gallery.setViewMode(QListView::IconMode);
        gallery.setFlow(QListView::LeftToRight);
        gallery.setWrapping(false);
        gallery.setGridSize({80, 100});
        gallery.show();
        QColumnView columns(&itemRoot);
        columns.setGeometry(300, 200, 280, 220);
        columns.setModel(&model);
        columns.show();
        pump();
        itemController->setHistoryEnabled(true);
        int itemHistory = 0;
        QObject::connect(itemController, &Controller::historyRequested, [&] { ++itemHistory; });
        require(!itemController->setTouchPanning(&gallery) && !itemController->setTouchPanning(&columns),
                "Gallery/Columns item views were claimed for direct touch pan");
        require(gallery.horizontalScrollBar()->maximum() > 0, "Gallery fixture cannot scroll horizontally");
        wheel(gallery.viewport(), &pad, {-120, 0}, {-120, 0}, Qt::ScrollUpdate);
        require(gallery.horizontalScrollBar()->value() > 0 && itemHistory == 0,
                "ordinary Gallery horizontal scroll failed or navigated");
        wheel(columns.viewport(), &pad, {-120, 0}, {-120, 0}, Qt::ScrollUpdate);
        require(itemHistory == 0, "ordinary Columns horizontal scroll navigated");
        pass("native file selection/drag configuration and ordinary Gallery/Columns horizontal scrolling");

        // Controllers can be removed/reinstalled, destroyed mid-stream, and
        // deleted reentrantly by a host action without dangling event filters.
        QWidget lifetimeRoot;
        lifetimeRoot.resize(450, 350);
        auto *liveArea = new QScrollArea(&lifetimeRoot);
        liveArea->resize(400, 300);
        auto *liveBody = new QWidget;
        liveBody->resize(800, 1200);
        liveArea->setWidget(liveBody);
        show(&lifetimeRoot);
        QPointer<Controller> lifetime = Controller::install(&lifetimeRoot);
        lifetime->setTouchPanning(liveArea);
        touch(liveArea->viewport(), &screen, QEvent::TouchBegin, {100, 220}, 1600);
        touch(liveArea->viewport(), &screen, QEvent::TouchUpdate, {100, 80}, 1630);
        delete liveArea;
        pump();
        delete lifetime.data();
        require(!lifetime, "deleted controller was retained");
        lifetime = Controller::install(&lifetimeRoot);
        lifetime->setHistoryEnabled(true);
        QObject::connect(lifetime, &Controller::historyRequested, &lifetimeRoot, [&] { delete lifetime.data(); });
        native(&lifetimeRoot, &pad, Qt::SwipeNativeGesture, 180);
        require(!lifetime, "reentrant host action did not delete controller");
        lifetime = Controller::install(&lifetimeRoot);
        lifetime->setZoomTarget(&lifetimeRoot);
        QObject::connect(lifetime, &Controller::zoomRequested, &lifetimeRoot, [&] { delete lifetime.data(); });
        native(&lifetimeRoot, &pad, Qt::BeginNativeGesture);
        native(&lifetimeRoot, &pad, Qt::ZoomNativeGesture, .1);
        require(!lifetime, "reentrant zoom did not delete controller");
        auto *ownedRoot = new QWidget;
        lifetime = Controller::install(ownedRoot);
        delete ownedRoot;
        require(!lifetime, "root destruction leaked controller");
        auto *activeRoot = new QWidget;
        activeRoot->resize(400, 350);
        auto *activeArea = new QScrollArea(activeRoot);
        activeArea->resize(350, 300);
        auto *activeBody = new QWidget;
        activeBody->resize(800, 1200);
        activeArea->setWidget(activeBody);
        show(activeRoot);
        lifetime = Controller::install(activeRoot);
        lifetime->setTouchPanning(activeArea);
        QPointer<QScroller> activeScroller = QScroller::scroller(activeArea->viewport());
        touch(activeArea->viewport(), &screen, QEvent::TouchBegin, {100, 220}, 1700);
        touch(activeArea->viewport(), &screen, QEvent::TouchUpdate, {100, 80}, 1730);
        delete activeRoot;
        require(!lifetime && !activeScroller, "active root destruction retained controller/scroller");
        pump();
        pass("active viewport destruction, controller removal/reinstall, reentrant signals and root ownership");
        std::puts("All portable gesture dispatch tests passed.");
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
    return 0;
}
