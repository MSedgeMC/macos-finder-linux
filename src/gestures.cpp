// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#include "gestures.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAbstractSlider>
#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QNativeGestureEvent>
#include <QPointer>
#include <QScrollBar>
#include <QScroller>
#include <QSettings>
#include <QStandardPaths>
#include <QStyle>
#include <QTouchEvent>

#include <algorithm>
#include <cmath>
#include <vector>

namespace FinderGestures {

class Controller::Private {
public:
    explicit Private(Controller *owner, QWidget *widget) : q(owner), root(widget) {
        clock.start();
    }

    struct Pan {
        QPointer<QAbstractScrollArea> area;
        QPointer<QWidget> viewport;
        QPointer<QScroller> scroller;
        bool acceptedTouch = false;
    };

    Controller *q;
    QPointer<QWidget> root;
    std::vector<QPointer<QWidget>> zoomTargets;
    std::vector<Pan> pans;
    QElapsedTimer clock;
    bool history = false;
    bool reducedMotion = false;
    bool nativeActive = false;
    bool nativeCancelled = false;
    bool nativeSwipe = false;
    bool nativeZoom = false;
    QPointer<const QPointingDevice> nativeDevice;
    QPointer<QWidget> nativeTarget;
    QPointer<QWidget> touchTarget;
    QPointer<const QPointingDevice> touchDevice;
    QPointer<QScroller> touchScroller;
    int touchId = -1;
    bool touchCancelled = false;
    bool touchReduced = false;

    bool contains(QWidget *widget) const {
        return root && widget && widget->window() == root->window() &&
               (widget == root || root->isAncestorOf(widget));
    }

    bool available(QWidget *widget) const {
        return contains(widget) && widget->isVisible() && widget->isEnabled() &&
               !root->window()->isMinimized() && !QApplication::activePopupWidget() &&
               (!QApplication::activeModalWidget() ||
                QApplication::activeModalWidget() == root->window());
    }

    QWidget *zoomTarget(QWidget *widget) const {
        for (QWidget *candidate = widget; contains(candidate); candidate = candidate->parentWidget()) {
            if (std::any_of(zoomTargets.begin(), zoomTargets.end(), [candidate](const auto &target) {
                    return target == candidate;
                })) return candidate;
        }
        return nullptr;
    }

    void resetNative() {
        nativeActive = nativeCancelled = nativeSwipe = nativeZoom = false;
        nativeDevice.clear();
        nativeTarget.clear();
    }

    void stopTouch() {
        touchCancelled = true;
        if (touchScroller) touchScroller->stop();
    }

    void resetTouch() {
        stopTouch();
        touchTarget.clear();
        touchDevice.clear();
        touchScroller.clear();
        touchId = -1;
    }

    void cancel() {
        if (nativeActive) nativeCancelled = true;
        stopTouch();
        // Includes kinetic tails after TouchEnd, when touchTarget is cleared.
        for (auto &pan : pans) if (pan.scroller) pan.scroller->stop();
    }

    void release(Pan &pan) {
        if (touchTarget && touchTarget == pan.viewport) resetTouch();
        if (pan.viewport) {
            pan.viewport->setAttribute(Qt::WA_AcceptTouchEvents, pan.acceptedTouch);
            // QAbstractScrollArea's default generic two-point Pan was
            // suspended while our touchscreen-only panner owned this surface.
            pan.viewport->grabGesture(Qt::PanGesture);
        }
        if (pan.scroller) {
            pan.scroller->stop();
            // QScroller is a viewport-owned QObject; no recognizer was grabbed.
            delete static_cast<QObject *>(pan.scroller.data());
        }
    }

