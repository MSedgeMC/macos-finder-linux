// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#pragma once

#include <QColor>
#include <QObject>
#include <QPalette>
#include <QPointer>

class QApplication;

// GUI-thread appearance policy, independent of native actions and rendering.
// Call apply() after setting the application's organization/name, and after
// writing QSettings. The application owns instance(); receivers should use a
// QObject context when connecting changed(). No timers or worker callbacks.
class FinderAppearance final : public QObject {
    Q_OBJECT
public:
    enum Role {
        Header, HeaderText, SecondaryText, Border, Focus, Accent,
        Selection, SelectedText, IconSelection, IconLabelSelection,
        SidebarSelection, SidebarHover, ScrollHandle, Error,
        MaterialEdge, MaterialShadow, ToolbarSurface, ToolbarActive, ToolbarEdge
    };
    Q_ENUM(Role)

    // QSettings: Appearance/mode = dark (default), light, or system;
    // Appearance/increaseContrast and Appearance/reduceTransparency = false.
    // System follows Qt's color-scheme signal, using the native palette when
    // the platform reports Unknown. Invalid preferences fall back to dark.
    static void apply(QApplication *application);
    static FinderAppearance *instance();
    static QPalette palette();
    static bool dark();
    static bool highContrast();
    static bool reduceTransparency();
    static QColor materialTint();
    static QColor color(Role role);

signals:
    // Emitted once after an effective policy/palette change, including a
    // transparency-only change. Root updates its stylesheet/material here.
    void changed();

protected:
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    explicit FinderAppearance(QApplication *application);
    void refresh();
    QPalette m_systemPalette;
    QPalette m_palette;
    bool m_dark = true;
    bool m_highContrast = false;
    bool m_reduceTransparency = false;
    bool m_initialized = false;
    bool m_applying = false;
    QPointer<QApplication> m_application;
};
