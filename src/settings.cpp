// SPDX-License-Identifier: MIT
#include "settings.h"
#include "appearance.h"
#include "appearancecontrols.h"
#include <QApplication>
#include <KProtocolInfo>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
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
#include <QMessageBox>
#include <QMetaMethod>
#include <QMimeData>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTabBar>
#include <QShortcut>
#include <QStyle>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>

namespace {
constexpr auto tagMimeType = "application/x-finder-tag-id";
constexpr auto favoriteMimeType = "application/x-finder-favorite-slot";

class SettingsPage : public QScrollArea {
public:
    using QScrollArea::QScrollArea;
    QFormLayout *form = nullptr;
protected:
    void resizeEvent(QResizeEvent *event) override {
        if (form) form->setRowWrapPolicy(event->size().width() < 500
            ? QFormLayout::WrapAllRows : QFormLayout::WrapLongRows);
        QScrollArea::resizeEvent(event);
    }
};

class SettingsContent final : public QWidget {
public:
    using QWidget::QWidget;
    QSize minimumSizeHint() const override {
        const QSize original = QWidget::minimumSizeHint();
        return QSize(0, original.height());
    }
};

QIcon tagIcon(const QColor &color) {
    QPixmap pixmap(20, 20);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color.alpha() ? color.darker(115) : QColor("#8e8e93"), 1));
    painter.setBrush(color);
    painter.drawEllipse(QRectF(4, 4, 12, 12));
    return QIcon(pixmap);
}

class TagList : public QListWidget {
public:
    using QListWidget::QListWidget;
protected:
    QMimeData *mimeData(const QList<QListWidgetItem *> &items) const override {
        auto *mime = QListWidget::mimeData(items);
        if (!items.isEmpty()) mime->setData(tagMimeType, items.first()->data(Qt::UserRole).toString().toUtf8());
        return mime;
    }
    QStringList mimeTypes() const override {
        auto types = QListWidget::mimeTypes();
        types.append(tagMimeType);
        return types;
    }
};

// Slots are fixed positions, including empty ones. A drag within this list
// moves a slot (shifting the intervening slots); a registry drag replaces a
// slot. Neither operation removes tags from the independent sidebar registry.
class FavoriteList : public QListWidget {
public:
    using QListWidget::QListWidget;
    std::function<void(const QString &, int, int)> dropped;
protected:
    QMimeData *mimeData(const QList<QListWidgetItem *> &items) const override {
        auto *mime = new QMimeData;
        if (!items.isEmpty()) {
            mime->setData(tagMimeType, items.first()->data(Qt::UserRole).toString().toUtf8());
            mime->setData(favoriteMimeType, QByteArray::number(row(items.first())));
        }
        return mime;
    }
    QStringList mimeTypes() const override { return {tagMimeType, favoriteMimeType}; }
    bool accepts(const QMimeData *mime) const { return mime->hasFormat(tagMimeType); }
    void dragEnterEvent(QDragEnterEvent *event) override {
        if (accepts(event->mimeData())) event->acceptProposedAction(); else event->ignore();
    }
    void dragMoveEvent(QDragMoveEvent *event) override {
        if (accepts(event->mimeData())) event->acceptProposedAction(); else event->ignore();
    }
    void dropEvent(QDropEvent *event) override {
        const auto *target = itemAt(event->position().toPoint());
        if (!target || !accepts(event->mimeData()) || !dropped) { event->ignore(); return; }
        int source = -1;
        if (event->source() == this && event->mimeData()->hasFormat(favoriteMimeType))
            source = event->mimeData()->data(favoriteMimeType).toInt();
        dropped(QString::fromUtf8(event->mimeData()->data(tagMimeType)), source, row(target));
        // Persisted slots are reconstructed by the callback. Report Copy so
        // QAbstractItemView doesn't remove the newly reconstructed source row.
        event->setDropAction(Qt::CopyAction);
        event->accept();
    }
};
}

