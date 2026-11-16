// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#include "scroll.h"

#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QPointer>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QStyle>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace FinderScroll {
namespace {

class Controller final : public QObject {
public:
    Controller(QAbstractScrollArea *area, const Options &options)
        : QObject(area), m_area(area), m_options(options) {
        setObjectName(QStringLiteral("finderSmoothScroll"));
        m_frames.setParent(this);
        m_frames.setObjectName(QStringLiteral("finderSmoothScrollFrames"));
        m_frames.setTimerType(Qt::PreciseTimer);
        m_frames.setInterval(16);
        m_clock.start();
        connect(&m_frames, &QTimer::timeout, this, [this] {
            if (!inputAvailable()) stop();
            else advance(m_clock.nsecsElapsed() / 1000000.0);
        });
        connect(qApp, &QGuiApplication::applicationStateChanged, this,
                [this](Qt::ApplicationState state) {
            if (state != Qt::ApplicationActive) stop();
        });
        area->installEventFilter(this);
        syncWidgets();
    }

    ~Controller() override { removeGlobalFilter(); }

    void configure(const Options &options) {
        stop();
        m_options = options;
        syncWidgets();
    }

    void stop() {
        m_horizontal.running = m_vertical.running = false;
        m_horizontal.remainder = m_vertical.remainder = 0;
        m_frames.stop();
        removeGlobalFilter();
    }

    bool isAnimating() const { return m_frames.isActive(); }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        // While animating only, catch input elsewhere in the same window
        // before native selection, menus, navigation or a drag can run.
        // No application-wide filter remains installed while idle.
        switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
        case QEvent::KeyPress:
        case QEvent::ShortcutOverride:
        case QEvent::TouchBegin:
        case QEvent::DragEnter:
        case QEvent::DragMove:
        case QEvent::Drop:
        case QEvent::ContextMenu:
            if (auto *widget = qobject_cast<QWidget *>(watched);
                widget && widget->window() == m_window) stop();
            break;
        default: break;
        }

        const bool local = watched == m_area || watched == m_viewport ||
                           watched == m_horizontal.bar || watched == m_vertical.bar;
        if (!local && watched != m_window) return false;

        switch (event->type()) {
        case QEvent::Wheel:
            if (local) return wheel(watched, static_cast<QWheelEvent *>(event));
            break;
        case QEvent::Hide:
        case QEvent::HideToParent:
        case QEvent::WindowDeactivate:
        case QEvent::Close:
        case QEvent::FocusOut:
        case QEvent::StyleChange:
            stop();
            break;
        case QEvent::EnabledChange:
            if (auto *widget = qobject_cast<QWidget *>(watched); widget && !widget->isEnabled()) stop();
            break;
        case QEvent::Resize:
            if (watched == m_viewport || watched == m_area) stop();
            break;
        case QEvent::Show:
        case QEvent::ParentChange:
            syncWidgets();
            break;
        case QEvent::ChildAdded:
        case QEvent::ChildRemoved:
            if (watched == m_area) {
                // setViewport/setScrollBar changes its pointer after child
                // events. Queue a single refresh, not a polling timer.
                queueSync();
            }
            break;
        default: break;
        }
        return false;
    }