    bool reduceMotion(QWidget *widget) const {
        if (reducedMotion || QSettings().value(QStringLiteral("Input/reduceMotion"), false).toBool() ||
            widget->style()->styleHint(QStyle::SH_Widget_Animation_Duration) == 0) return true;
        for (const auto &directory : QStandardPaths::standardLocations(QStandardPaths::ConfigLocation)) {
            QSettings settings(directory + QStringLiteral("/kdeglobals"), QSettings::IniFormat);
            const QVariant value = settings.value(QStringLiteral("KDE/AnimationDurationFactor"));
            if (!value.isValid()) continue;
            bool valid = false;
            const double factor = value.toDouble(&valid);
            return valid && std::isfinite(factor) && factor == 0;
        }
        return false;
    }

    bool native(QWidget *widget, QNativeGestureEvent *event) {
        const auto *device = event->pointingDevice();
        if (!device || (device->type() != QInputDevice::DeviceType::TouchPad &&
                        device->type() != QInputDevice::DeviceType::TouchScreen)) return false;
        const auto type = event->gestureType();
        if (type == Qt::BeginNativeGesture) {
            resetNative();
            nativeActive = true;
            nativeDevice = device;
            nativeTarget = zoomTarget(widget);
            nativeCancelled = !available(widget) ||
                              (event->fingerCount() != 0 && event->fingerCount() != 2);
            if (!nativeCancelled && (history || nativeTarget)) {
                event->accept();
                return true;
            }
            return false;
        }
        if (nativeActive && nativeDevice != device) return false;
        if (type == Qt::EndNativeGesture) {
            const bool handled = nativeActive && !nativeCancelled && (nativeSwipe || nativeZoom);
            resetNative();
            if (handled) event->accept();
            return handled;
        }
        if (!available(widget) || nativeCancelled) return false;
        if (type == Qt::SwipeNativeGesture) {
            if (!history || nativeSwipe || nativeZoom || event->fingerCount() != 2 ||
                device->type() != QInputDevice::DeviceType::TouchPad ||
                !std::isfinite(event->value())) return false;
            const qreal angle = std::remainder(event->value(), 360.0);
            HistoryDirection direction;
            if (std::abs(angle) <= 30) direction = HistoryDirection::Forward;
            else if (std::abs(angle) >= 150) direction = HistoryDirection::Back;
            else return false;
            // Swipe is normally standalone, not a distance stream. Within a
            // Begin..End stream issue at most one history request.
            nativeSwipe = nativeActive;
            for (auto &pan : pans) if (pan.scroller) pan.scroller->stop();
            event->accept();
            Q_EMIT q->historyRequested(direction);
            return true; // A host slot may have deleted q/root.
        }
        if (type != Qt::ZoomNativeGesture || !nativeActive || nativeSwipe || !nativeTarget ||
            !available(nativeTarget) || !zoomTarget(nativeTarget) ||
            (event->fingerCount() != 0 && event->fingerCount() != 2)) return false;
        const qreal factor = 1 + event->value();
        if (!std::isfinite(factor) || factor <= 0 || qFuzzyIsNull(event->value())) return false;
        const QPointF position = nativeTarget->mapFromGlobal(event->globalPosition());
        if (!std::isfinite(position.x()) || !std::isfinite(position.y())) return false;
        nativeZoom = true;
        event->accept();
        Q_EMIT q->zoomRequested(nativeTarget, factor, position);
        return true;
    }

    bool interactiveAt(QWidget *viewport, const QPointF &position) const {
        for (QWidget *child = viewport->childAt(position.toPoint()); child && child != viewport;
             child = child->parentWidget()) {
            if (child->focusPolicy() != Qt::NoFocus || qobject_cast<QAbstractButton *>(child) ||
                qobject_cast<QAbstractSlider *>(child) || qobject_cast<QAbstractItemView *>(child)) return true;
            if (auto *label = qobject_cast<QLabel *>(child);
                label && label->textInteractionFlags() != Qt::NoTextInteraction) return true;
        }
        return false;
    }

