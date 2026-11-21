// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#include "appearance.h"

#include <QApplication>
#include <QEvent>
#include <QSettings>
#include <QStyleHints>
#include <QThread>
#include <QAbstractButton>
#include <QFocusEvent>
#include <QStyle>
#include <QFileSystemWatcher>
#include <QFileInfo>
#include <QDir>
#include <QProxyStyle>
#include <QStyleOption>

namespace {
QPointer<FinderAppearance> appearance;

// Native button feedback stays intact; only a mouse-origin focus cue is
// suppressed. Text editing and keyboard traversal keep their native state.
class ButtonFocusStyle final : public QProxyStyle {
public:
    explicit ButtonFocusStyle(QStyle *base) : QProxyStyle(base) {}
    void drawControl(ControlElement element, const QStyleOption *option, QPainter *painter,
                     const QWidget *widget = nullptr) const override {
        if (const auto *button = qobject_cast<const QAbstractButton *>(widget);
            button && !button->property("finderKeyboardFocus").toBool()) {
            if (const auto *native = qstyleoption_cast<const QStyleOptionButton *>(option)) {
                auto copy = *native; copy.state &= ~State_HasFocus;
                copy.features &= ~(QStyleOptionButton::DefaultButton | QStyleOptionButton::AutoDefaultButton);
                QProxyStyle::drawControl(element, &copy, painter, widget); return;
            }
            if (const auto *native = qstyleoption_cast<const QStyleOptionToolButton *>(option)) {
                auto copy = *native; copy.state &= ~State_HasFocus;
                QProxyStyle::drawControl(element, &copy, painter, widget); return;
            }
        }
        QProxyStyle::drawControl(element, option, painter, widget);
    }
    void drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter,
                       const QWidget *widget = nullptr) const override {
        if (const auto *button = qobject_cast<const QAbstractButton *>(widget);
            button && !button->property("finderKeyboardFocus").toBool()) {
            if (element == PE_FrameFocusRect) return;
            if (const auto *native = qstyleoption_cast<const QStyleOptionButton *>(option)) {
                auto copy = *native; copy.state &= ~State_HasFocus;
                copy.features &= ~(QStyleOptionButton::DefaultButton | QStyleOptionButton::AutoDefaultButton);
                QProxyStyle::drawPrimitive(element, &copy, painter, widget); return;
            }
        }
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }
};

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
    application->setStyle(new ButtonFocusStyle(application->style()));
    m_preferencesWatcher = new QFileSystemWatcher(this);
    connect(m_preferencesWatcher, &QFileSystemWatcher::fileChanged, this, [this] { queuePreferencesRefresh(); });
    connect(m_preferencesWatcher, &QFileSystemWatcher::directoryChanged, this, [this] { queuePreferencesRefresh(); });
    watchPreferences();
    connect(application->styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        // Qt may update its native palette after delivering the scheme signal.
        QMetaObject::invokeMethod(this, [this] { refresh(); }, Qt::QueuedConnection);
    });
}

void FinderAppearance::watchPreferences()
{
    // QSettings publishes by atomic replacement. Watch its directory too,
    // then re-arm the file after replacement; no idle polling is required.
    const QFileInfo file(QSettings().fileName());
    const QString directory = file.absolutePath();
    if (QDir().mkpath(directory) && !m_preferencesWatcher->directories().contains(directory))
        m_preferencesWatcher->addPath(directory);
    if (file.isFile() && !m_preferencesWatcher->files().contains(file.absoluteFilePath()))
        m_preferencesWatcher->addPath(file.absoluteFilePath());
}

void FinderAppearance::queuePreferencesRefresh()
{
    if (m_refreshQueued) return;
    m_refreshQueued = true;
    QMetaObject::invokeMethod(this, [this] {
        m_refreshQueued = false; watchPreferences(); refresh();
    }, Qt::QueuedConnection);
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
    const bool glossy = settings.value("Appearance/windowControlsStyle", "glossy").toString() != "flat";
    const bool left = settings.value("Appearance/windowControlsSide", "right").toString() == "left";
    const bool hover = settings.value("Appearance/windowControlsSymbols", "always").toString() == "hover";
    bool validTranslucency = false;
    const int storedTranslucency = settings.value("Appearance/sidebarTranslucency", isDark ? 17 : 9).toInt(&validTranslucency);
    const int translucency = validTranslucency ? qBound(0, storedTranslucency, 40) : (isDark ? 17 : 9);
    const bool alternatingRows = settings.value("View/alternatingRowColors", true).toBool();
    const QString sidebarSize = settings.value("Sidebar/itemSize", "small").toString();
    const int sidebarHeight = sidebarSize == "large" ? 40 : sidebarSize == "medium" ? 32 : 24;
    const QPalette next = makePalette(isDark, contrast);
    const bool changedPolicy = !m_initialized || isDark != m_dark || contrast != m_highContrast
        || reduced != m_reduceTransparency || next != m_palette
        || glossy != m_glossyControls || left != m_leftControls
        || hover != m_hoverSymbols || translucency != m_sidebarTranslucency
        || alternatingRows != m_alternatingRows || sidebarHeight != m_sidebarRowHeight;
    m_dark = isDark;
    m_highContrast = contrast;
    m_reduceTransparency = reduced;
    m_glossyControls = glossy;
    m_leftControls = left;
    m_hoverSymbols = hover;
    m_sidebarTranslucency = translucency;
    m_alternatingRows = alternatingRows;
    m_sidebarRowHeight = sidebarHeight;
    m_palette = next;
    m_initialized = true;
    m_applying = true;
    if (m_application->palette() != next) m_application->setPalette(next);
    m_applying = false;
    if (changedPolicy) emit changed();
}

