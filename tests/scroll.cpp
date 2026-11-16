// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
// Standalone Qt6::Widgets test; no QtTest, KDE libraries or application build.
#include "scroll.h"

#include <QApplication>
#include <QAbstractScrollArea>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QKeyEvent>
#include <QListView>
#include <QMimeData>
#include <QMouseEvent>
#include <QProxyStyle>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QTreeView>
#include <QWheelEvent>
#include <QDragEnterEvent>

#include <cstdio>
#include <stdexcept>

namespace {
void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

void pump(int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

bool wheel(QWidget *target, QPoint angle, QPoint pixels = {},
           Qt::ScrollPhase phase = Qt::NoScrollPhase, bool inverted = false,
           Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    const QPointF position(target->rect().center());
    QWheelEvent event(position, target->mapToGlobal(position.toPoint()), pixels, angle,
                      Qt::NoButton, modifiers, phase, inverted);
    event.ignore();
    QApplication::sendEvent(target, &event);
    return event.isAccepted();
}

class TestArea : public QAbstractScrollArea {
public:
    int wheelEvents = 0;
    int keyEvents = 0;
    int dragEvents = 0;
    Qt::ScrollPhase lastPhase = Qt::NoScrollPhase;
    void reset() {
        FinderScroll::stop(this);
        horizontalScrollBar()->setRange(0, 2000);
        verticalScrollBar()->setRange(0, 2000);
        for (auto *bar : {horizontalScrollBar(), verticalScrollBar()}) {
            bar->setSingleStep(20);
            bar->setPageStep(300);
            bar->setValue(500);
            bar->setInvertedControls(true);
        }
        pump(10); // Drain the native scrollbar visibility/viewport relayout.
    }
protected:
    void wheelEvent(QWheelEvent *event) override {
        ++wheelEvents;
        lastPhase = event->phase();
        QAbstractScrollArea::wheelEvent(event);
    }
    void keyPressEvent(QKeyEvent *event) override {
        ++keyEvents;
        QAbstractScrollArea::keyPressEvent(event);
    }
    void dragEnterEvent(QDragEnterEvent *event) override {
        ++dragEvents;
        event->acceptProposedAction();
    }
};

class TimerProbe : public QObject {
public:
    int deliveries = 0;
protected:
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::Timer) ++deliveries;
        return false;
    }
};

class PhaseBar : public QScrollBar {
public:
    int begins = 0;
    int ends = 0;
protected:
    void wheelEvent(QWheelEvent *event) override {
        if (event->phase() == Qt::ScrollBegin) ++begins;
        if (event->phase() == Qt::ScrollEnd) ++ends;
        QScrollBar::wheelEvent(event);
    }
};

class NoAnimationStyle : public QProxyStyle {
public:
    int styleHint(StyleHint hint, const QStyleOption *option = nullptr,
                  const QWidget *widget = nullptr, QStyleHintReturn *data = nullptr) const override {
        if (hint == SH_Widget_Animation_Duration) return 0;
        return QProxyStyle::styleHint(hint, option, widget, data);
    }
};

void settled(QAbstractScrollArea *area) {
    QElapsedTimer timeout;
    timeout.start();
    while (FinderScroll::isAnimating(area) && timeout.elapsed() < 1500) pump(10);
    require(!FinderScroll::isAnimating(area), "animation failed to settle");
}

void pass(const char *message) { std::printf("PASS %s\n", message); }
} // namespace