private:
    struct Axis {
        QPointer<QScrollBar> bar;
        double start = 0;
        double target = 0;
        double position = 0;
        double velocity = 0;
        double tangent = 3;
        double started = 0;
        double duration = 180;
        double remainder = 0;
        int direction = 0;
        bool running = false;
    };

    QAbstractScrollArea *m_area;
    Options m_options;
    QPointer<QWidget> m_viewport;
    QPointer<QWidget> m_window;
    Axis m_horizontal, m_vertical;
    QTimer m_frames;
    QElapsedTimer m_clock;
    QScrollBar *m_writingBar = nullptr;
    int m_writtenValue = 0;
    bool m_globalFilter = false;
    bool m_syncQueued = false;

    bool inputAvailable() const {
        return m_area->isVisible() && m_area->isEnabled() &&
               m_viewport && m_viewport->isVisible() && m_viewport->isEnabled() &&
               !m_area->window()->isMinimized() &&
               QGuiApplication::applicationState() == Qt::ApplicationActive;
    }

    void removeGlobalFilter() {
        if (m_globalFilter && qApp) qApp->removeEventFilter(this);
        m_globalFilter = false;
    }

    void queueSync() {
        if (m_syncQueued) return;
        m_syncQueued = true;
        QMetaObject::invokeMethod(this, [this] {
            m_syncQueued = false;
            syncWidgets();
        }, Qt::QueuedConnection);
    }

    void syncWidgets() {
        if (m_viewport != m_area->viewport()) {
            stop();
            if (m_viewport) m_viewport->removeEventFilter(this);
            m_viewport = m_area->viewport();
            if (m_viewport) m_viewport->installEventFilter(this);
        }
        bind(m_horizontal, m_area->horizontalScrollBar());
        bind(m_vertical, m_area->verticalScrollBar());
        if (m_window != m_area->window()) {
            stop();
            if (m_window && m_window != m_area) m_window->removeEventFilter(this);
            m_window = m_area->window();
            if (m_window && m_window != m_area) m_window->installEventFilter(this);
        }
    }

    void bind(Axis &axis, QScrollBar *bar) {
        if (axis.bar == bar) return;
        stop();
        if (axis.bar) {
            axis.bar->removeEventFilter(this);
            disconnect(axis.bar, nullptr, this, nullptr);
        }
        axis = Axis{};
        axis.bar = bar;
        if (!bar) return;
        bar->installEventFilter(this);
        connect(bar, &QScrollBar::valueChanged, this, [this, bar](int value) {
            // Native scrollbar actions, scrollTo, autoscroll and navigation
            // always take ownership. Detect even a reentrant external write.
            if (m_writingBar != bar || m_writtenValue != value) stop();
        });
        connect(bar, &QScrollBar::rangeChanged, this, [this] { stop(); });
        connect(bar, &QScrollBar::sliderPressed, this, [this] { stop(); });
        connect(bar, &QScrollBar::actionTriggered, this, [this] { stop(); });
        connect(bar, &QObject::destroyed, this, [this] {
            stop();
            queueSync(); // Replacements live inside Qt's scrollbar container.
        });
    }

    int animationDuration() const {
        if (m_options.durationMs <= 0) return 0;
        if (!m_options.respectReducedMotion)
            return std::clamp(m_options.durationMs, 16, 1200);
        // Query the global hint, not the tree-view hint: Qt's common style
        // disables tree expansion animation independently of reduced motion.
        if (m_area->style()->styleHint(QStyle::SH_Widget_Animation_Duration) == 0)
            return 0;
        double factor = 1;
        // KDE's public config format, respecting user/system XDG precedence.
        // Qt 6.8–6.10 exposes no general reduce-motion accessibility hint.
        for (const auto &directory : QStandardPaths::standardLocations(QStandardPaths::ConfigLocation)) {
            QSettings settings(directory + QStringLiteral("/kdeglobals"), QSettings::IniFormat);
            const QVariant value = settings.value(QStringLiteral("KDE/AnimationDurationFactor"));
            if (!value.isValid()) continue;
            bool valid = false;
            const double configured = value.toDouble(&valid);
            if (valid && std::isfinite(configured) && configured >= 0) factor = configured;
            break;
        }
        if (factor == 0) return 0;
        return qRound(std::clamp(double(m_options.durationMs) * factor, 16.0, 1200.0));
    }

    void write(Axis &axis, double position) {
        if (!axis.bar) return;
        m_writtenValue = qRound(std::clamp(position, double(axis.bar->minimum()), double(axis.bar->maximum())));
        m_writingBar = axis.bar;
        axis.bar->setValue(m_writtenValue);
        m_writingBar = nullptr;
    }

    void advanceAxis(Axis &axis, double now) {
        if (!axis.running || !axis.bar) return;
        const double u = std::clamp((now - axis.started) / axis.duration, 0.0, 1.0);
        const double distance = axis.target - axis.start;
        // Monotone cubic Hermite, ending at zero velocity. New same-direction
        // input preserves current velocity; its tangent is bounded to prevent
        // overshoot. A reversal discards forward backlog immediately.
        const double curve = (-2 * u * u * u + 3 * u * u) +
                             axis.tangent * (u * u * u - 2 * u * u + u);
        const double derivative = (-6 * u * u + 6 * u) +
                                  axis.tangent * (3 * u * u - 4 * u + 1);
        axis.position = axis.start + distance * curve;
        axis.velocity = distance * derivative / axis.duration;
        if (u >= 1) {
            axis.position = axis.target;
            axis.velocity = 0;
            axis.running = false;
        }
        write(axis, axis.position);
    }

    void advance(double now) {
        advanceAxis(m_horizontal, now);
        advanceAxis(m_vertical, now);
        updateTimer();
    }

    void updateTimer() {
        if (m_horizontal.running || m_vertical.running) {
            if (!m_frames.isActive()) m_frames.start();
            if (!m_globalFilter) {
                qApp->installEventFilter(this);
                m_globalFilter = true;
            }
        } else {
            m_frames.stop();
            removeGlobalFilter();
        }
    }

    bool hasRange(const Axis &axis) const {
        return axis.bar && axis.bar->isEnabled() && axis.bar->minimum() < axis.bar->maximum();
    }

    Axis &angleAxis(QObject *watched, const QPoint &delta, int &amount) {
        if (watched == m_horizontal.bar) {
            amount = std::abs(qint64(delta.x())) > std::abs(qint64(delta.y())) ? delta.x() : delta.y();
            return m_horizontal;
        }
        if (watched == m_vertical.bar) {
            amount = std::abs(qint64(delta.x())) > std::abs(qint64(delta.y())) ? delta.x() : delta.y();
            return m_vertical;
        }
        if (std::abs(qint64(delta.x())) > std::abs(qint64(delta.y()))) {
            amount = delta.x();
            return m_horizontal;
        }
        amount = delta.y();
        if (delta.x() == 0 && (m_options.horizontalWheel ||
                              (!hasRange(m_vertical) && hasRange(m_horizontal)))) return m_horizontal;
        return m_vertical;
    }

    bool pixelAxis(Axis &axis, int delta) {
        if (!delta || !hasRange(axis)) return false;
        const double movement = axis.bar->invertedControls() ? -double(delta) : double(delta);
        const double target = std::clamp(double(axis.bar->value()) + movement,
                                        double(axis.bar->minimum()), double(axis.bar->maximum()));
        if (qRound(target) == axis.bar->value()) return false;
        write(axis, target);
        return true;
    }

    bool pixel(QObject *watched, const QPoint &delta) {
        if (watched == m_horizontal.bar)
            return pixelAxis(m_horizontal, delta.x() != 0 ? delta.x() : delta.y());
        if (watched == m_vertical.bar) return pixelAxis(m_vertical, delta.y());
        if (delta.x() == 0 && (m_options.horizontalWheel ||
                              (!hasRange(m_vertical) && hasRange(m_horizontal))))
            return pixelAxis(m_horizontal, delta.y());
        // Both physical axes retain their native distance, including momentum.
        const bool x = pixelAxis(m_horizontal, delta.x());
        const bool y = pixelAxis(m_vertical, delta.y());
        return x || y;
    }

    void pixelPhase(QObject *watched, const QWheelEvent *event) {
        if (event->phase() != Qt::ScrollBegin && event->phase() != Qt::ScrollEnd) return;
        // Qt's transient native scrollbar listens to begin/end. Some devices
        // include pixels on those events, so relay phase metadata without
        // moving again after the direct pixel write.
        const auto notify = [event](QScrollBar *bar) {
            if (!bar) return;
            QWheelEvent phase(event->position(), event->globalPosition(), {}, {},
                              event->buttons(), event->modifiers(), event->phase(),
                              event->inverted(), event->source(), event->pointingDevice());
            QCoreApplication::sendEvent(bar, &phase);
        };
        if (watched == m_horizontal.bar) notify(m_horizontal.bar);
        else if (watched == m_vertical.bar) notify(m_vertical.bar);
        else if (event->pixelDelta().x() == 0 && (m_options.horizontalWheel ||
                 (!hasRange(m_vertical) && hasRange(m_horizontal)))) notify(m_horizontal.bar);
        else {
            if (event->pixelDelta().x() != 0) notify(m_horizontal.bar);
            if (event->pixelDelta().y() != 0) notify(m_vertical.bar);
        }
    }

    bool enqueue(Axis &axis, int delta, double now, int duration) {
        if (!delta || !hasRange(axis)) return false;
        const int step = std::clamp(axis.bar->singleStep(), 0, qMax(1, m_options.maximumWheelStep));
        const double speed = std::isfinite(m_options.wheelSpeed)
            ? std::clamp(m_options.wheelSpeed, 0.1, 2.0) : 0.5;
        const double movement = (axis.bar->invertedControls() ? -1.0 : 1.0) *
                                double(delta) / 120 * QApplication::wheelScrollLines() *
                                step * speed;
        if (movement == 0) return false;
        const int direction = movement > 0 ? 1 : -1;
        if (axis.direction != direction) axis.remainder = 0;
        const bool continuing = axis.running && axis.direction == direction;
        if (axis.running && !continuing) {
            axis.running = false;
            axis.position = axis.bar->value();
            axis.velocity = 0;
        }
        // Bound each event to one page like QScrollBar, retain fine-angle
        // fractions between events, and never bank movement past an endpoint.
        const double steps = std::clamp(movement + axis.remainder,
                                       -double(axis.bar->pageStep()), double(axis.bar->pageStep()));
        const double whole = std::trunc(steps);
        axis.remainder = steps - whole;
        axis.direction = direction;
        const double current = continuing ? axis.position : axis.bar->value();
        const double base = continuing ? axis.target : current;
        const double target = std::clamp(base + whole, double(axis.bar->minimum()), double(axis.bar->maximum()));
        const bool room = direction > 0 ? base < axis.bar->maximum() : base > axis.bar->minimum();
        if (!room) {
            axis.remainder = 0;
            return continuing; // Finish an already-clamped tail before chaining.
        }
        if (whole == 0) return true; // Accepted fraction; no new timer.
        if (!duration) {
            axis.running = false;
            write(axis, target);
            return true;
        }
        const double distance = target - current;
        axis.tangent = continuing && distance != 0 ?
                           std::clamp(axis.velocity * duration / distance, 0.0, 3.0) : 3.0;
        axis.start = axis.position = current;
        axis.target = target;
        axis.started = now;
        axis.duration = duration;
        axis.running = distance != 0;
        return true;
    }

    bool wheel(QObject *watched, QWheelEvent *event) {
        if (event->modifiers() != Qt::NoModifier || event->buttons() != Qt::NoButton ||
            !inputAvailable()) {
            stop();
            return false;
        }
        if (!event->pixelDelta().isNull()) {
            stop();
            const bool moved = pixel(watched, event->pixelDelta());
            pixelPhase(watched, event);
            event->setAccepted(moved);
            return true;
        }
        if (event->angleDelta().isNull()) {
            if (event->phase() != Qt::NoScrollPhase) stop();
            return false;
        }
        const bool phased = event->phase() != Qt::NoScrollPhase;
        if (phased) {
            // Keep the device's update/momentum cadence, with the same wheel
            // sensitivity as unphased input and no synthetic momentum tail.
            m_horizontal.running = m_vertical.running = false;
        }
        const int duration = phased ? 0 : animationDuration();
        const double now = m_clock.nsecsElapsed() / 1000000.0;
        if (!duration) {
            // A changed accessibility setting cancels outstanding movement.
            m_horizontal.running = m_vertical.running = false;
        } else advance(now);
        int amount = 0;
        Axis &axis = angleAxis(watched, event->angleDelta(), amount);
        event->setAccepted(enqueue(axis, amount, now, duration));
        updateTimer();
        // Ignoring at endpoints permits normal QWidget wheel propagation to
        // an enclosing area. Do not feed it back into this area's native bar.
        return true;
    }
};

