// SPDX-License-Identifier: MIT
#include "settings.h"
#include "tags.h"
#include <KProtocolInfo>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QStandardPaths>
#include <QTabWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace FinderPreferences {
QUrl newWindowUrl() {
    const auto saved = QSettings().value("General/newWindowFolder").toString();
    if (!saved.isEmpty()) {
        // Older releases saved absolute paths. Do not reinterpret '#' or '%'
        // in those paths as URL syntax or replace an explicit saved choice.
        if (QDir::isAbsolutePath(saved)) return QUrl::fromLocalFile(saved);
        const QUrl url(saved);
        if (url.isValid() && !url.scheme().isEmpty()) return url;
    }
    return KProtocolInfo::isKnownProtocol("recentlyused")
        ? QUrl("recentlyused:/files") : QUrl::fromLocalFile(QDir::homePath());
}

QString defaultSearchScope() {
    QSettings settings;
    const auto scope = settings.value("Search/defaultScope").toString();
    if (scope == "computer" || scope == "current" || scope == "previous") return scope;
    // The old false choice searched Home, rather than the whole computer.
    // Migrate the explicit current-folder choice; broaden the other choice to
    // the documented computer scope, whose actual coverage the search reports.
    return settings.contains("Search/currentFolder") && settings.value("Search/currentFolder").toBool()
        ? QStringLiteral("current") : QStringLiteral("computer");
}

QString initialSearchScope() {
    const auto scope = defaultSearchScope();
    if (scope != "previous") return scope;
    const auto last = QSettings().value("Search/lastScope", "computer").toString();
    return last == "current" ? QStringLiteral("current") : QStringLiteral("computer");
}

void rememberSearchScope(const QString &scope) {
    if (scope == "computer" || scope == "current") QSettings().setValue("Search/lastScope", scope);
}

bool sidebarItemVisible(const QString &key) {
    QSettings settings;
    auto hidden = settings.value("Sidebar/hiddenKeys").toStringList();
    if (settings.value("Sidebar/locationGroupsVersion", 0).toInt() < 1) {
        if (hidden.removeAll("devices"))
            for (const auto &group : {"internalDisks", "externalDisks", "opticalMedia"})
                if (!hidden.contains(group)) hidden << group;
        if (hidden.contains("network") && !hidden.contains("connectedServers")) hidden << "connectedServers";
        settings.setValue("Sidebar/hiddenKeys", hidden);
        settings.setValue("Sidebar/locationGroupsVersion", 1);
    }
    if (hidden.contains(key)) return false;
    if (key == "filesystem" && hidden.contains("internalDisks")) return false;
    return true;
}
}