int main(int argc, char **argv) {
    // This test always uses offscreen, strips working-session connections and
    // provides its own writable runtime/config/data/cache/state directories.
    // It deliberately performs no physical pointer/keyboard automation.
    QTemporaryDir privateEnvironment;
    if (!privateEnvironment.isValid()) return 2;
    for (const char *name : {"DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "XAUTHORITY",
                            "DBUS_SESSION_BUS_ADDRESS", "DBUS_SESSION_BUS_PID",
                            "DBUS_STARTER_ADDRESS", "DBUS_STARTER_BUS_TYPE", "SESSION_MANAGER",
                            "QT_QPA_PLATFORMTHEME", "QT_QPA_PLATFORM_PLUGIN_PATH", "QT_PLUGIN_PATH"})
        qunsetenv(name);
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_ACCESSIBILITY", "0");
    qputenv("NO_AT_BRIDGE", "1");
    for (const auto &entry : {qMakePair("runtime", "XDG_RUNTIME_DIR"),
                              qMakePair("config", "XDG_CONFIG_HOME"),
                              qMakePair("data", "XDG_DATA_HOME"),
                              qMakePair("cache", "XDG_CACHE_HOME"),
                              qMakePair("state", "XDG_STATE_HOME")}) {
        const QString path = privateEnvironment.path() + '/' + entry.first;
        QDir().mkpath(path);
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        qputenv(entry.second, path.toUtf8());
    }
    qputenv("XDG_CONFIG_DIRS", qgetenv("XDG_CONFIG_HOME"));
    QApplication app(argc, argv);
    QApplication::setStyle(QStringLiteral("Fusion"));
    QApplication::setWheelScrollLines(3);
    try {
        TestArea area;
        area.resize(400, 300);
        area.show();
        area.activateWindow();
        area.setFocus();
        pump(30);
        require(QGuiApplication::applicationState() == Qt::ApplicationActive, "offscreen window inactive");
        FinderScroll::Options options;
        options.wheelSpeed = 1.0;
        options.respectReducedMotion = false;
        FinderScroll::install(&area, options);
        FinderScroll::install(&area, options);
        area.reset();
        auto *v = area.verticalScrollBar();
        auto *h = area.horizontalScrollBar();
        auto *frames = area.findChild<QTimer *>(QStringLiteral("finderSmoothScrollFrames"));
        require(frames && area.findChildren<QTimer *>(QStringLiteral("finderSmoothScrollFrames")).size() == 1,
                "install was not idempotent");
        TimerProbe timerProbe;
        frames->installEventFilter(&timerProbe);

        int changes = 0;
        QObject::connect(v, &QScrollBar::valueChanged, &area, [&](int) { ++changes; });
        require(wheel(area.viewport(), {0, -120}), "wheel event unaccepted");
        require(FinderScroll::isAnimating(&area) && v->value() == 500, "wheel jumped immediately");
        pump(60);
        require(v->value() > 500 && v->value() < 560, "no intermediate wheel position");
        settled(&area);
        require(v->value() == 560 && changes >= 3, "system wheel speed or easing wrong");
        const int finishedDeliveries = timerProbe.deliveries;
        const int finishedChanges = changes;
        pump(250);
        require(!frames->isActive() && timerProbe.deliveries == finishedDeliveries && changes == finishedChanges,
                "idle animation timer/redraw activity");
        std::printf("METRIC wheel value changes=%d, active timer deliveries=%d, idle timer deliveries=0\n",
                    changes, finishedDeliveries);
        pass("finite wheel animation, system speed, intermediate positions, idempotent install and zero idle timers");

        FinderScroll::Options slower;
        slower.respectReducedMotion = false;
        FinderScroll::install(&area, slower);
        area.reset();
        wheel(area.viewport(), {0, -120}); settled(&area);
        require(v->value() == 530 && v->singleStep() == 20, "default wheel speed did not halve distance or changed native step");
        area.reset();
        v->setSingleStep(256);
        wheel(area.viewport(), {0, -120}); settled(&area);
        require(v->value() == 536 && v->singleStep() == 256, "large tile increased bounded wheel distance");
        area.reset();
        v->setSingleStep(256);
        for (int i = 0; i < 120; ++i) wheel(area.viewport(), {0, -1});
        settled(&area);
        require(v->value() == 536, "slower fractional angle input lost distance");
        area.reset();
        v->setSingleStep(256);
        wheel(area.viewport(), {0, -120}, {}, Qt::ScrollUpdate);
        require(v->value() == 536 && !FinderScroll::isAnimating(&area), "phased angle input bypassed speed limit or added inertia");
        wheel(area.viewport(), {0, -120}, {}, Qt::ScrollMomentum);
        require(v->value() == 572 && !FinderScroll::isAnimating(&area), "angle momentum changed cadence or distance");
        wheel(area.viewport(), {}, {0, -17}, Qt::ScrollUpdate);
        require(v->value() == 589, "wheel speed setting changed precise pixels");
        area.reset();
        slower.durationMs = 0;
        FinderScroll::install(&area, slower);
        v->setSingleStep(256);
        wheel(area.viewport(), {0, -120});
        require(v->value() == 536 && !FinderScroll::isAnimating(&area), "disabling smoothing restored fast wheel distance");
        slower.wheelSpeed = 0.25;
        FinderScroll::install(&area, slower);
        area.reset(); v->setSingleStep(256);
        wheel(area.viewport(), {0, -120});
        require(v->value() == 518, "configured wheel speed ignored");
        QSettings preferences;
        preferences.clear();
        require(FinderScroll::optionsFromPreferences().wheelSpeed == 0.5, "preference default speed");
        preferences.setValue("Input/wheelSpeed", 25);
        preferences.setValue("Input/smoothScrolling", false);
        auto configured = FinderScroll::optionsFromPreferences(true);
        require(configured.wheelSpeed == 0.25 && configured.durationMs == 0 && configured.horizontalWheel, "stored wheel preferences");
        preferences.setValue("Input/wheelSpeed", "invalid");
        require(FinderScroll::optionsFromPreferences().wheelSpeed == 0.5, "invalid speed fallback");
        preferences.setValue("Input/wheelSpeed", 500);
        require(FinderScroll::optionsFromPreferences().wheelSpeed == 2.0, "out-of-range speed clamp");
        preferences.clear();
        FinderScroll::install(&area, options);
        pass("bounded default speed, large tiles, fractions, phased momentum, preferences and unchanged precise pixels/native steps");

        area.reset();
        wheel(area.viewport(), {0, -120});
        const int beforeStall = timerProbe.deliveries;
        QThread::msleep(220); // Simulate a busy native layout/thumbnail callback.
        QApplication::processEvents();
        require(v->value() == 560 && !FinderScroll::isAnimating(&area) &&
                    timerProbe.deliveries <= beforeStall + 1, "animation progressed by frame count rather than elapsed time");
        pass("elapsed-time completion after a delayed GUI event loop");

        area.reset();
        wheel(area.viewport(), {0, -120});
        pump(35);
        wheel(area.viewport(), {0, -120});
        pump(35);
        wheel(area.viewport(), {0, -120});
        settled(&area);
        require(v->value() == 680, "same-direction wheel distance lost or accelerated");
        area.reset();
        wheel(area.viewport(), {0, -120});
        pump(40);
        wheel(area.viewport(), {0, 120});
        const int reversalPosition = v->value(); // Time-sampled at this event.
        pump(25);
        require(v->value() < reversalPosition, "reversal kept forward momentum");
        settled(&area);
        require(v->value() == reversalPosition - 60, "reversal retained forward target backlog");
        area.reset();
        for (int i = 0; i < 120; ++i) wheel(area.viewport(), {0, -1});
        settled(&area);
        require(v->value() == 560, "fine angle fractions lost");
        pass("interruptible accumulation, immediate reversal and fractional angle precision");

        area.reset();
        wheel(area.viewport(), {0, -120});
        pump(20);
        const int beforePixels = v->value();
        const int nativeWheels = area.wheelEvents;
        wheel(area.viewport(), {}, {}, Qt::ScrollBegin);
        require(!FinderScroll::isAnimating(&area) && area.wheelEvents == nativeWheels + 1 &&
                    area.lastPhase == Qt::ScrollBegin, "begin phase did not reach native view");
        wheel(area.viewport(), {}, {7, -11}, Qt::ScrollUpdate);
        require(v->value() == beforePixels + 11 && h->value() == 493, "pixel-only two-axis delta not 1:1");
        for (int delta : {-8, -5, -3, -1}) wheel(area.viewport(), {}, {0, delta}, Qt::ScrollMomentum);
        require(v->value() == beforePixels + 28, "native momentum was rescaled");
        wheel(area.viewport(), {}, {}, Qt::ScrollEnd);
        require(area.lastPhase == Qt::ScrollEnd && !FinderScroll::isAnimating(&area), "native end phase gained a tail");
        const int momentumEnd = v->value();
        pump(230);
        require(v->value() == momentumEnd, "second touchpad momentum tail");
        area.reset();
        wheel(area.viewport(), {0, -120}, {}, Qt::ScrollUpdate);
        require(v->value() == 560 && !FinderScroll::isAnimating(&area), "angle-only phase was accelerated/animated");
        pass("pixel-only input and two-axis momentum 1:1, native phases, no added acceleration or tail");

        area.reset();
        wheel(area.viewport(), {0, -120}, {}, Qt::NoScrollPhase, true);
        settled(&area);
        require(v->value() == 560, "inverted flag applied twice to angle delta");
        wheel(area.viewport(), {}, {0, -10}, Qt::ScrollUpdate, true);
        require(v->value() == 570, "inverted flag applied twice to pixel delta");
        v->setInvertedControls(false);
        wheel(area.viewport(), {}, {0, -10});
        require(v->value() == 560, "scrollbar invertedControls ignored");
        wheel(area.viewport(), {0, -120});
        settled(&area);
        require(v->value() == 500, "angle invertedControls ignored");
        pass("Qt natural-scroll delta semantics and scrollbar invertedControls");

        area.reset();
        v->setRange(0, 0);
        pump(10);
        wheel(area.viewport(), {0, -120});
        settled(&area);
        require(h->value() == 560, "horizontal-only view routing failed");
        wheel(area.viewport(), {}, {0, -17}, Qt::ScrollMomentum);
        require(h->value() == 577, "horizontal-only pixel routing failed");
        area.reset();
        options.horizontalWheel = true;
        FinderScroll::install(&area, options);
        wheel(area.viewport(), {0, -120});
        settled(&area);
        require(h->value() == 560 && v->value() == 500, "horizontal-wheel option failed");
        wheel(area.viewport(), {}, {-9, -12}, Qt::ScrollUpdate);
        require(h->value() == 569 && v->value() == 512, "horizontal option corrupted diagonal pixels");
        options.horizontalWheel = false;
        FinderScroll::install(&area, options);
        pass("Gallery/Columns horizontal routing and preserved diagonal input");

        area.reset();
        v->setRange(0, 530);
        wheel(area.viewport(), {0, -120});
        settled(&area);
        require(v->value() == 530 && !wheel(area.viewport(), {0, -120}), "endpoint clamp/propagation failed");
        area.reset();
        wheel(area.viewport(), {0, -120});
        pump(20);
        v->setRange(0, 510);
        const int afterRange = v->value();
        require(!FinderScroll::isAnimating(&area), "range change did not cancel");
        pump(200);
        require(v->value() == afterRange, "stale range-change target replayed");
        area.reset();
        wheel(area.viewport(), {0, -120});
        v->setValue(123);
        pump(200);
        require(v->value() == 123 && !FinderScroll::isAnimating(&area), "external native position lost");
        pass("range clamps, boundary propagation and native/programmatic position ownership");

        area.reset();
        wheel(area.viewport(), {0, -120});
        QKeyEvent key(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
        QApplication::sendEvent(&area, &key);
        require(area.keyEvents == 1 && v->value() == 520 && !FinderScroll::isAnimating(&area),
                "keyboard handler was consumed or animation survived");
        area.reset();
        wheel(area.viewport(), {0, -120});
        QMimeData mime;
        mime.setText(QStringLiteral("native drag fixture"));
        area.setAcceptDrops(true);
        QDragEnterEvent drag(QPoint(20, 20), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(area.viewport(), &drag);
        require(area.dragEvents == 1 && !FinderScroll::isAnimating(&area), "native drag handler blocked");
        area.reset();
        wheel(area.viewport(), {0, -120});
        v->triggerAction(QAbstractSlider::SliderPageStepAdd);
        require(v->value() == 800 && !FinderScroll::isAnimating(&area), "scrollbar action blocked");
        const int priorWheelEvents = area.wheelEvents;
        wheel(area.viewport(), {0, 120}, {}, Qt::NoScrollPhase, false, Qt::ControlModifier);
        require(area.wheelEvents == priorWheelEvents + 1 && !FinderScroll::isAnimating(&area),
                "modifier wheel was intercepted");
        pass("keyboard, drag/drop, scrollbar and modifier handlers stay native");

        area.reset();
        wheel(area.viewport(), {0, -120});
        area.hide();
        require(!FinderScroll::isAnimating(&area), "hidden view still animates");
        area.show();
        area.activateWindow();
        pump(15);
        area.reset();
        wheel(area.viewport(), {0, -120});
        area.setEnabled(false);
        require(!FinderScroll::isAnimating(&area), "disabled view still animates");
        area.setEnabled(true);
        area.reset();
        wheel(area.viewport(), {0, -120});
        QEvent deactivate(QEvent::WindowDeactivate);
        QApplication::sendEvent(&area, &deactivate);
        require(!FinderScroll::isAnimating(&area), "inactive input still animates");
        area.reset();
        wheel(area.viewport(), {0, -120});
        auto *replacement = new QWidget;
        area.setViewport(replacement);
        pump(10);
        require(!FinderScroll::isAnimating(&area), "replaced viewport retained animation");
        wheel(replacement, {}, {0, -13});
        require(v->value() == 513, "replacement viewport not installed");
        wheel(replacement, {0, -120});
        auto *phaseBar = new PhaseBar;
        area.setVerticalScrollBar(phaseBar);
        pump(10);
        v = area.verticalScrollBar();
        require(!FinderScroll::isAnimating(&area), "replaced scrollbar retained animation");
        const int replacedValue = v->value();
        wheel(replacement, {}, {0, -13});
        require(v->value() == replacedValue + 13, "replacement scrollbar not installed");
        wheel(replacement, {}, {0, -3}, Qt::ScrollBegin);
        wheel(replacement, {}, {0, -2}, Qt::ScrollEnd);
        require(phaseBar->begins == 1 && phaseBar->ends == 1 && v->value() == replacedValue + 18 &&
                    !FinderScroll::isAnimating(&area), "pixel begin/end phase notifications lost or moved twice");
        pass("hide, disable, deactivate and replacement viewport/scrollbar cancellation");
        pass("pixel-carrying begin/end reaches native scrollbar once without duplicate movement");

        area.reset();
        options.respectReducedMotion = true;
        FinderScroll::install(&area, options);
        QSettings motion(QString::fromUtf8(qgetenv("XDG_CONFIG_HOME")) + QStringLiteral("/kdeglobals"), QSettings::IniFormat);
        motion.setValue(QStringLiteral("KDE/AnimationDurationFactor"), 0);
        motion.sync();
        wheel(area.viewport(), {0, -120});
        require(v->value() == 560 && !FinderScroll::isAnimating(&area), "KDE reduced motion ignored");
        motion.setValue(QStringLiteral("KDE/AnimationDurationFactor"), 1);
        motion.sync();
        wheel(area.viewport(), {0, -120});
        require(FinderScroll::isAnimating(&area), "live reduced-motion change not read");
        FinderScroll::stop(&area);
        area.setStyle(new NoAnimationStyle);
        area.reset();
        wheel(area.viewport(), {0, -120});
        require(v->value() == 560 && !FinderScroll::isAnimating(&area), "global Qt reduced motion ignored");
        pass("KDE zero animation factor, live preference change and Qt global no-animation style hint");

        QStandardItemModel model;
        for (int i = 0; i < 120; ++i) model.appendRow(new QStandardItem(QStringLiteral("Item %1").arg(i)));
        QTreeView tree;
        tree.setModel(&model);
        tree.setHeaderHidden(true);
        tree.resize(300, 240);
        tree.show();
        tree.activateWindow();
        tree.setFocus();
        pump(20);
        options.respectReducedMotion = false;
        FinderScroll::install(&tree, options);
        tree.verticalScrollBar()->setValue(100);
        require(tree.horizontalScrollMode() == QAbstractItemView::ScrollPerPixel &&
                    tree.verticalScrollMode() == QAbstractItemView::ScrollPerPixel, "view not in pixel modes");
        wheel(tree.viewport(), {0, -120});
        settled(&tree);
        auto index = tree.indexAt(QPoint(40, 50));
        require(index.isValid() && tree.visualRect(index).contains(QPoint(40, 50)), "scrolled hit geometry wrong");
        wheel(tree.viewport(), {0, -120});
        const QPointF click(tree.visualRect(index).center());
        QMouseEvent press(QEvent::MouseButtonPress, click, tree.viewport()->mapToGlobal(click.toPoint()),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree.viewport(), &press);
        QMouseEvent release(QEvent::MouseButtonRelease, click, tree.viewport()->mapToGlobal(click.toPoint()),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(tree.viewport(), &release);
        require(tree.currentIndex() == index && !FinderScroll::isAnimating(&tree), "native item click failed");
        pass("real tree pixel scrolling, selection and consistent visualRect/indexAt geometry");

        QListView gallery;
        gallery.setModel(&model);
        gallery.setViewMode(QListView::IconMode);
        gallery.setFlow(QListView::LeftToRight);
        gallery.setWrapping(false);
        gallery.setGridSize(QSize(90, 100));
        gallery.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        gallery.resize(350, 150);
        gallery.show();
        gallery.activateWindow();
        pump(20);
        options.horizontalWheel = true;
        FinderScroll::install(&gallery, options);
        require(gallery.horizontalScrollBar()->maximum() > 0, "gallery fixture has no horizontal range");
        wheel(gallery.viewport(), {0, -120});
        settled(&gallery);
        require(gallery.horizontalScrollBar()->value() > 0, "real gallery did not scroll");
        pass("real QListView horizontal Gallery");

        auto *destroyed = new TestArea;
        destroyed->resize(300, 200);
        destroyed->show();
        destroyed->activateWindow();
        pump(15);
        FinderScroll::install(destroyed, options);
        destroyed->reset();
        wheel(destroyed->viewport(), {0, -120});
        delete destroyed;
        pump(200);
        pass("destroying an active area releases its timer and input filter safely");
        std::puts("All smooth scroll tests passed.");
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
    return 0;
}