    bool touch(QWidget *widget, QTouchEvent *event) {
        const auto *device = event->pointingDevice();
        if (!device || device->type() != QInputDevice::DeviceType::TouchScreen) return false;
        if (event->type() == QEvent::TouchBegin) {
            // Only registered viewports. Their interactive descendants retain
            // native touch/mouse synthesis; no delayed/fake selection clicks.
            auto it = std::find_if(pans.begin(), pans.end(), [widget](const auto &pan) {
                return pan.viewport == widget && pan.area && pan.area->viewport() == widget;
            });
            if (it == pans.end() || !it->scroller || !available(widget) ||
                event->modifiers() != Qt::NoModifier || event->points().size() != 1) return false;
            const auto &point = event->points().first();
            const QPointF position = widget->mapFromGlobal(point.globalPosition());
            const auto hasRange = [](QScrollBar *bar) {
                return bar->isEnabled() && bar->maximum() > bar->minimum();
            };
            if (point.state() != QEventPoint::Pressed || !std::isfinite(position.x()) ||
                !std::isfinite(position.y()) || !widget->rect().contains(position.toPoint()) ||
                interactiveAt(widget, position) ||
                (!hasRange(it->area->horizontalScrollBar()) && !hasRange(it->area->verticalScrollBar()))) return false;
            resetTouch();
            touchTarget = widget;
            touchDevice = device;
            touchScroller = it->scroller;
            touchId = point.id();
            touchCancelled = false;
            touchReduced = reduceMotion(widget);
            touchScroller->stop();
            touchScroller->handleInput(QScroller::InputPress, position, timestamp(event));
            if (touchScroller->state() != QScroller::Pressed) {
                resetTouch();
                return false;
            }
            event->accept();
            return true;
        }
        if (widget != touchTarget || device != touchDevice) return false;
        if (event->type() == QEvent::TouchCancel) {
            resetTouch();
            event->accept();
            return true;
        }
        if (!available(widget) || !touchScroller || event->modifiers() != Qt::NoModifier ||
            event->points().size() != 1 || event->points().first().id() != touchId) stopTouch();
        if (!touchCancelled) {
            const auto &point = event->points().first();
            const QPointF position = widget->mapFromGlobal(point.globalPosition());
            if (!std::isfinite(position.x()) || !std::isfinite(position.y())) stopTouch();
            else touchScroller->handleInput(event->type() == QEvent::TouchEnd ?
                                             QScroller::InputRelease : QScroller::InputMove,
                                             position, timestamp(event));
        }
        if (event->type() == QEvent::TouchEnd) {
            if (touchReduced && touchScroller) touchScroller->stop();
            touchTarget.clear();
            touchDevice.clear();
            touchScroller.clear();
            touchId = -1;
        }
        event->accept();
        return true;
    }

    qint64 timestamp(const QInputEvent *event) const {
        return event->timestamp() ? qint64(event->timestamp()) : clock.elapsed();
    }
};

Controller *Controller::install(QWidget *root) {
    if (!root) return nullptr;
    const auto controllers = root->findChildren<Controller *>(QString(), Qt::FindDirectChildrenOnly);
    if (!controllers.isEmpty()) return controllers.first();
    return new Controller(root);
}

Controller::Controller(QWidget *root) : QObject(root), d(std::make_unique<Private>(this, root)) {
    setObjectName(QStringLiteral("finderGestures"));
    qApp->installEventFilter(this);
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive) cancel();
    });
}

Controller::~Controller() {
    if (qApp) qApp->removeEventFilter(this);
    for (auto &pan : d->pans) d->release(pan);
}

void Controller::setHistoryEnabled(bool enabled) {
    if (d->history == enabled) return;
    cancel();
    d->history = enabled;
}

bool Controller::historyEnabled() const { return d->history; }

void Controller::setZoomTarget(QWidget *target, bool enabled) {
    if (!target) return;
    std::erase_if(d->zoomTargets, [](const auto &item) { return !item; });
    const bool registered = std::any_of(d->zoomTargets.begin(), d->zoomTargets.end(), [target](const auto &item) {
        return item == target;
    });
    if (enabled && d->contains(target) && !registered) d->zoomTargets.push_back(target);
    if (!enabled) {
        if (target == d->nativeTarget) cancel();
        std::erase_if(d->zoomTargets, [target](const auto &item) { return !item || item == target; });
    }
}

