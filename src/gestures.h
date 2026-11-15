// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#pragma once

#include <QObject>
#include <QPointF>

#include <memory>

class QAbstractScrollArea;
class QWidget;

namespace FinderGestures {

// GUI-thread controller, owned by root. Only events inside root's widget tree
// and window are considered; newly created children need no filter installation.
// Uses public Qt 6.8+ APIs. It adds no global desktop gesture or wheel handler.
class Controller final : public QObject {
    Q_OBJECT
public:
    enum class HistoryDirection { Back, Forward };
    Q_ENUM(HistoryDirection)

    // Idempotent. Add gestures.cpp/.h to the host's AUTOMOC-enabled target.
    static Controller *install(QWidget *root);
    ~Controller() override;

    // Enable only while the host has usable history actions (default: false).
    void setHistoryEnabled(bool enabled);
    bool historyEnabled() const;

    // Explicit zoom surfaces, e.g. an active view or raster/PDF preview.
    // Nearest registered ancestor wins. The target is locked at native Begin;
    // no signal is emitted outside it or after it is disabled/destroyed.
    void setZoomTarget(QWidget *target, bool enabled = true);

    // Opt-in, one-finger TouchScreen panning with QScroller::handleInput.
    // Returns false for item views (selection/file dragging), an existing
    // scroller, or a surface outside root. Focusable/interactive descendants
    // retain their own input. Suspends the scroll area's built-in generic Pan
    // subscription until unregister; hosts with custom Pan handling should
    // not opt in. Does not grab mouse or touchpad gestures, change scroll modes,
    // or synthesize clicks. Call again after viewport replacement.
    bool setTouchPanning(QAbstractScrollArea *area, bool enabled = true);

    // Suppresses touch inertia, not direct finger tracking. Also checks the
    // host's Input/reduceMotion setting, Qt's global animation style hint and
    // KDE's AnimationDurationFactor at touch begin, without an idle poll.
    // Native zoom/history are immediate requests with no added animation.
    void setReducedMotion(bool reduced);

public Q_SLOTS:
    // Before navigation/view replacement, or when the host cancels input.
    // Stops panning and ignores the remainder of the current native/touch
    // stream until End/TouchEnd or a new Begin. Applied zoom stays applied.
    void cancel();

Q_SIGNALS:
    // Only an explicit TouchPad SwipeNativeGesture with fingerCount()==2.
    // Qt angle 180 (rightward) requests Back; 0 (leftward) requests Forward.
    // Angles more than 30 degrees from horizontal are ignored. A swipe is an
    // OS-recognized discrete event: Qt exposes no distance/velocity threshold.
    // The host should invoke its existing enabled history action.
    void historyRequested(FinderGestures::Controller::HistoryDirection direction);

    // Native Zoom delta, during Begin..End, on TouchPad/TouchScreen with two
    // fingers or an unknown count (Qt Wayland zoom often reports zero).
    // Apply currentZoom * factor through the host's bounded native zoom API.
    // factor = 1 + event.value(); position is logical pixels in target.
    void zoomRequested(QWidget *target, qreal factor, const QPointF &position);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    explicit Controller(QWidget *root);
    class Private;
    std::unique_ptr<Private> d;
};

// Portable limits: Wayland two-finger scrolling arrives as QWheelEvent, and
// native Pan normally means three or more fingers. Neither becomes history.
// Qt 6.8-6.10's Wayland backend exposes pinch, not a two-finger native Swipe;
// this API therefore cannot promise two-finger history on that backend. X11
// may expose no native gestures. QNativeGestureEvent also has no cancellation
// flag: OS cancellation reported as End cannot be distinguished from success.
// No distance gesture is inferred from native Pan, raw touchpad or wheels.

} // namespace FinderGestures