FinderSettings::FinderSettings(QWidget *parent) : QDialog(parent) {
    setObjectName("finderSettings");
    setWindowTitle(tr("Finder Settings"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(580, 540);
    setMinimumSize(370, 320);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 12, 16, 12);
    auto *tabs = new QTabWidget(this);
    tabs->setObjectName("finderSettingsTabs");
    tabs->setDocumentMode(true); tabs->setIconSize(QSize(20, 20));
    tabs->tabBar()->setExpanding(true); tabs->tabBar()->setAccessibleName(tr("Finder Settings sections"));
    auto restyle = [this] {
        const auto colors = FinderAppearance::palette();
        setStyleSheet(QStringLiteral(
            "#finderSettingsTabs::pane{border:0;} #finderSettingsTabs QTabBar::tab{border:0;border-radius:8px;padding:9px 12px;background:transparent;color:%1;}"
            "#finderSettingsTabs QTabBar::tab:selected{background:%2;color:%3;} #finderSettingsTabs QTabBar::tab:hover{background:%4;}"
            "QLabel[settingsSection=true]{color:%1;font-weight:600;padding-top:8px;}"
            "QLabel[settingsHelp=true]{color:%5;}")
            .arg(colors.color(QPalette::Text).name(), colors.color(QPalette::Highlight).name(),
                 colors.color(QPalette::HighlightedText).name(), colors.color(QPalette::Mid).name(), colors.color(QPalette::PlaceholderText).name()));
    };
    restyle(); connect(FinderAppearance::instance(), &FinderAppearance::changed, this, restyle);
    layout->addWidget(tabs);
    auto page = [&](const QString &label, const QString &name) {
        auto *scroll = new SettingsPage(tabs);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto *widget = new SettingsContent(scroll);
        widget->setObjectName(name);
        auto *form = new QFormLayout(widget);
        scroll->form = form;
        form->setContentsMargins(24, 20, 24, 20);
        form->setSpacing(12);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        // Wrapped help text must use the viewport width rather than force
        // a wider scroll widget through its unwrapped minimum-size hint.
        form->setSizeConstraint(QLayout::SetNoConstraint);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setWidget(widget);
        const auto icon = label == tr("General") ? QStyle::SP_ComputerIcon : label == tr("Tags") ? QStyle::SP_FileDialogDetailedView :
            label == tr("Sidebar") ? QStyle::SP_DirIcon : QStyle::SP_FileDialogInfoView;
        const int index = tabs->addTab(scroll, style()->standardIcon(icon), label);
        tabs->setTabToolTip(index, tr("%1 settings · Alt+%2").arg(label).arg(index + 1));
        auto *shortcut = new QShortcut(QKeySequence(QStringLiteral("Alt+%1").arg(index + 1)), this);
        connect(shortcut, &QShortcut::activated, tabs, [tabs, index] { tabs->setCurrentIndex(index); });
        return form;
    };
    auto section = [this](QFormLayout *form, const QString &text, const QString &name) {
        auto *label = new QLabel(text, this); label->setObjectName(name); label->setProperty("settingsSection", true); form->addRow(label);
    };
    auto option = [&](QFormLayout *form, const QString &text, const QString &key,
                      bool fallback, const QString &name) {
        auto *box = new QCheckBox(text, this);
        box->setObjectName(name);
        box->setProperty("preferenceKey", key);
        box->setProperty("preferenceDefault", fallback);
        box->setChecked(QSettings().value(key, fallback).toBool());
        form->addRow(box);
        connect(box, &QCheckBox::toggled, this, [this, key](bool checked) {
            QSettings().setValue(key, checked);
            if (key.startsWith(QStringLiteral("Appearance/")) || key == "View/alternatingRowColors") FinderAppearance::apply(qApp);
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
    section(general, tr("Appearance and accessibility"), "finderSettingsAppearanceHeading");
    auto *appearanceMode = new QComboBox(this);
    appearanceMode->setObjectName("finderSettingsAppearanceMode");
    appearanceMode->setAccessibleName(tr("Finder appearance"));
    appearanceMode->addItem(tr("Dark"), "dark");
    appearanceMode->addItem(tr("Light"), "light");
    appearanceMode->addItem(tr("Follow System"), "system");
    const int modeIndex = appearanceMode->findData(QSettings().value("Appearance/mode", "dark"));
    appearanceMode->setCurrentIndex(modeIndex < 0 ? 0 : modeIndex);
    general->addRow(tr("Appearance:"), appearanceMode);
    connect(appearanceMode, &QComboBox::currentIndexChanged, this, [this, appearanceMode] {
        QSettings().setValue("Appearance/mode", appearanceMode->currentData());
        FinderAppearance::apply(qApp);
        emit preferencesChanged();
    });
    option(general, tr("Increase contrast"), "Appearance/increaseContrast", false, "finderSettingsIncreaseContrast");
    auto *reduceTransparency = option(general, tr("Reduce transparency"), "Appearance/reduceTransparency", false, "finderSettingsReduceTransparency");
    reduceTransparency->setToolTip(tr("Use an opaque material for Finder window surfaces."));
    FinderAppearanceControls::addRows(general, this, [this] { emit preferencesChanged(); });
    option(general, tr("Alternating row colors in file lists"), "View/alternatingRowColors", true, "finderSettingsAlternatingRows");
    section(general, tr("Desktop"), "finderSettingsDesktopHeading");
    auto *desktop = new QLabel(tr("Desktop items are managed in Plasma’s Configure Desktop and Wallpaper settings."), this);
    desktop->setProperty("settingsHelp", true);
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
    m_tagList = new TagList(this);
    m_tagList->setObjectName("finderSettingsTagList");
    m_tagList->setAccessibleName(tr("Tags; checked tags appear in the sidebar"));
    m_tagList->setDragDropMode(QAbstractItemView::InternalMove);
    m_tagList->setDefaultDropAction(Qt::MoveAction);
    m_tagList->setMinimumHeight(160);
    tagPage->addRow(tr("Show these tags in the sidebar:"), m_tagList);
    m_tagName = new QLineEdit(this);
    m_tagName->setObjectName("finderSettingsTagName");
    m_tagName->setPlaceholderText(tr("Tag name"));
    m_tagName->setMaxLength(256);
    tagPage->addRow(tr("Name:"), m_tagName);
    m_tagColor = new QComboBox(this);
    m_tagColor->setObjectName("finderSettingsTagColor");
    m_tagColor->addItem(tagIcon(Qt::transparent), tr("None"), QColor(Qt::transparent).name(QColor::HexArgb));
    for (const auto &tag : FinderPreferences::standardTagRegistry()) m_tagColor->addItem(tagIcon(tag.color), tr(qPrintable(tag.name)), tag.color.name(QColor::HexArgb));
    tagPage->addRow(tr("Color:"), m_tagColor);
    auto buttonRow = [this](QFormLayout *form, const QList<QPushButton *> &buttons) {
        auto *widget = new QWidget(this);
        auto *row = new QHBoxLayout(widget);
        row->setContentsMargins(0, 0, 0, 0);
        for (auto *button : buttons) { button->setAutoDefault(false); row->addWidget(button); }
        form->addRow(widget);
    };
    m_addTag = new QPushButton(tr("+"), this);
    m_renameTag = new QPushButton(tr("Rename…"), this);
    m_deleteTag = new QPushButton(tr("Delete Tag…"), this);
    m_addTag->setObjectName("finderSettingsAddTag");
    m_addTag->setAccessibleName(tr("Create tag"));
    m_addTag->setToolTip(tr("Create a tag with this name and color"));
    m_renameTag->setObjectName("finderSettingsRenameTag");
    m_deleteTag->setObjectName("finderSettingsRemoveTag");
    buttonRow(tagPage, {m_addTag, m_renameTag, m_deleteTag});
    m_tagError = new QLabel(this);
    m_tagError->setObjectName("finderSettingsTagError");
    m_tagError->setWordWrap(true);
    tagPage->addRow(m_tagError);
    auto *favorites = new FavoriteList(this);
    m_favoriteList = favorites;
    favorites->setObjectName("finderSettingsFavoriteTags");
    favorites->setAccessibleName(tr("Seven ordered favorite tag slots"));
    favorites->setDragDropMode(QAbstractItemView::DragDrop);
    favorites->setDefaultDropAction(Qt::CopyAction);
    favorites->setViewMode(QListView::IconMode); favorites->setMovement(QListView::Static);
    favorites->setDragEnabled(true); favorites->setAcceptDrops(true); favorites->viewport()->setAcceptDrops(true);
    favorites->setFlow(QListView::LeftToRight); favorites->setWrapping(true); favorites->setResizeMode(QListView::Adjust);
    favorites->setIconSize(QSize(20, 20)); favorites->setGridSize(QSize(68, 58)); favorites->setSpacing(2);
    favorites->setMinimumHeight(130); favorites->setMaximumHeight(160);
    tagPage->addRow(tr("Favorite Tags:"), favorites);
    m_assignFavorite = new QPushButton(tr("Use Selected Tag"), this);
    m_clearFavorite = new QPushButton(tr("Clear Slot"), this);
    m_favoriteUp = new QPushButton(tr("↑"), this);
    m_favoriteDown = new QPushButton(tr("↓"), this);
    m_assignFavorite->setObjectName("finderSettingsAssignFavorite");
    m_clearFavorite->setObjectName("finderSettingsClearFavorite");
    m_favoriteUp->setObjectName("finderSettingsFavoriteUp");
    m_favoriteDown->setObjectName("finderSettingsFavoriteDown");
    m_favoriteUp->setAccessibleName(tr("Move favorite tag slot up"));
    m_favoriteDown->setAccessibleName(tr("Move favorite tag slot down"));
    m_favoriteUp->setFixedWidth(32);
    m_favoriteDown->setFixedWidth(32);
    buttonRow(tagPage, {m_assignFavorite, m_clearFavorite, m_favoriteUp, m_favoriteDown});
    auto *favoriteHelp = new QLabel(tr("Drag tags into the seven shortcut slots, or select a tag and a slot. Drag slots to reorder them. Clearing a slot keeps the tag and its file metadata."), this);
    favoriteHelp->setWordWrap(true);
    tagPage->addRow(favoriteHelp);
    option(tagPage, tr("Tint folders based on tags"), "Tags/tintFolders", false, "finderSettingsTintFolders");
    connect(m_tagList, &QListWidget::currentItemChanged, this, [this] { updateTagSelection(); });
    connect(m_tagList, &QListWidget::itemDoubleClicked, this, [this] { m_tagName->setFocus(); m_tagName->selectAll(); });
    connect(m_favoriteList, &QListWidget::currentItemChanged, this, [this] {
        const int row = m_favoriteList->currentRow();
        m_assignFavorite->setEnabled(m_pendingRequest.isEmpty() && m_tagList->currentItem() && row >= 0);
        m_clearFavorite->setEnabled(m_pendingRequest.isEmpty() && row >= 0 && !m_favoriteList->item(row)->data(Qt::UserRole).toString().isEmpty());
        m_favoriteUp->setEnabled(m_pendingRequest.isEmpty() && row > 0);
        m_favoriteDown->setEnabled(m_pendingRequest.isEmpty() && row >= 0 && row < 6);
    });
    connect(m_tagList, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        auto hidden = QSettings().value("Sidebar/hiddenTags").toStringList();
        hidden.removeAll(item->text());
        if (item->checkState() == Qt::Unchecked) hidden.append(item->text());
        QSettings().setValue("Sidebar/hiddenTags", hidden);
        emit preferencesChanged();
    });
    connect(m_tagList->model(), &QAbstractItemModel::rowsMoved, this, [this] {
        if (!m_pendingRequest.isEmpty()) return;
        const auto registry = FinderPreferences::tagRegistry();
        QList<FinderPreferences::Tag> ordered;
        for (int row = 0; row < m_tagList->count(); ++row)
            for (const auto &tag : registry)
                if (tag.id == m_tagList->item(row)->data(Qt::UserRole).toString()) { ordered.append(tag); break; }
        if (ordered.size() == registry.size()) { FinderPreferences::setTagRegistry(ordered); emit preferencesChanged(); }
    });
    connect(m_addTag, &QPushButton::clicked, this, [this] {
        const auto name = m_tagName->text().trimmed();
        auto tags = FinderPreferences::tagRegistry();
        if (!FinderPreferences::validTagName(name)) { m_tagError->setText(tr("Use 1–256 characters without commas or control characters.")); return; }
        if (std::any_of(tags.cbegin(), tags.cend(), [&name](const auto &tag) { return tag.name.compare(name, Qt::CaseInsensitive) == 0; })) {
            m_tagError->setText(tr("A tag with that name already exists.")); return;
        }
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        tags.append({id, name, QColor(m_tagColor->currentData().toString())});
        FinderPreferences::setTagRegistry(tags);
        m_tagError->clear();
        reloadTags();
        for (int row = 0; row < m_tagList->count(); ++row)
            if (m_tagList->item(row)->data(Qt::UserRole).toString() == id) m_tagList->setCurrentRow(row);
        emit preferencesChanged();
    });
    connect(m_tagColor, &QComboBox::currentIndexChanged, this, [this] {
        const auto *item = m_tagList->currentItem();
        if (!item || !m_pendingRequest.isEmpty()) return;
        // While entering a new name, the color belongs to the prospective tag.
        // Do not recolor the selected registry entry as a side effect of create.
        if (m_tagName->text().trimmed() != item->text()) return;
        auto tags = FinderPreferences::tagRegistry();
        for (auto &tag : tags)
            if (tag.id == item->data(Qt::UserRole).toString()) { tag.color = QColor(m_tagColor->currentData().toString()); break; }
        FinderPreferences::setTagRegistry(tags);
        reloadTags();
        emit preferencesChanged();
    });
    connect(m_renameTag, &QPushButton::clicked, this, [this] { requestTagMetadataChange(false); });
    connect(m_deleteTag, &QPushButton::clicked, this, [this] { requestTagMetadataChange(true); });
    favorites->dropped = [this](const QString &id, int source, int target) {
        if (!m_pendingRequest.isEmpty() || target < 0 || target >= 7) return;
        auto favoriteIds = FinderPreferences::favoriteTagIds();
        const auto tags = FinderPreferences::tagRegistry();
        if (source >= 0 && source < 7) favoriteIds.move(source, target);
        else {
            if (std::none_of(tags.cbegin(), tags.cend(), [&id](const auto &tag) { return tag.id == id; })) return;
            for (auto &other : favoriteIds) if (other == id) other.clear();
            favoriteIds[target] = id;
        }
        FinderPreferences::setFavoriteTagIds(favoriteIds);
        reloadTags(); m_favoriteList->setCurrentRow(target);
        emit preferencesChanged();
    };
    connect(m_assignFavorite, &QPushButton::clicked, this, [this, favorites] {
        if (const auto *item = m_tagList->currentItem()) favorites->dropped(item->data(Qt::UserRole).toString(), -1, m_favoriteList->currentRow());
    });
    connect(m_clearFavorite, &QPushButton::clicked, this, [this] {
        const int row = m_favoriteList->currentRow();
        if (row < 0) return;
        auto favoriteIds = FinderPreferences::favoriteTagIds();
        favoriteIds[row].clear(); FinderPreferences::setFavoriteTagIds(favoriteIds);
        reloadTags(); emit preferencesChanged();
    });
    connect(m_favoriteUp, &QPushButton::clicked, this, [this, favorites] {
        const int row = m_favoriteList->currentRow();
        favorites->dropped(QString(), row, row - 1);
    });
    connect(m_favoriteDown, &QPushButton::clicked, this, [this, favorites] {
        const int row = m_favoriteList->currentRow();
        favorites->dropped(QString(), row, row + 1);
    });
    reloadTags();

    auto *sidebar = page(tr("Sidebar"), "finderSettingsSidebar");
    auto *sidebarSize = new QComboBox(this);
    sidebarSize->setObjectName("finderSettingsSidebarItemSize");
    sidebarSize->setAccessibleName(tr("Sidebar icon size"));
    sidebarSize->addItem(tr("Small"), "small");
    sidebarSize->addItem(tr("Medium"), "medium");
    sidebarSize->addItem(tr("Large"), "large");
    const auto reloadSidebarSize = [sidebarSize] {
        const QSignalBlocker blocker(sidebarSize);
        const int index = sidebarSize->findData(QSettings().value("Sidebar/itemSize", "small"));
        sidebarSize->setCurrentIndex(index < 0 ? 0 : index);
    };
    reloadSidebarSize(); sidebar->addRow(tr("Sidebar icon size:"), sidebarSize);
    connect(FinderAppearance::instance(), &FinderAppearance::changed, sidebarSize, reloadSidebarSize);
    connect(sidebarSize, &QComboBox::currentIndexChanged, this, [this, sidebarSize] {
        QSettings().setValue("Sidebar/itemSize", sidebarSize->currentData());
        FinderAppearance::apply(qApp); emit preferencesChanged();
    });
    section(sidebar, tr("Favorites"), "finderSettingsFavoritesHeading");
    for (const auto &entry : QList<QPair<QString, QString>>{
        {"recents", tr("Recents")}, {"shared", tr("Shared")}, {"applications", tr("Applications")},
        {"desktop", tr("Desktop")}, {"documents", tr("Documents")}, {"downloads", tr("Downloads")},
        {"home", tr("Home")}, {"music", tr("Music")}, {"pictures", tr("Pictures")},
        {"videos", tr("Movies")}, {"internalDisks", tr("Hard disks")},
        {"externalDisks", tr("External disks")}, {"opticalMedia", tr("CDs, DVDs, and removable media")},
        {"connectedServers", tr("Connected servers")}, {"network", tr("Network")}, {"trash", tr("Trash")}}) {
        if (entry.first == "internalDisks") section(sidebar, tr("Locations"), "finderSettingsLocationsHeading");
        if (entry.first == "trash") section(sidebar, tr("Other"), "finderSettingsOtherHeading");
        auto *box = new QCheckBox(entry.second, this);
        box->setObjectName("finderSettingsSidebar_" + entry.first);
        if (entry.first == "internalDisks") box->setToolTip(tr("Includes Linux Filesystem."));
        box->setProperty("sidebarKey", entry.first);
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
    section(advanced, tr("Filename extensions"), "finderSettingsExtensionsHeading");
    option(advanced, tr("Show all filename extensions"), "Advanced/showAllExtensions", false, "finderSettingsShowExtensions");
    option(advanced, tr("Show a warning before changing an extension"), "Advanced/warnExtensionChange", true, "finderSettingsWarnExtension");
    section(advanced, tr("Trash"), "finderSettingsTrashHeading");
    option(advanced, tr("Show a warning before emptying the Trash"), "Advanced/warnEmptyTrash", true, "finderSettingsWarnTrash");
    auto *trashAge = option(advanced, tr("Remove items from the Trash after 30 days"), "Advanced/removeTrashAfter30Days", false, "finderSettingsTrash30Days");
    trashAge->setToolTip(tr("Age is counted from the time an item was moved to the Trash. Applies to supported local Trash locations while Finder is running."));
    section(advanced, tr("Keep folders on top"), "finderSettingsSortingHeading");
    option(advanced, tr("In windows when sorting by name"), "View/foldersFirst", false, "finderSettingsFoldersFirst");
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
    section(advanced, tr("Scrolling and motion"), "finderSettingsInputHeading");
    option(advanced, tr("Use smooth scrolling for mouse wheels"), "Input/smoothScrolling", true, "finderSettingsSmoothScroll");
    auto *wheelSpeed = new QSpinBox(this);
    wheelSpeed->setObjectName("finderSettingsWheelSpeed");
    wheelSpeed->setRange(10, 200); wheelSpeed->setSingleStep(5); wheelSpeed->setSuffix(tr("%"));
    wheelSpeed->setValue(qBound(10, QSettings().value("Input/wheelSpeed", 50).toInt(), 200));
    wheelSpeed->setToolTip(tr("Adjusts mouse-wheel distance inside Finder. Default: 50%. Precise touchpad scrolling uses the device's pixel motion."));
    advanced->addRow(tr("Mouse wheel speed:"), wheelSpeed);
    connect(wheelSpeed, &QSpinBox::valueChanged, this, [this](int value) {
        QSettings().setValue("Input/wheelSpeed", value); emit preferencesChanged();
    });
    option(advanced, tr("Reduce motion"), "Input/reduceMotion", false, "finderSettingsReduceMotion");
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
}

void FinderSettings::reloadTags() {
    const QSignalBlocker tagBlocker(m_tagList);
    const QSignalBlocker favoriteBlocker(m_favoriteList);
    const auto selectedId = m_tagList->currentItem() ? m_tagList->currentItem()->data(Qt::UserRole).toString() : QString();
    const int selectedSlot = m_favoriteList->currentRow();
    m_tagList->clear();
    m_favoriteList->clear();
    const auto hidden = QSettings().value("Sidebar/hiddenTags").toStringList();
    const auto tags = FinderPreferences::tagRegistry();
    for (const auto &tag : tags) {
        auto *item = new QListWidgetItem(tagIcon(tag.color), tag.name, m_tagList);
        item->setData(Qt::UserRole, tag.id);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
        item->setCheckState(hidden.contains(tag.name) ? Qt::Unchecked : Qt::Checked);
        item->setToolTip(tr("Check to show this tag in the sidebar. Drag to reorder."));
        if (tag.id == selectedId) m_tagList->setCurrentItem(item);
    }
    const auto favoriteIds = FinderPreferences::favoriteTagIds();
    for (int slot = 0; slot < 7; ++slot) {
        QString name;
        QColor color(Qt::transparent);
        for (const auto &tag : tags)
            if (tag.id == favoriteIds.at(slot)) { name = tag.name; color = tag.color; break; }
        auto *item = new QListWidgetItem(tagIcon(color), name.isEmpty() ? tr("None") : name, m_favoriteList);
        item->setToolTip(tr("Slot %1: %2").arg(slot + 1).arg(name.isEmpty() ? tr("None") : name));
        item->setData(Qt::UserRole, favoriteIds.at(slot));
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
    }
    if (m_tagList->currentRow() < 0 && m_tagList->count()) m_tagList->setCurrentRow(0);
    m_favoriteList->setCurrentRow(selectedSlot >= 0 && selectedSlot < 7 ? selectedSlot : 0);
    updateTagSelection();
}

void FinderSettings::updateTagSelection() {
    const bool pending = !m_pendingRequest.isEmpty();
    const auto *item = m_tagList->currentItem();
    const bool hasTag = item;
    const bool metadataAvailable = isSignalConnected(QMetaMethod::fromSignal(&FinderSettings::tagMetadataChangeRequested));
    m_tagList->setEnabled(!pending);
    m_favoriteList->setEnabled(!pending);
    m_tagName->setEnabled(!pending);
    m_tagColor->setEnabled(!pending);
    m_addTag->setEnabled(!pending);
    m_renameTag->setEnabled(!pending && hasTag && metadataAvailable);
    m_deleteTag->setEnabled(!pending && hasTag && metadataAvailable);
    m_renameTag->setToolTip(metadataAvailable ? tr("Rename this tag and update known local files") : tr("Tag metadata updates are unavailable."));
    m_deleteTag->setToolTip(metadataAvailable ? tr("Delete this tag and remove it from known local files") : tr("Tag metadata updates are unavailable."));
    if (!pending) {
        m_tagName->setText(item ? item->text() : QString());
        const QSignalBlocker colorBlocker(m_tagColor);
        const auto color = item ? FinderPreferences::registeredTagColor(item->text()) : QColor("#8e8e93");
        int colorIndex = m_tagColor->findData(color.name(QColor::HexArgb));
        if (colorIndex < 0) { m_tagColor->addItem(tagIcon(color), tr("Custom color"), color.name(QColor::HexArgb)); colorIndex = m_tagColor->count() - 1; }
        m_tagColor->setCurrentIndex(colorIndex);
    }
    const int slot = m_favoriteList->currentRow();
    m_assignFavorite->setEnabled(!pending && hasTag && slot >= 0);
    m_clearFavorite->setEnabled(!pending && slot >= 0 && !m_favoriteList->item(slot)->data(Qt::UserRole).toString().isEmpty());
    m_favoriteUp->setEnabled(!pending && slot > 0);
    m_favoriteDown->setEnabled(!pending && slot >= 0 && slot < 6);
}

void FinderSettings::requestTagMetadataChange(bool remove) {
    const auto *item = m_tagList->currentItem();
    if (!item || !m_pendingRequest.isEmpty() || !isSignalConnected(QMetaMethod::fromSignal(&FinderSettings::tagMetadataChangeRequested))) return;
    const auto newName = remove ? QString() : m_tagName->text().trimmed();
    if (!remove && !FinderPreferences::validTagName(newName)) { m_tagError->setText(tr("Use 1–256 characters without commas or control characters.")); return; }
    if (!remove && newName == item->text()) return;
    const auto id = item->data(Qt::UserRole).toString();
    for (const auto &tag : FinderPreferences::tagRegistry())
        if (!remove && tag.id != id && tag.name.compare(newName, Qt::CaseInsensitive) == 0) {
            m_tagError->setText(tr("A tag with that name already exists.")); return;
        }
    m_pendingRequest = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_pendingTagId = id;
    m_pendingOldName = item->text();
    m_pendingNewName = newName;
    m_tagError->clear();
    updateTagSelection();
    auto *confirmation = new QMessageBox(QMessageBox::Warning,
        remove ? tr("Delete Tag") : tr("Rename Tag"),
        remove ? tr("Delete “%1” and remove it from local files already discovered by Finder?").arg(m_pendingOldName)
               : tr("Rename “%1” to “%2” on local files already discovered by Finder?").arg(m_pendingOldName, m_pendingNewName),
        QMessageBox::Cancel, this);
    confirmation->setObjectName("finderSettingsTagConfirmation");
    confirmation->setAttribute(Qt::WA_DeleteOnClose);
    confirmation->setInformativeText(tr("This updates files in folders and search results Finder has loaded. Other files keep their existing tag; opening them later may add that tag to the registry again. File contents are unchanged."));
    auto *apply = confirmation->addButton(remove ? tr("Delete Tag") : tr("Rename Tag"), QMessageBox::AcceptRole);
    confirmation->setDefaultButton(QMessageBox::Cancel);
    confirmation->setEscapeButton(QMessageBox::Cancel);
    connect(confirmation, &QDialog::finished, this, [this, confirmation, apply] {
        if (confirmation->clickedButton() != apply) {
            m_pendingRequest.clear(); m_pendingTagId.clear(); m_pendingOldName.clear(); m_pendingNewName.clear();
            updateTagSelection(); return;
        }
        if (!isSignalConnected(QMetaMethod::fromSignal(&FinderSettings::tagMetadataChangeRequested))) {
            completeTagMetadataChange(m_pendingRequest, tr("Tag metadata updates are unavailable.")); return;
        }
        m_tagError->setText(tr("Updating known local files…"));
        emit tagMetadataChangeRequested(m_pendingRequest, m_pendingTagId, m_pendingOldName, m_pendingNewName);
    });
    confirmation->open();
}

void FinderSettings::completeTagMetadataChange(const QString &requestId, const QString &error) {
    if (requestId.isEmpty() || requestId != m_pendingRequest) return;
    if (error.isEmpty()) {
        auto tags = FinderPreferences::tagRegistry();
        auto favoriteIds = FinderPreferences::favoriteTagIds();
        auto hidden = QSettings().value("Sidebar/hiddenTags").toStringList();
        const bool wasHidden = hidden.contains(m_pendingOldName);
        hidden.removeAll(m_pendingOldName);
        if (m_pendingNewName.isEmpty()) {
            tags.removeIf([this](const auto &tag) { return tag.id == m_pendingTagId; });
            for (auto &id : favoriteIds) if (id == m_pendingTagId) id.clear();
        } else {
            // A directory refresh can observe the new name before this worker
            // completes. Fold that discovered entry into the original stable ID.
            QStringList mergedIds;
            for (const auto &tag : tags)
                if (tag.id != m_pendingTagId && tag.name == m_pendingNewName) mergedIds.append(tag.id);
            tags.removeIf([&mergedIds](const auto &tag) { return mergedIds.contains(tag.id); });
            for (auto &tag : tags) if (tag.id == m_pendingTagId) tag.name = m_pendingNewName;
            bool assigned = favoriteIds.contains(m_pendingTagId);
            for (auto &id : favoriteIds)
                if (mergedIds.contains(id)) { if (assigned) id.clear(); else { id = m_pendingTagId; assigned = true; } }
            if (wasHidden && !hidden.contains(m_pendingNewName)) hidden.append(m_pendingNewName);
        }
        FinderPreferences::setTagRegistry(tags);
        FinderPreferences::setFavoriteTagIds(favoriteIds);
        QSettings().setValue("Sidebar/hiddenTags", hidden);
    }
    m_pendingRequest.clear(); m_pendingTagId.clear(); m_pendingOldName.clear(); m_pendingNewName.clear();
    reloadTags();
    m_tagError->setText(error);
    if (error.isEmpty()) emit preferencesChanged();
}

void FinderSettings::reloadPreferences() {
    FinderPreferences::invalidateTagRegistryCache();
    if (auto *speed = findChild<QSpinBox *>("finderSettingsWheelSpeed")) {
        const QSignalBlocker blocker(speed);
        speed->setValue(qBound(10, QSettings().value("Input/wheelSpeed", 50).toInt(), 200));
    }
    for (auto *box : findChildren<QCheckBox *>()) {
        const QSignalBlocker blocker(box);
        const auto key = box->property("preferenceKey").toString();
        const auto sidebarKey = box->property("sidebarKey").toString();
        if (!key.isEmpty()) box->setChecked(QSettings().value(key, box->property("preferenceDefault")).toBool());
        else if (!sidebarKey.isEmpty()) box->setChecked(FinderPreferences::sidebarItemVisible(sidebarKey));
    }
    if (auto *folder = findChild<QComboBox *>("finderSettingsNewWindow")) {
        const QSignalBlocker blocker(folder);
        const auto url = FinderPreferences::newWindowUrl();
        const auto value = url.toString(QUrl::FullyEncoded);
        int index = folder->findData(value);
        if (index < 0) { folder->addItem(url.toDisplayString(QUrl::PreferLocalFile), value); index = folder->count() - 1; }
        folder->setCurrentIndex(index);
    }
    if (auto *scope = findChild<QComboBox *>("finderSettingsSearchScope")) {
        const QSignalBlocker blocker(scope);
        scope->setCurrentIndex(scope->findData(FinderPreferences::defaultSearchScope()));
    }
    if (auto *mode = findChild<QComboBox *>("finderSettingsAppearanceMode")) {
        const QSignalBlocker blocker(mode);
        const int index = mode->findData(QSettings().value("Appearance/mode", "dark"));
        mode->setCurrentIndex(index < 0 ? 0 : index);
    }
    reloadTags();
}

void FinderSettings::closeEvent(QCloseEvent *event) {
    if (!m_pendingRequest.isEmpty()) { event->ignore(); return; }
    QDialog::closeEvent(event);
}

void FinderSettings::reject() {
    if (m_pendingRequest.isEmpty()) QDialog::reject();
}

void FinderSettings::showEvent(QShowEvent *event) {
    reloadPreferences();
    QDialog::showEvent(event);
}