bool Controller::setTouchPanning(QAbstractScrollArea *area, bool enabled) {
    if (!area) return false;
    std::erase_if(d->pans, [this](auto &pan) {
        if (pan.area && pan.viewport && pan.scroller) return false;
        d->release(pan);
        return true;
    });
    auto it = std::find_if(d->pans.begin(), d->pans.end(), [area](const auto &pan) { return pan.area == area; });
    if (it != d->pans.end()) {
        if (enabled && d->contains(area) && it->viewport == area->viewport()) return true;
        d->release(*it);
        d->pans.erase(it);
    }
    if (!enabled) return true;
    auto *viewport = area->viewport();
    if (!d->contains(area) || !viewport || qobject_cast<QAbstractItemView *>(area) ||
        QScroller::hasScroller(viewport)) return false;
    Private::Pan pan;
    pan.area = area;
    pan.viewport = viewport;
    pan.acceptedTouch = viewport->testAttribute(Qt::WA_AcceptTouchEvents);
    // Qt's generic Pan recognizer runs before QObject filters and can consume
    // multi-touch updates first. Suspend the scroll area's built-in Pan on
    // this explicitly opted-in viewport to avoid a second scroll path when
    // an extra finger cancels our one-finger touchscreen sequence.
    viewport->ungrabGesture(Qt::PanGesture);
    viewport->setAttribute(Qt::WA_AcceptTouchEvents);
    pan.scroller = QScroller::scroller(viewport);
    auto properties = pan.scroller->scrollerProperties();
    properties.setScrollMetric(QScrollerProperties::HorizontalOvershootPolicy, QScrollerProperties::OvershootAlwaysOff);
    properties.setScrollMetric(QScrollerProperties::VerticalOvershootPolicy, QScrollerProperties::OvershootAlwaysOff);
    pan.scroller->setScrollerProperties(properties);
    d->pans.push_back(pan);
    return true;
}

void Controller::setReducedMotion(bool reduced) {
    d->reducedMotion = reduced;
    if (reduced) cancel();
}

void Controller::cancel() { d->cancel(); }

bool Controller::eventFilter(QObject *watched, QEvent *event) {
    auto *widget = qobject_cast<QWidget *>(watched);
    if (!widget) return false;
    // ParentChange has already changed the tree. Stop a registered surface
    // even if it has just left root; ancestors can hide an active viewport too.
    if (event->type() == QEvent::Hide || event->type() == QEvent::ParentChange ||
        event->type() == QEvent::EnabledChange || event->type() == QEvent::Resize) {
        const auto affects = [widget](const QPointer<QWidget> &target) {
            return target && (target == widget || widget->isAncestorOf(target));
        };
        if (affects(d->nativeTarget) || affects(d->touchTarget)) cancel();
        for (auto &pan : d->pans) {
            if (pan.scroller && affects(pan.viewport)) pan.scroller->stop();
        }
    }
    if (!d->contains(widget)) return false;
    switch (event->type()) {
    case QEvent::NativeGesture:
        return d->native(widget, static_cast<QNativeGestureEvent *>(event));
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TouchCancel:
        return d->touch(widget, static_cast<QTouchEvent *>(event));
    case QEvent::Wheel:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::KeyPress:
    case QEvent::DragEnter:
    case QEvent::Drop:
    case QEvent::ContextMenu:
    case QEvent::WindowDeactivate:
    case QEvent::Close:
        cancel();
        break;
    case QEvent::Hide:
    case QEvent::ParentChange:
    case QEvent::EnabledChange:
        if (widget == d->root || widget == d->root->window() ||
            widget == d->nativeTarget || widget == d->touchTarget) cancel();
        break;
    default: break;
    }
    return false;
}

} // namespace FinderGestures
