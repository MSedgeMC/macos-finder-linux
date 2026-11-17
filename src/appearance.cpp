// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#include "appearance.h"

#include <QApplication>
#include <QEvent>
#include <QSettings>
#include <QStyleHints>
#include <QThread>

namespace {
QPointer<FinderAppearance> appearance;

QPalette makePalette(bool dark, bool contrast)
{
    QPalette result;
    const QColor window(contrast ? (dark ? "#18181a" : "#ffffff") : (dark ? "#26262a" : "#f2f2f5"));
    const QColor base(contrast ? (dark ? "#101012" : "#ffffff") : (dark ? "#1e1e1e" : "#ffffff"));
    const QColor text(contrast ? (dark ? "#ffffff" : "#000000") : (dark ? "#ededf2" : "#202024"));
    const QColor secondary(contrast ? (dark ? "#dedee3" : "#303036") : (dark ? "#aaaab1" : "#5a5a63"));
    const QColor button(contrast ? (dark ? "#303034" : "#e7e7ec") : (dark ? "#343438" : "#e5e5ea"));
    const QColor border(contrast ? (dark ? "#b4b4bd" : "#505059") : (dark ? "#55555b" : "#aaaab2"));
    // A slightly deeper default blue keeps white selection/menu text above
    // 4.5:1. Icon-label and neutral row selection retain their original fills.
    const QColor accent(contrast ? (dark ? "#8abaff" : "#004da8") : (dark ? "#2d70e8" : "#0866d9"));
    for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        const QColor foreground = group == QPalette::Disabled ? secondary : text;
        result.setColor(group, QPalette::Window, window);
        result.setColor(group, QPalette::Base, base);
        result.setColor(group, QPalette::AlternateBase, dark ? QColor("#252525") : QColor("#f4f4f7"));
        result.setColor(group, QPalette::WindowText, foreground);
        result.setColor(group, QPalette::Text, foreground);
        result.setColor(group, QPalette::Button, button);
        result.setColor(group, QPalette::ButtonText, foreground);
        result.setColor(group, QPalette::PlaceholderText, secondary);
        result.setColor(group, QPalette::Highlight, accent);
        result.setColor(group, QPalette::HighlightedText, contrast && dark ? QColor("#101012") : QColor(Qt::white));
        result.setColor(group, QPalette::Accent, accent);
        result.setColor(group, QPalette::Link, contrast ? accent : (dark ? QColor("#6ca4f8") : accent));
        result.setColor(group, QPalette::LinkVisited, dark ? QColor("#c9a4ff") : QColor("#6840a8"));
        result.setColor(group, QPalette::Mid, border);
        result.setColor(group, QPalette::Midlight, dark ? QColor("#676770") : QColor("#d8d8df"));
        result.setColor(group, QPalette::Light, dark ? QColor("#85858e") : QColor(Qt::white));
        result.setColor(group, QPalette::Dark, dark ? QColor("#151517") : QColor("#74747c"));
        result.setColor(group, QPalette::Shadow, dark ? QColor(Qt::black) : QColor("#606069"));
        result.setColor(group, QPalette::ToolTipBase, window);
        result.setColor(group, QPalette::ToolTipText, text);
        result.setColor(group, QPalette::BrightText, dark ? QColor("#ffb4b4") : QColor("#a01824"));
    }
    return result;
}
}

FinderAppearance::FinderAppearance(QApplication *application)
    : QObject(application), m_systemPalette(application->palette()),
      m_palette(makePalette(true, false)), m_application(application)
{
    application->installEventFilter(this);
    connect(application->styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        // Qt may update its native palette after delivering the scheme signal.
        QMetaObject::invokeMethod(this, [this] { refresh(); }, Qt::QueuedConnection);
    });
}

FinderAppearance *FinderAppearance::instance()
{
    auto *application = qobject_cast<QApplication *>(QCoreApplication::instance());
    if (!application || QThread::currentThread() != application->thread()) return nullptr;
    if (!appearance) appearance = new FinderAppearance(application);
    return appearance;
}

void FinderAppearance::apply(QApplication *application)
{
    if (!application || application != QCoreApplication::instance()
        || QThread::currentThread() != application->thread()) return;
    if (auto *policy = instance()) policy->refresh();
}

