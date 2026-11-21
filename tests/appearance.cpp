// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
// Run through tests/run-headless.py only. Target sources: appearance, browser,
// places, preview, settings, tags, preferences, scroll (AUTOMOC); the existing
// Qt Widgets/Pdf/Multimedia and browser/places/settings KF6 dependencies.
#include "appearance.h"
#include "windowcontrols.h"
#include "glasscontrols.h"
#include <QSlider>
#include <QFocusEvent>
#include <QSaveFile>
#include <QPushButton>
#include <QStyleOptionButton>
#include <QScrollArea>
#include <QScrollBar>
#include "browser.h"
#include "places.h"
#include "preview.h"
#include "settings.h"

#include <KFilePlacesModel>
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QSettings>
#include <QStandardPaths>
#include <QStandardItemModel>
#include <QStyleHints>
#include <QStyleOptionViewItem>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QTreeView>
#include <QListView>
#include <QTabWidget>
#include <QTabBar>
#include <cmath>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>

namespace {
QStringList warnings;
std::mutex messageMutex;
void messages(QtMsgType type, const QMessageLogContext &, const QString &message)
{
    const std::lock_guard guard(messageMutex);
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) warnings.append(message);
    if (type == QtFatalMsg) std::abort();
}
void check(bool value, const char *message)
{
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
bool waitFor(const std::function<bool()> &condition, int timeout = 10000)
{
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < timeout) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return condition();
}
double luminance(const QColor &color)
{
    const auto linear = [](double value) { return value <= .04045 ? value / 12.92 : std::pow((value + .055) / 1.055, 2.4); };
    return .2126 * linear(color.redF()) + .7152 * linear(color.greenF()) + .0722 * linear(color.blueF());
}
double contrast(const QColor &a, const QColor &b)
{
    return (std::max(luminance(a), luminance(b)) + .05) / (std::min(luminance(a), luminance(b)) + .05);
}
void checkContrast(bool increased)
{
    const QPalette p = FinderAppearance::palette();
    const double primary = increased ? 7 : 4.5;
    for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        check(contrast(p.color(group, QPalette::Text), p.color(group, QPalette::Base)) >= primary, "content text contrast");
        check(contrast(p.color(group, QPalette::WindowText), p.color(group, QPalette::Window)) >= primary, "window text contrast");
        check(contrast(p.color(group, QPalette::ButtonText), p.color(group, QPalette::Button)) >= (group == QPalette::Disabled ? 4.5 : primary), "button text contrast");
        check(contrast(p.color(group, QPalette::PlaceholderText), p.color(group, QPalette::Window)) >= 4.5, "secondary text contrast");
        check(contrast(p.color(group, QPalette::ToolTipText), p.color(group, QPalette::ToolTipBase)) >= primary, "tooltip text contrast");
        check(contrast(p.color(group, QPalette::HighlightedText), p.color(group, QPalette::Highlight)) >= primary, "native selection and menu text contrast");
    }
    check(contrast(FinderAppearance::color(FinderAppearance::SelectedText), FinderAppearance::color(FinderAppearance::Selection)) >= primary, "row selection contrast");
    check(contrast(p.color(QPalette::HighlightedText), FinderAppearance::color(FinderAppearance::IconLabelSelection)) >= primary, "icon selection text contrast");
    check(contrast(FinderAppearance::color(FinderAppearance::Focus), p.color(QPalette::Base)) >= 3, "visible keyboard focus");
    check(contrast(FinderAppearance::color(FinderAppearance::Accent), p.color(QPalette::Window)) >= 4.5, "link contrast");
}
void apply(const QString &mode, bool increased = false, bool reduced = false)
{
    QSettings settings;
    settings.setValue("Appearance/mode", mode);
    settings.setValue("Appearance/increaseContrast", increased);
    settings.setValue("Appearance/reduceTransparency", reduced);
    FinderAppearance::apply(qApp);
    QApplication::processEvents();
}
void render(QWidget &widget)
{
    widget.ensurePolished();
    QImage raster(widget.size() * widget.devicePixelRatioF(), QImage::Format_ARGB32_Premultiplied);
    raster.setDevicePixelRatio(widget.devicePixelRatioF());
    raster.fill(Qt::transparent);
    widget.render(&raster);
    check(!raster.isNull(), "real widget rendering");
}
}

