// SPDX-License-Identifier: MIT
#include "settings.h"
#include "tags.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QStandardPaths>
#include <QTabWidget>
#include <QVBoxLayout>
#include <algorithm>

FinderSettings::FinderSettings(QWidget *parent) : QDialog(parent) {
    setObjectName("finderSettings");
    setWindowTitle(tr("Finder Settings"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(490, 420);
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
    for (const auto &entry : QList<QPair<QString, QStandardPaths::StandardLocation>>{
        {tr("Home"), QStandardPaths::HomeLocation}, {tr("Desktop"), QStandardPaths::DesktopLocation},
        {tr("Documents"), QStandardPaths::DocumentsLocation}, {tr("Downloads"), QStandardPaths::DownloadLocation}})
        folder->addItem(entry.first, QStandardPaths::writableLocation(entry.second));
    const auto saved = QSettings().value("General/newWindowFolder", QDir::homePath()).toString();
    int current = folder->findData(saved);
    if (current < 0) { folder->addItem(saved, saved); current = folder->count() - 1; }
    folder->setCurrentIndex(current);
    general->addRow(tr("New Finder windows show:"), folder);
    connect(folder, &QComboBox::currentIndexChanged, this, [this, folder] {
        QSettings().setValue("General/newWindowFolder", folder->currentData());
        emit preferencesChanged();
    });
    option(general, tr("Open folders in tabs instead of new windows"), "General/openFoldersInTabs", true, "finderSettingsOpenTabs");
    auto *desktop = new QLabel(tr("Desktop items are managed by Plasma's desktop settings."), this);
    desktop->setWordWrap(true);
    general->addRow(desktop);

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
    const auto hidden = QSettings().value("Sidebar/hiddenKeys").toStringList();
    for (const auto &entry : QList<QPair<QString, QString>>{
        {"recents", tr("Recents")}, {"shared", tr("Shared")}, {"applications", tr("Applications")},
        {"desktop", tr("Desktop")}, {"documents", tr("Documents")}, {"downloads", tr("Downloads")},
        {"home", tr("Home")}, {"music", tr("Music")}, {"pictures", tr("Pictures")},
        {"videos", tr("Movies")}, {"devices", tr("Hard disks and external disks")},
        {"network", tr("Connected servers and Network")}, {"trash", tr("Trash")}}) {
        auto *box = new QCheckBox(entry.second, this);
        box->setObjectName("finderSettingsSidebar_" + entry.first);
        box->setChecked(!hidden.contains(entry.first)); sidebar->addRow(box);
        connect(box, &QCheckBox::toggled, this, [this, key = entry.first](bool checked) {
            auto keys = QSettings().value("Sidebar/hiddenKeys").toStringList();
            keys.removeAll(key); if (!checked) keys << key;
            QSettings().setValue("Sidebar/hiddenKeys", keys); emit preferencesChanged();
        });
    }
    auto *advanced = page(tr("Advanced"), "finderSettingsAdvanced");
    option(advanced, tr("Keep folders on top when sorting by name"), "View/foldersFirst", false, "finderSettingsFoldersFirst");
    option(advanced, tr("Search the current folder by default"), "Search/currentFolder", true, "finderSettingsSearchFolder");
    option(advanced, tr("Use smooth scrolling for mouse wheels"), "Input/smoothScrolling", true, "finderSettingsSmoothScroll");
    option(advanced, tr("Reduce motion"), "Input/reduceMotion", false, "finderSettingsReduceMotion");
    option(advanced, tr("Show a warning before emptying the Trash"), "Advanced/warnEmptyTrash", true, "finderSettingsWarnTrash");
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
}