FinderSettings::FinderSettings(QWidget *parent) : QDialog(parent) {
    setObjectName("finderSettings");
    setWindowTitle(tr("Finder Settings"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(560, 520);
    auto *layout = new QVBoxLayout(this);
    auto *tabs = new QTabWidget(this);
    tabs->setObjectName("finderSettingsTabs");
    layout->addWidget(tabs);
    auto page = [&](const QString &label, const QString &name) {
        auto *scroll = new QScrollArea(tabs);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto *widget = new QWidget(scroll);
        widget->setObjectName(name);
        auto *form = new QFormLayout(widget);
        form->setContentsMargins(24, 20, 24, 20);
        form->setSpacing(12);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        scroll->setWidget(widget);
        tabs->addTab(scroll, label);
        return form;
    };
    auto option = [&](QFormLayout *form, const QString &text, const QString &key,
                      bool fallback, const QString &name) {
        auto *box = new QCheckBox(text, this);
        box->setObjectName(name);
        box->setChecked(QSettings().value(key, fallback).toBool());
        form->addRow(box);
        connect(box, &QCheckBox::toggled, this, [this, key](bool checked) {
            QSettings().setValue(key, checked);
            emit preferencesChanged();
        });
        return box;
    };
    auto *general = page(tr("General"), "finderSettingsGeneral");
    auto *folder = new QComboBox(this);
    folder->setObjectName("finderSettingsNewWindow");
    if (KProtocolInfo::isKnownProtocol("recentlyused"))
        folder->addItem(tr("Recents"), "recentlyused:/files");
    folder->addItem(tr("Applications"), QUrl::fromLocalFile("/usr/share/applications").toString(QUrl::FullyEncoded));
    for (const auto &entry : QList<QPair<QString, QStandardPaths::StandardLocation>>{
        {tr("Home"), QStandardPaths::HomeLocation}, {tr("Desktop"), QStandardPaths::DesktopLocation},
        {tr("Documents"), QStandardPaths::DocumentsLocation}, {tr("Downloads"), QStandardPaths::DownloadLocation},
        {tr("Music"), QStandardPaths::MusicLocation}, {tr("Pictures"), QStandardPaths::PicturesLocation},
        {tr("Movies"), QStandardPaths::MoviesLocation}}) {
        const auto path = QStandardPaths::writableLocation(entry.second);
        if (!path.isEmpty()) folder->addItem(entry.first, QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded));
    }
    if (KProtocolInfo::isKnownProtocol("remote")) folder->addItem(tr("Network"), "remote:/");
    const auto saved = FinderPreferences::newWindowUrl().toString(QUrl::FullyEncoded);
    int current = folder->findData(saved);
    if (current < 0) { folder->addItem(saved, saved); current = folder->count() - 1; }
    folder->setCurrentIndex(current);
    general->addRow(tr("New Finder windows show:"), folder);
    connect(folder, &QComboBox::currentIndexChanged, this, [this, folder] {
        QSettings().setValue("General/newWindowFolder", folder->currentData());
        emit preferencesChanged();
    });
    auto *chooseFolder = new QPushButton(tr("Other Folder…"), this);
    chooseFolder->setObjectName("finderSettingsChooseFolder");
    general->addRow(QString(), chooseFolder);
    connect(chooseFolder, &QPushButton::clicked, this, [this, folder] {
        auto *chooser = new QFileDialog(this, tr("Choose a Folder"));
        chooser->setAttribute(Qt::WA_DeleteOnClose);
        chooser->setFileMode(QFileDialog::Directory);
        chooser->setOption(QFileDialog::ShowDirsOnly);
        chooser->setDirectoryUrl(FinderPreferences::newWindowUrl());
        connect(chooser, &QFileDialog::urlSelected, this, [folder](const QUrl &url) {
            if (!url.isValid() || url.isEmpty()) return;
            const auto value = url.toString(QUrl::FullyEncoded);
            int index = folder->findData(value);
            if (index < 0) { folder->addItem(url.toDisplayString(QUrl::PreferLocalFile), value); index = folder->count() - 1; }
            folder->setCurrentIndex(index);
        });
        chooser->open();
    });
    option(general, tr("Open folders in tabs instead of new windows"), "General/openFoldersInTabs", true, "finderSettingsOpenTabs");
    auto *desktop = new QLabel(tr("Plasma manages desktop icons and folder sorting. Use Configure Desktop and Wallpaper on the desktop for Folder View options, or System Settings for desktop appearance."), this);
    desktop->setWordWrap(true);
    general->addRow(desktop);
    auto *desktopSettings = new QPushButton(tr("Open System Settings…"), this);
    desktopSettings->setObjectName("finderSettingsDesktopSettings");
    const auto systemSettingsProgram = QStandardPaths::findExecutable("systemsettings");
    desktopSettings->setEnabled(!systemSettingsProgram.isEmpty());
    general->addRow(QString(), desktopSettings);
    auto *desktopError = new QLabel(this);
    desktopError->setWordWrap(true);
    general->addRow(desktopError);
    connect(desktopSettings, &QPushButton::clicked, this, [this, systemSettingsProgram, desktopError] {
        if (!QProcess::startDetached(systemSettingsProgram, QStringList()))
            desktopError->setText(tr("System Settings could not be opened."));
    });

    auto *tagPage = page(tr("Tags"), "finderSettingsTags");
    auto *tagList = new QListWidget(this);
    tagList->setObjectName("finderSettingsTagList");
    const auto hiddenTags = QSettings().value("Sidebar/hiddenTags").toStringList();
    auto addTag = [tagList, hiddenTags](const QString &tag) {
        auto *item = new QListWidgetItem(tag, tagList);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        item->setCheckState(hiddenTags.contains(tag) ? Qt::Unchecked : Qt::Checked);
    };
    for (const auto &tag : FileTags::standardTags()) addTag(tag);
    for (const auto &tag : QSettings().value("Tags/favorites").toStringList())
        if (!FileTags::standardTags().contains(tag)) addTag(tag);
    tagPage->addRow(tr("Favorite tags in the sidebar:"), tagList);
    connect(tagList, &QListWidget::itemChanged, this, [this, tagList] {
        QStringList hidden;
        for (int i = 0; i < tagList->count(); ++i)
            if (tagList->item(i)->checkState() == Qt::Unchecked) hidden << tagList->item(i)->text();
        QSettings().setValue("Sidebar/hiddenTags", hidden);
        emit preferencesChanged();
    });
    auto *tagInput = new QLineEdit(this);
    tagInput->setObjectName("finderSettingsTagName");
    tagInput->setPlaceholderText(tr("New favorite tag"));
    auto *tagButtons = new QWidget(this);
    auto *tagRow = new QHBoxLayout(tagButtons);
    tagRow->setContentsMargins(0, 0, 0, 0);
    auto *add = new QPushButton(tr("+"), this);
    auto *remove = new QPushButton(tr("−"), this);
    add->setObjectName("finderSettingsAddTag"); remove->setObjectName("finderSettingsRemoveTag");
    tagRow->addWidget(tagInput); tagRow->addWidget(add); tagRow->addWidget(remove);
    tagPage->addRow(tagButtons);
    auto *tagError = new QLabel(this); tagError->setObjectName("finderSettingsTagError");
    tagError->setWordWrap(true); tagPage->addRow(tagError);
    connect(add, &QPushButton::clicked, this, [this, tagInput, tagList, tagError, addTag] {
        const auto tag = tagInput->text().trimmed();
        if (tag.isEmpty() || tag.size() > 128 || tag.contains(',') ||
            std::any_of(tag.cbegin(), tag.cend(), [](QChar c) { return c.category() == QChar::Other_Control; })) {
            tagError->setText(tr("Enter a tag without commas or control characters.")); return;
        }
        for (int i = 0; i < tagList->count(); ++i)
            if (tagList->item(i)->text().compare(tag, Qt::CaseInsensitive) == 0) return;
        auto favorites = QSettings().value("Tags/favorites").toStringList(); favorites << tag;
        QSettings().setValue("Tags/favorites", favorites); addTag(tag);
        tagInput->clear(); tagError->clear(); emit preferencesChanged();
    });
    connect(remove, &QPushButton::clicked, this, [this, tagList] {
        auto *item = tagList->currentItem();
        if (!item || FileTags::standardTags().contains(item->text())) return;
        auto favorites = QSettings().value("Tags/favorites").toStringList(); favorites.removeAll(item->text());
        QSettings().setValue("Tags/favorites", favorites);
        delete tagList->takeItem(tagList->row(item)); emit preferencesChanged();
    });
    tagPage->addRow(new QLabel(tr("Removing a favorite keeps tags stored on your files."), this));

    auto *sidebar = page(tr("Sidebar"), "finderSettingsSidebar");
    for (const auto &entry : QList<QPair<QString, QString>>{
        {"recents", tr("Recents")}, {"shared", tr("Shared")}, {"applications", tr("Applications")},
        {"desktop", tr("Desktop")}, {"documents", tr("Documents")}, {"downloads", tr("Downloads")},
        {"home", tr("Home")}, {"music", tr("Music")}, {"pictures", tr("Pictures")},
        {"videos", tr("Movies")}, {"internalDisks", tr("Hard disks (including Linux Filesystem)")},
        {"externalDisks", tr("External disks")}, {"opticalMedia", tr("CDs, DVDs, and other removable media")},
        {"connectedServers", tr("Connected servers")}, {"network", tr("Network")}, {"trash", tr("Trash")}}) {
        auto *box = new QCheckBox(entry.second, this);
        box->setObjectName("finderSettingsSidebar_" + entry.first);
        box->setChecked(FinderPreferences::sidebarItemVisible(entry.first)); sidebar->addRow(box);
        connect(box, &QCheckBox::toggled, this, [this, key = entry.first](bool checked) {
            auto keys = QSettings().value("Sidebar/hiddenKeys").toStringList();
            if (key == "internalDisks") keys.removeAll("filesystem");
            keys.removeAll(key); if (!checked) keys << key;
            QSettings().setValue("Sidebar/hiddenKeys", keys); emit preferencesChanged();
        });
    }
    auto *sidebarHelp = new QLabel(tr("Show available locations in Finder. Devices appear when connected. Other applications use their own sidebar settings."), this);
    sidebarHelp->setWordWrap(true);
    sidebar->addRow(sidebarHelp);
    auto *advanced = page(tr("Advanced"), "finderSettingsAdvanced");
    option(advanced, tr("Show all filename extensions"), "Advanced/showAllExtensions", false, "finderSettingsShowExtensions");
    option(advanced, tr("Show a warning before changing an extension"), "Advanced/warnExtensionChange", true, "finderSettingsWarnExtension");
    option(advanced, tr("Show a warning before emptying the Trash"), "Advanced/warnEmptyTrash", true, "finderSettingsWarnTrash");
    auto *trashAge = option(advanced, tr("Remove items from the Trash after 30 days"), "Advanced/removeTrashAfter30Days", false, "finderSettingsTrash30Days");
    trashAge->setToolTip(tr("Age is counted from the time an item was moved to the Trash. Applies to supported local Trash locations while Finder is running."));
    option(advanced, tr("Keep folders on top when sorting by name"), "View/foldersFirst", false, "finderSettingsFoldersFirst");
    auto *searchScope = new QComboBox(this);
    searchScope->setObjectName("finderSettingsSearchScope");
    searchScope->addItem(tr("Search This Computer"), "computer");
    searchScope->addItem(tr("Search the Current Folder"), "current");
    searchScope->addItem(tr("Use the Previous Search Scope"), "previous");
    searchScope->setCurrentIndex(searchScope->findData(FinderPreferences::defaultSearchScope()));
    advanced->addRow(tr("When performing a search:"), searchScope);
    connect(searchScope, &QComboBox::currentIndexChanged, this, [this, searchScope] {
        QSettings settings;
        settings.setValue("Search/defaultScope", searchScope->currentData());
        settings.remove("Search/currentFolder");
        emit preferencesChanged();
    });
    option(advanced, tr("Use smooth scrolling for mouse wheels"), "Input/smoothScrolling", true, "finderSettingsSmoothScroll");
    option(advanced, tr("Reduce motion"), "Input/reduceMotion", false, "finderSettingsReduceMotion");
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
}