int main(int argc, char **argv)
{
    check(qgetenv("QT_QPA_PLATFORM") == "offscreen" && qEnvironmentVariableIsSet("XDG_CONFIG_HOME")
        && qEnvironmentVariableIsSet("DBUS_SESSION_BUS_ADDRESS") && !qEnvironmentVariableIsSet("DISPLAY")
        && !qEnvironmentVariableIsSet("WAYLAND_DISPLAY"), "private headless runner required");
    QApplication app(argc, argv);
    app.setOrganizationName("FinderAppearancePrivateTest");
    app.setApplicationName("Appearance");
    app.setStyle("Fusion");
    const QFont originalFont = app.font();
    QSettings().clear();
    FinderAppearance::apply(&app);
    auto *policy = FinderAppearance::instance();
    check(policy && policy->parent() == &app && policy->thread() == app.thread(), "application-owned main-thread lifecycle");
    check(FinderAppearance::dark() && !FinderAppearance::highContrast() && !FinderAppearance::reduceTransparency(), "default dark policy");
    check(app.font() == originalFont, "appearance preserves font");
    check(app.palette().color(QPalette::Base) == QColor("#1e1e1e")
        && FinderAppearance::color(FinderAppearance::IconSelection) == QColor("#363638")
        && FinderAppearance::color(FinderAppearance::IconLabelSelection) == QColor("#296ec6"), "default dark surfaces and selections");
    check(FinderAppearance::materialTint() == QColor(37, 37, 40, 212), "dark neutral sidebar frost");
    // Any blurred desktop color is bounded by black/white after the native
    // saturation pass. Verify navigation readability at both extremes.
    const QColor frost = FinderAppearance::materialTint();
    for (const QColor backdrop : {QColor(Qt::black), QColor(Qt::white)}) {
        QImage surface(1, 1, QImage::Format_ARGB32_Premultiplied); surface.fill(backdrop);
        QPainter painter(&surface); painter.fillRect(surface.rect(), frost); painter.end();
        check(contrast(app.palette().color(QPalette::Text), surface.pixelColor(0, 0)) >= 4.5, "primary text over sidebar frost");
        check(contrast(FinderAppearance::color(FinderAppearance::SidebarSecondaryText), surface.pixelColor(0, 0)) >= 4.5, "secondary text over sidebar frost");
        check(contrast(FinderAppearance::color(FinderAppearance::SidebarGlyph), surface.pixelColor(0, 0)) >= 3, "sidebar glyph over frost");
    }
    int changes = 0;
    QObject::connect(policy, &FinderAppearance::changed, &app, [&] { ++changes; });
    FinderAppearance::apply(&app);
    check(changes == 0, "idempotent refresh");
    check(FinderAppearance::glossyWindowControls() && !FinderAppearance::leftWindowControls()
        && !FinderAppearance::hoverWindowSymbols() && FinderAppearance::sidebarTranslucency()==17, "control defaults preserve explicit desktop choices");
    check(FinderAppearance::alternatingRowColors() && FinderAppearance::sidebarRowHeight()==24, "compact sidebar and striped list defaults");
    for (const auto &entry : QList<QPair<QString,int>>{{"small",24},{"medium",32},{"large",40},{"invalid",24}}) {
        QSettings().setValue("Sidebar/itemSize",entry.first); FinderAppearance::apply(&app);
        check(FinderAppearance::sidebarRowHeight()==entry.second,"sidebar density uses current size presets");
    }
    QSettings().remove("Sidebar/itemSize"); FinderAppearance::apply(&app);
    QSettings().setValue("Appearance/windowControlsStyle","flat");
    QSettings().setValue("Appearance/windowControlsSide","left");
    QSettings().setValue("Appearance/windowControlsSymbols","hover");
    QSettings().setValue("Appearance/sidebarTranslucency",40);
    FinderAppearance::apply(&app);
    check(!FinderAppearance::glossyWindowControls() && FinderAppearance::leftWindowControls()
        && FinderAppearance::hoverWindowSymbols() && FinderAppearance::materialTint().alpha()==153, "cached control and translucency preferences applied");
    for (const QString modeName : {QString("dark"),QString("light")}) {
        QSettings().setValue("Appearance/mode",modeName); FinderAppearance::apply(&app);
        for(const QColor backdrop:{QColor(Qt::black),QColor(Qt::white)}) {
            QImage surface(1,1,QImage::Format_ARGB32_Premultiplied);surface.fill(backdrop);
            QPainter painter(&surface);painter.fillRect(surface.rect(),FinderAppearance::materialTint());painter.end();
            check(contrast(FinderAppearance::color(FinderAppearance::SidebarText),surface.pixelColor(0,0))>=4.5
                && contrast(FinderAppearance::color(FinderAppearance::SidebarSecondaryText),surface.pixelColor(0,0))>=4.5,"navigation text readable at maximum translucency");
            check(contrast(FinderAppearance::color(FinderAppearance::SidebarGlyph),surface.pixelColor(0,0))>=3,"navigation glyph readable at maximum translucency");
        }
    }
    QSettings().setValue("Appearance/mode","dark");FinderAppearance::apply(&app);
    const int controlChanges=changes;
    for(int i=0;i<10000;++i) { (void)FinderAppearance::materialTint(); (void)FinderAppearance::hoverWindowSymbols(); }
    check(changes==controlChanges,"painting policy reads never reapply settings");
    // Another process uses the same settings file. Replace it atomically as
    // QSettings does; both the first delivery and watcher re-arm must work.
    for (const QString sideName : {QString("right"),QString("left")}) {
        QSettings().sync();
        QFile input(QSettings().fileName());check(input.open(QIODevice::ReadOnly),"external settings read");
        QByteArray data=input.readAll();input.close();
        data.replace("windowControlsSide=left", "windowControlsSide="+sideName.toUtf8());
        data.replace("windowControlsSide=right", "windowControlsSide="+sideName.toUtf8());
        QSaveFile replacement(QSettings().fileName());check(replacement.open(QIODevice::WriteOnly),"external atomic settings update");
        check(replacement.write(data)==data.size() && replacement.commit(),"external settings publication");
        check(waitFor([&] {return FinderAppearance::leftWindowControls()==(sideName=="left");}),"external preferences refresh without polling");
    }
    QSettings().setValue("Appearance/sidebarTranslucency",300); FinderAppearance::apply(&app);
    check(FinderAppearance::sidebarTranslucency()==40 && FinderAppearance::materialTint().alpha()==153,"translucency bounded");
    QSettings().setValue("Appearance/sidebarTranslucency","invalid"); FinderAppearance::apply(&app);
    check(FinderAppearance::sidebarTranslucency()==17,"invalid translucency retains default");
    QSettings().remove("Appearance/sidebarTranslucency");
    QSettings().remove("Appearance/windowControlsStyle"); QSettings().remove("Appearance/windowControlsSide"); QSettings().remove("Appearance/windowControlsSymbols");
    FinderAppearance::apply(&app); changes=0;

    checkContrast(false);
    apply("light");
    check(!FinderAppearance::dark() && changes == 1, "light switch signal");
    checkContrast(false);
    apply("light", true);
    check(FinderAppearance::highContrast() && FinderAppearance::materialTint().alpha() == 255, "increased contrast solid material");
    checkContrast(true);
    apply("dark", true);
    checkContrast(true);
    apply("dark");
    const QPalette beforeReduced = app.palette();
    const int beforeChanges = changes;
    apply("dark", false, true);
    check(FinderAppearance::reduceTransparency() && FinderAppearance::materialTint().alpha() == 255
        && app.palette() == beforeReduced && changes == beforeChanges + 1, "transparency-only change signal and consumption");
    apply("unknown-value");
    check(FinderAppearance::dark(), "invalid mode preserves dark default");

    // Offscreen has no native theme provider. Its Unknown color scheme must
    // follow an external native palette without feeding back our own palette.
    app.styleHints()->unsetColorScheme();
    apply("system");
    if (app.styleHints()->colorScheme() == Qt::ColorScheme::Unknown) {
        QPalette native = app.palette(); native.setColor(QPalette::Window, Qt::white);
        app.setPalette(native); QApplication::processEvents();
        check(!FinderAppearance::dark(), "unknown system scheme follows light native palette");
        FinderAppearance::apply(&app);
        check(!FinderAppearance::dark(), "system mode does not infer from applied palette");
        native.setColor(QPalette::Window, Qt::black);
        app.setPalette(native); QApplication::processEvents();
        check(FinderAppearance::dark(), "native application palette change refreshes system mode");
    }
    bool workerCreatedPolicy = true;
    std::thread worker([&] { workerCreatedPolicy = FinderAppearance::instance() != nullptr; FinderAppearance::apply(&app); });
    worker.join();
    check(!workerCreatedPolicy, "workers cannot create or mutate GUI policy");

    // Queued policy deliveries use receiver lifetime, never an unguarded view.
    int staleDelivery = 0;
    auto *receiver = new QObject;
    QObject::connect(policy, &FinderAppearance::changed, receiver, [&] { ++staleDelivery; }, Qt::QueuedConnection);
    apply("light");
    staleDelivery = 0;
    QSettings().setValue("Appearance/mode", "dark");
    FinderAppearance::apply(&app);
    delete receiver;
    QApplication::processEvents();
    check(staleDelivery == 0, "destroyed receiver drops queued appearance delivery");
    check(policy->findChildren<QTimer *>().isEmpty(), "no appearance polling timers");

    // KService expects an unprefixed applications.menu even in a private XDG
    // environment. Reuse the host's public menu definition in our writable
    // test directory, without connecting to or modifying its desktop session.
    const QString menus = qEnvironmentVariable("XDG_CONFIG_HOME") + QStringLiteral("/menus");
    check(QDir().mkpath(menus), "private menu directory");
    QString sourceMenu;
    for (const auto &candidate : {QStringLiteral("/etc/xdg/menus/applications.menu"),
             QStringLiteral("/etc/xdg/menus/plasma-applications.menu"),
             QStringLiteral("/etc/xdg/menus/kf5-applications.menu")})
        if (QFile::exists(candidate)) { sourceMenu = candidate; break; }
    if (!sourceMenu.isEmpty()) check(QFile::copy(sourceMenu, menus + "/applications.menu"), "private system menu fixture");
    else {
        QFile menu(menus + "/applications.menu");
        check(menu.open(QIODevice::WriteOnly) && menu.write("<!DOCTYPE Menu PUBLIC '-//freedesktop//DTD Menu 1.0//EN' 'http://www.freedesktop.org/standards/menu-spec/1.0/menu.dtd'><Menu><Name>Applications</Name><DefaultAppDirs/><DefaultDirectoryDirs/><Include><All/></Include></Menu>") > 0, "portable private menu fixture");
    }

    const auto previousHandler = qInstallMessageHandler(messages);
    QTemporaryDir files;
    check(files.isValid(), "private fixture directory");
    QFile textFile(files.filePath("readable.txt"));
    check(textFile.open(QIODevice::WriteOnly) && textFile.write("Readable preview\n") > 0, "text fixture");
    textFile.close();
    Browser browser;
    browser.resize(850, 500);
    browser.setIconPreviewsShown(false);
    browser.navigate(QUrl::fromLocalFile(files.path()));
    check(waitFor([&] { return browser.itemCount() == 1; }), "real native browser listing");
    KFilePlacesModel model;
    FinderPlacesView places;
    places.resize(210, 480);
    places.setModel(&model);
    PreviewPane preview;
    preview.resize(750, 480);
    preview.setGallery(true);
    preview.setUrls({QUrl::fromLocalFile(textFile.fileName())});
    auto *text = preview.findChild<QTextEdit *>("previewText");
    check(text && waitFor([&] { return text->toPlainText().contains("Readable preview"); }), "real preview worker result");
    browser.setMode(Browser::Gallery); QApplication::processEvents();
    auto *strip = browser.findChild<QListView *>("finderGalleryStrip");
    const QImage names = strip->grab().toImage(); const int namedHeight = strip->height();
    browser.setGalleryFilenamesShown(false); QApplication::processEvents();
    check(strip->height() < namedHeight && strip->grab().toImage() != names, "Gallery filename option changes real filmstrip geometry and painting");
    const auto galleryState = browser.saveViewState();
    browser.setMode(Browser::Icons); browser.setMode(Browser::Gallery);
    check(!browser.galleryFilenamesShown(), "Gallery filenames stay independent across mode changes");
    browser.setGalleryFilenamesShown(true); browser.restoreViewState(galleryState);
    check(!browser.galleryFilenamesShown(), "Gallery filename visibility persists in folder view state");
    browser.setGalleryFilenamesShown(true);
    FinderSettings settings;
    settings.resize(580, 530);
    auto *settingsTabs = settings.findChild<QTabWidget *>("finderSettingsTabs");
    check(settingsTabs && settingsTabs->count() == 4 && settingsTabs->documentMode(), "four Settings sections use document-style navigation");
    for (int i = 0; i < 4; ++i) check(!settingsTabs->tabIcon(i).isNull(), "Settings sections have native icons");
    auto *mode = settings.findChild<QComboBox *>("finderSettingsAppearanceMode");
    auto *increased = settings.findChild<QCheckBox *>("finderSettingsIncreaseContrast");
    auto *reduced = settings.findChild<QCheckBox *>("finderSettingsReduceTransparency");
    auto *style = settings.findChild<QComboBox *>("finderSettingsWindowControlsStyle");
    auto *side = settings.findChild<QComboBox *>("finderSettingsWindowControlsSide");
    auto *symbols = settings.findChild<QComboBox *>("finderSettingsWindowControlsSymbols");
    auto *translucency = settings.findChild<QSlider *>("finderSettingsSidebarTranslucency");
    auto *stripes = settings.findChild<QCheckBox *>("finderSettingsAlternatingRows");
    auto *sidebarSize = settings.findChild<QComboBox *>("finderSettingsSidebarItemSize");
    check(stripes && sidebarSize,"list stripes and sidebar density settings exist");
    browser.setMode(Browser::List); browser.show(); QApplication::processEvents();
    auto *fileList = qobject_cast<QTreeView *>(browser.activeView());
    check(fileList && fileList->alternatingRowColors(),"native list stripes initially enabled");
    browser.selectUrls({QUrl::fromLocalFile(textFile.fileName())}); QApplication::processEvents();
    const auto selectionBefore = browser.selectedUrls();
    const QImage striped = fileList->viewport()->grab().toImage();
    stripes->setChecked(false); QApplication::processEvents();
    check(!FinderAppearance::alternatingRowColors() && !fileList->alternatingRowColors()
        && browser.selectedUrls()==selectionBefore,"disabling stripes retains native selection");
    const QImage plain = fileList->viewport()->grab().toImage();
    check(striped.copy(0,plain.height()/2,plain.width(),plain.height()/2)!=plain.copy(0,plain.height()/2,plain.width(),plain.height()/2),
        "stripe setting removes decorative bands below loaded files");
    stripes->setChecked(true); QApplication::processEvents();
    check(fileList->alternatingRowColors() && fileList->viewport()->grab().toImage()==striped,"stripe rendering restores without model mutation");
    sidebarSize->setCurrentIndex(sidebarSize->findData("large")); QApplication::processEvents();
    check(FinderAppearance::sidebarRowHeight()==40,"sidebar setting applies immediately");
    sidebarSize->setCurrentIndex(sidebarSize->findData("small")); QApplication::processEvents();
    check(style && side && symbols && translucency,"shared appearance settings exist");
    style->setCurrentIndex(style->findData("flat")); side->setCurrentIndex(side->findData("left")); symbols->setCurrentIndex(symbols->findData("hover"));
    translucency->setValue(35);
    check(!FinderAppearance::glossyWindowControls() && FinderAppearance::leftWindowControls() && FinderAppearance::hoverWindowSymbols()
        && FinderAppearance::sidebarTranslucency()==35,"settings control changes apply immediately");
    apply("dark",true); check(!translucency->isEnabled() && translucency->value()==35 && FinderAppearance::materialTint().alpha()==255,"accessibility preserves stored translucency");
    apply("dark"); check(translucency->isEnabled(),"translucency restored after accessibility override");
    QSettings().setValue("Appearance/windowControlsSide","right"); FinderAppearance::apply(&app);
    check(side->currentData().toString()=="right","other settings views reload without feedback writes");
    check(mode && increased && reduced && !mode->accessibleName().isEmpty(), "accessible appearance settings controls");
    settings.show(); settings.activateWindow(); QApplication::processEvents();
    QPushButton focusButton("Native focus", &settings);focusButton.setGeometry(20,20,120,32);focusButton.show();
    auto *nativeButton = &focusButton;
    auto nativeFocusPaint = [&] {
        QStyleOptionButton option; option.initFrom(nativeButton); option.rect=QRect(0,0,120,32); option.text="Native focus";
        option.state |= QStyle::State_Enabled | QStyle::State_Active | QStyle::State_HasFocus | QStyle::State_KeyboardFocusChange;
        QImage pixels(120,32,QImage::Format_ARGB32_Premultiplied); pixels.fill(app.palette().color(QPalette::Window));
        QPainter painter(&pixels);app.style()->drawControl(QStyle::CE_PushButton,&option,&painter,nativeButton);return pixels;
    };
    QFocusEvent mouseFocus(QEvent::FocusIn, Qt::MouseFocusReason);QApplication::sendEvent(nativeButton,&mouseFocus);
    check(!nativeButton->property("finderKeyboardFocus").toBool(),"native mouse focus policy");
    const auto mousePaint=nativeFocusPaint();
    QFocusEvent tabFocus(QEvent::FocusIn, Qt::TabFocusReason);QApplication::sendEvent(nativeButton,&tabFocus);
    check(nativeButton->property("finderKeyboardFocus").toBool() && nativeFocusPaint()!=mousePaint,
        "native style retains keyboard focus and suppresses mouse focus cue");
    settings.resize(370,520); QApplication::processEvents();
    for (auto *page : settings.findChildren<QScrollArea *>())
        if (page->isVisible()) check(page->horizontalScrollBar()->maximum()==0, "narrow Settings has no horizontal overflow");
    settings.hide(); settings.resize(580,530);
    for (const auto &name : {QStringLiteral("dark"), QStringLiteral("light")}) {
        for (bool high : {false, true}) {
            apply(name, high);
            settings.reloadPreferences();
            check(mode->currentData().toString() == name && increased->isChecked() == high, "settings reload does not overwrite preferences");
            for (auto browserMode : {Browser::Icons, Browser::List, Browser::Columns, Browser::Gallery}) {
                browser.setMode(browserMode);
                auto *view = browser.activeView();
                check(!view->accessibleName().isEmpty(), "native views have accessible names");
                check(view->palette().color(QPalette::Text) == app.palette().color(QPalette::Text), "views consume changed palette");
                const QModelIndex index = view->model()->index(0, 0);
                view->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                render(browser);
            }
            render(places); render(preview); render(settings);
            check(preview.findChild<QLabel *>("previewMetadata")->text().contains(app.palette().color(QPalette::PlaceholderText).name()), "preview rich text refreshes semantic color");
        }
    }
    mode->setCurrentIndex(mode->findData("light"));
    increased->setChecked(false);
    reduced->setChecked(true);
    check(!FinderAppearance::dark() && !FinderAppearance::highContrast() && FinderAppearance::reduceTransparency()
        && QSettings().value("Appearance/reduceTransparency").toBool(), "settings changes apply immediately");

    // Destroy real preview receivers while a decoded image delivery is pending.
    QImage image(2200, 1800, QImage::Format_RGB32); image.fill(Qt::cyan);
    const QString imagePath = files.filePath("pending.png");
    check(image.save(imagePath), "pending preview fixture");
    auto *pending = new PreviewPane;
    pending->setUrls({QUrl::fromLocalFile(imagePath)});
    QPointer<PreviewPane> guard(pending);
    delete pending;
    auto *changing = new PreviewPane;
    changing->setUrls({QUrl::fromLocalFile(imagePath)});
    apply("light");
    changing->setUrls({QUrl::fromLocalFile(textFile.fileName())});
    auto *changedText = changing->findChild<QTextEdit *>("previewText");
    check(waitFor([&] { return changedText->toPlainText().contains("Readable preview"); }), "appearance changes preserve current worker source");
    check(changing->currentUrl() == QUrl::fromLocalFile(textFile.fileName()), "old worker cannot replace new preview selection");
    delete changing;
    apply("dark", true, true);
    check(waitFor([&] { return guard.isNull(); }), "stale preview worker receiver safety");
    bool deliveriesSettled = false;
    QTimer::singleShot(200, &app, [&] { deliveriesSettled = true; });
    check(waitFor([&] { return deliveriesSettled; }), "pending worker deliveries settle safely");
    qInstallMessageHandler(previousHandler);
    if (!warnings.isEmpty()) {
        for (const auto &warning : warnings) std::cerr << "Qt warning: " << warning.toStdString() << '\n';
        check(false, "real views render warning-free");
    }
    std::cout << "PASS appearance: palette modes, contrast, preferences, main-thread lifecycle, real views and stale receivers\n";
}
