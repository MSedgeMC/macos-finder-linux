// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#pragma once

class QAbstractScrollArea;

// Native Qt Widgets scrolling for a Linux approximation inspired by Finder.
// Changes scrollbar values, never painting or item/pointer hit geometry.
namespace FinderScroll {

struct Options {
    // Finite settling time after the last unphased wheel event. Wheel distance
    // comes from Qt's system wheelScrollLines and a bounded logical-pixel step.
    int durationMs = 180;
    double wheelSpeed = 0.5;
    // Cap the view's wheel step so a large tile cannot make one notch jump
    // several tiles. Keyboard and scrollbar single/page steps stay native.
    int maximumWheelStep = 24;
    // Map a purely vertical wheel to the horizontal bar (Gallery/Columns).
    // Automatic routing also does this if only the horizontal bar has a range.
    // Diagonal pixel input keeps its two axes; native modifiers remain native.
    bool horizontalWheel = false;
    bool respectReducedMotion = true;
};

// GUI-thread only; idempotent, owned by area, and requires no subclass or moc.
// Item views are switched to ScrollPerPixel on both axes; their selection,
// keyboard, editing, drag/drop and scrollbar action handlers remain native.
// Install on each actual view, including dynamically created child columns
// and the sidebar's presentation tree, rather than its hidden backing view.
// Pixel deltas are applied immediately, 1:1 in logical pixels. Angle-only
// phased input uses the same bounded wheel distance without an extra curve.
// Qt has already adjusted deltas for natural scrolling: inverted() is not
// applied again, matching QScrollBar. invertedControls() remains respected.
// The KDE animation factor and global Qt style hint are read on wheel input;
// zero disables animation. There is no settings poll or idle animation timer.
void install(QAbstractScrollArea *area, const Options &options = {});

// App-local Input/wheelSpeed is a percentage (10..200, default 50).
// Read when views are installed/refreshed, never through an idle poll.
Options optionsFromPreferences(bool horizontalWheel = false);

// Optional cancellation before navigation; keeps the current native position.
void stop(QAbstractScrollArea *area);
bool isAnimating(const QAbstractScrollArea *area);

} // namespace FinderScroll