bool FinderAppearance::eventFilter(QObject *object, QEvent *event)
{
    if (auto *button = qobject_cast<QAbstractButton *>(object)) {
        bool change = false, keyboard = false;
        if (event->type() == QEvent::FocusIn) {
            const auto reason = static_cast<QFocusEvent *>(event)->reason();
            keyboard = reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason
                || reason == Qt::ShortcutFocusReason;
            change = true;
        } else if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::TouchBegin
                   || event->type() == QEvent::FocusOut) change = true;
        if (change && button->property("finderKeyboardFocus").toBool() != keyboard) {
            button->setProperty("finderKeyboardFocus", keyboard);
            button->style()->unpolish(button); button->style()->polish(button); button->update();
        }
    }
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
bool FinderAppearance::glossyWindowControls() { return !appearance || appearance->m_glossyControls; }
bool FinderAppearance::leftWindowControls() { return appearance && appearance->m_leftControls; }
bool FinderAppearance::hoverWindowSymbols() { return appearance && appearance->m_hoverSymbols; }
int FinderAppearance::sidebarTranslucency() { return appearance ? appearance->m_sidebarTranslucency : 17; }
bool FinderAppearance::alternatingRowColors() { return !appearance || appearance->m_alternatingRows; }
int FinderAppearance::sidebarRowHeight() { return appearance ? appearance->m_sidebarRowHeight : 24; }

QColor FinderAppearance::materialTint()
{
    QColor tint(highContrast() ? palette().color(QPalette::Window) : QColor(dark() ? "#252528" : "#f2f2f5"));
    if (dark() && !highContrast() && sidebarTranslucency() > 25) {
        // At the most transmitting presets, slightly deepen the coating to
        // keep white navigation text readable over a bright live backdrop.
        const int depth = qRound(16 * (sidebarTranslucency() - 25) / 15.0);
        tint.setRgb(37 - depth, 37 - depth, 40 - depth);
    }
    // A modestly transmitting frost lets native blur reveal broad backdrop
    // colors while retaining the reference's dark neutral navigation tint.
    // The toolbar has its own content-backed coating.
    tint.setAlpha(reduceTransparency() || highContrast() ? 255 : qRound(255 * (100 - sidebarTranslucency()) / 100.0));
    return tint;
}

qreal FinderAppearance::materialBackdropSaturation()
{
    return reduceTransparency() || highContrast() ? 1.0 : (dark() ? .35 : .7);
}

QColor FinderAppearance::color(Role role)
{
    const QPalette p = palette();
    switch (role) {
    case Header: return highContrast() ? p.color(QPalette::Window) : QColor(dark() ? "#29292b" : "#eaeaf0");
    case HeaderText: return highContrast() ? p.color(QPalette::Text) : QColor(dark() ? "#e6e6e8" : "#3e3e45");
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
    case ToolbarActive: return highContrast() ? p.color(QPalette::Highlight) : QColor(dark() ? "#2d2d2d" : "#dcdcdf");
    case ToolbarEdge: return highContrast() ? p.color(QPalette::Mid) : QColor(dark() ? "#393939" : "#c5c5c8");
    case SidebarGlyph: return highContrast() ? p.color(QPalette::Link) : QColor(dark() ? (sidebarTranslucency()>25 ? "#c3e2ff" : sidebarTranslucency()>17 ? "#78b9ff" : "#209cff") : (sidebarTranslucency()>9 ? "#00418b" : "#0866d9"));
    case SidebarText: return highContrast() ? p.color(QPalette::Text) : QColor(dark() ? (sidebarTranslucency()>25 ? "#ffffff" : "#ededf2") : "#202024");
    case SidebarSecondaryText:
        if(highContrast()) return p.color(QPalette::PlaceholderText);
        if(dark()) return QColor(sidebarTranslucency()>25 ? "#ffffff" : sidebarTranslucency()>17 ? "#dedee3" : "#c8c8ce");
        return QColor(sidebarTranslucency()>9 ? "#202024" : "#5a5a63");
    }
    return p.color(QPalette::Text);
}