Controller *controller(const QAbstractScrollArea *area) {
    if (area) for (auto *child : area->children())
        if (auto *existing = dynamic_cast<Controller *>(child)) return existing;
    return nullptr;
}

} // namespace

void install(QAbstractScrollArea *area, const Options &options) {
    if (!area) return;
    if (auto *view = qobject_cast<QAbstractItemView *>(area)) {
        view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    }
    if (auto *existing = controller(area)) existing->configure(options);
    else new Controller(area, options);
}

Options optionsFromPreferences(bool horizontalWheel) {
    QSettings settings;
    Options options;
    options.durationMs = settings.value("Input/smoothScrolling", true).toBool() &&
                         !settings.value("Input/reduceMotion", false).toBool() ? 180 : 0;
    bool valid = false;
    const int percent = settings.value("Input/wheelSpeed", 50).toInt(&valid);
    options.wheelSpeed = valid ? std::clamp(percent, 10, 200) / 100.0 : 0.5;
    options.horizontalWheel = horizontalWheel;
    return options;
}

void stop(QAbstractScrollArea *area) {
    if (auto *existing = controller(area)) existing->stop();
}

bool isAnimating(const QAbstractScrollArea *area) {
    if (auto *existing = controller(area)) return existing->isAnimating();
    return false;
}

} // namespace FinderScroll