void FinderAppearance::refresh()
{
    if (!m_application || m_applying) return;
    QSettings settings;
    settings.sync();
    const QString mode = settings.value("Appearance/mode", "dark").toString();
    bool isDark = mode != "light";
    if (mode == "system") {
        const auto scheme = m_application->styleHints()->colorScheme();
        if (scheme == Qt::ColorScheme::Dark) isDark = true;
        else if (scheme == Qt::ColorScheme::Light) isDark = false;
        else {
            const QColor background = m_systemPalette.color(QPalette::Window);
            isDark = .2126 * background.redF() + .7152 * background.greenF() + .0722 * background.blueF() < .5;
        }
    }
    const bool contrast = settings.value("Appearance/increaseContrast", false).toBool();
    const bool reduced = settings.value("Appearance/reduceTransparency", false).toBool();
    const QPalette next = makePalette(isDark, contrast);
    const bool changedPolicy = !m_initialized || isDark != m_dark || contrast != m_highContrast
        || reduced != m_reduceTransparency || next != m_palette;
    m_dark = isDark;
    m_highContrast = contrast;
    m_reduceTransparency = reduced;
    m_palette = next;
    m_initialized = true;
    m_applying = true;
    if (m_application->palette() != next) m_application->setPalette(next);
    m_applying = false;
    if (changedPolicy) emit changed();
}

bool FinderAppearance::eventFilter(QObject *object, QEvent *event)
{
    if (object == m_application && event->type() == QEvent::ApplicationPaletteChange
        && !m_applying && m_application->palette() != m_palette) {
        // Keep a native fallback separate: our dark palette must never become
        // the evidence for system-dark when Qt reports an unknown scheme.
        m_systemPalette = m_application->palette();
        refresh();
    }
    return QObject::eventFilter(object, event);
}

QPalette FinderAppearance::palette() { return appearance ? appearance->m_palette : makePalette(true, false); }
bool FinderAppearance::dark() { return !appearance || appearance->m_dark; }
bool FinderAppearance::highContrast() { return appearance && appearance->m_highContrast; }
bool FinderAppearance::reduceTransparency() { return appearance && appearance->m_reduceTransparency; }

QColor FinderAppearance::materialTint()
{
    QColor tint(highContrast() ? palette().color(QPalette::Window) : QColor(dark() ? "#151412" : "#f2f2f5"));
    // Finder's sidebar is a dark navigation surface with a restrained live
    // backdrop contribution. The toolbar has its own content-backed coating.
    tint.setAlpha(reduceTransparency() || highContrast() ? 255 : (dark() ? 232 : 240));
    return tint;
}

QColor FinderAppearance::color(Role role)
{
    const QPalette p = palette();
    switch (role) {
    case Header: return highContrast() ? p.color(QPalette::Window) : QColor(dark() ? "#29292b" : "#eaeaf0");
    case HeaderText: return highContrast() ? p.color(QPalette::Text) : QColor(dark() ? "#d7d7da" : "#3e3e45");
    case SecondaryText: return p.color(QPalette::PlaceholderText);
    case Border: return p.color(QPalette::Mid);
    case Focus: return highContrast() ? p.color(QPalette::Highlight) : QColor(dark() ? "#6ca4f8" : "#0866d9");
    case Accent: return p.color(QPalette::Link);
    case Selection: return highContrast() ? p.color(QPalette::Highlight) : QColor(dark() ? "#3b3b40" : "#d5e4fa");
    case SelectedText: return highContrast() ? p.color(QPalette::HighlightedText) : QColor(dark() ? "#ffffff" : "#202024");
    case IconSelection: return highContrast() ? p.color(QPalette::Highlight) : QColor(dark() ? "#363638" : "#d5e4fa");
    case IconLabelSelection: return highContrast() ? p.color(QPalette::Highlight) : QColor(dark() ? "#296ec6" : "#0866d9");
    case SidebarSelection: {
        if (highContrast()) return p.color(QPalette::Highlight);
        QColor fill = p.color(QPalette::WindowText); fill.setAlphaF(.11); return fill;
    }
    case SidebarHover: {
        if (highContrast()) return p.color(QPalette::Button);
        QColor fill = p.color(QPalette::WindowText); fill.setAlphaF(.045); return fill;
    }
    case ScrollHandle: return highContrast() ? p.color(QPalette::WindowText) : QColor(dark() ? "#77777f" : "#8b8b95");
    case Error: return p.color(QPalette::BrightText);
    case MaterialEdge: return highContrast() ? p.color(QPalette::Mid) : QColor(dark() ? 255 : 0, dark() ? 255 : 0, dark() ? 255 : 0, 26);
    case MaterialShadow: return highContrast() ? p.color(QPalette::Mid) : QColor(0, 0, 0, dark() ? 60 : 25);
    case ToolbarSurface: return highContrast() ? p.color(QPalette::Button) : QColor(dark() ? "#191919" : "#eeeeef");
    case ToolbarActive: return highContrast() ? p.color(QPalette::Highlight) : QColor(dark() ? "#292929" : "#dcdcdf");
    case ToolbarEdge: return highContrast() ? p.color(QPalette::Mid) : QColor(dark() ? "#393939" : "#c5c5c8");
    }
    return p.color(QPalette::Text);
}
