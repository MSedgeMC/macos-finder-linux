// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
// Public Qt/KIO views and jobs; no private Dolphin ABI or shell file mutations.
#include "browser.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QColumnView>
#include <QDir>
#include <QDropEvent>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QListView>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QStackedWidget>
#include <QTimer>
#include <QTreeView>
#include <QTextEdit>
#include <QTextLayout>
#include <QVBoxLayout>
#include <KDirModel>
#include <KDirLister>
#include <KDirSortFilterProxyModel>
#include <KFileItemDelegate>
#include <KFilePreviewGenerator>
#include <KPropertiesDialog>
#include <KJobWidgets>
#include <KIO/CopyJob>
#include <KIO/DropJob>
#include <KIO/FileUndoManager>
#include <KIO/Job>
#include <KIO/JobUiDelegateFactory>
#include <KIO/MkdirJob>
#include <KIO/PasteJob>

#include <functional>

namespace {
using DropHandler = std::function<void(QDropEvent *, const QModelIndex &)>;

class FileDelegate : public KFileItemDelegate {
public:
    using KFileItemDelegate::KFileItemDelegate;
    std::function<void(const QModelIndex &, const QString &)> rename;
    void setModelData(QWidget *editor, QAbstractItemModel *, const QModelIndex &index) const override {
        if (auto *text = qobject_cast<QTextEdit *>(editor); text && rename) rename(index, text->toPlainText());
    }
};

class IconDelegate : public FileDelegate {
public:
    explicit IconDelegate(QListView *view) : FileDelegate(view), m_view(view) {}

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override {
        return QSize(m_view->gridSize().width(), m_view->iconSize().height() + 28 + 2 * option.fontMetrics.height());
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const int size = m_view->iconSize().width();
        const QRect iconRect(option.rect.center().x() - size / 2, option.rect.top() + 12, size, size);
        const QRectF iconHighlight = iconRect.adjusted(-5, -5, 5, 5);
        const bool selected = option.state & QStyle::State_Selected;
        const bool enabled = option.state & QStyle::State_Enabled;
        if (selected) {
            painter->setPen(Qt::NoPen); painter->setBrush(QColor("#363638"));
            painter->drawRoundedRect(iconHighlight, 8, 8);
        }
        if (option.state & QStyle::State_HasFocus) {
            painter->setPen(QPen(QColor("#6ca4f8"), 1)); painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(iconHighlight, 8, 8);
        }
        const QVariant decoration = index.data(Qt::DecorationRole);
        const QIcon icon = decoration.metaType() == QMetaType::fromType<QPixmap>() ?
                           QIcon(decoration.value<QPixmap>()) : decoration.value<QIcon>();
        icon.paint(painter, iconRect, Qt::AlignCenter, enabled ? QIcon::Normal : QIcon::Disabled);

        QFont font(option.font);
        const auto entry = index.data(KDirModel::FileItemRole).value<KFileItem>();
        if (entry.isLink()) font.setItalic(true);
        const QFontMetrics metrics(font);
        const int width = qMax(16, option.rect.width() - 12);
        const QString name = index.data(Qt::DisplayRole).toString();
        QTextLayout wrapped(name, font);
        QTextOption textOption(Qt::AlignHCenter);
        textOption.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        wrapped.setTextOption(textOption); wrapped.beginLayout();
        QStringList lines;
        for (int lineNumber = 0; lineNumber < 2; ++lineNumber) {
            QTextLine line = wrapped.createLine();
            if (!line.isValid()) break;
            line.setLineWidth(width);
            const QString text = lineNumber == 1 ? name.mid(line.textStart()) :
                                 name.mid(line.textStart(), line.textLength());
            lines.append(metrics.elidedText(text.trimmed(), Qt::ElideRight, width));
        }
        wrapped.endLayout();
        painter->setFont(font);
        const int lineHeight = metrics.height();
        const int labelTop = iconRect.bottom() + 9;
        int widest = 0;
        for (const auto &line : lines) widest = qMax(widest, metrics.horizontalAdvance(line));
        QRectF label(option.rect.center().x() - widest / 2.0 - 3, labelTop - 1,
                     widest + 6, lines.size() * lineHeight + 2);
        if (selected) {
            painter->setPen(Qt::NoPen); painter->setBrush(QColor("#296ec6"));
            painter->drawRoundedRect(label, 3, 3);
        }
        painter->setPen(selected ? QColor(Qt::white) :
                        option.palette.color(enabled ? QPalette::Active : QPalette::Disabled, QPalette::Text));
        for (int number = 0; number < lines.size(); ++number) {
            const QRect textRect(option.rect.left() + 6, labelTop + number * lineHeight, width, lineHeight);
            painter->drawText(textRect, Qt::AlignHCenter | Qt::AlignVCenter, lines.at(number));
        }
        painter->restore();
    }

    void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option, const QModelIndex &) const override {
        const int height = qMax(24, 2 * option.fontMetrics.height() + 6);
        editor->setGeometry(option.rect.left() + 5,
                            option.rect.top() + 12 + m_view->iconSize().height() + 6,
                            option.rect.width() - 10, height);
    }

private:
    QListView *m_view;
};

class ColumnDelegate : public FileDelegate {
public:
    using FileDelegate::FileDelegate;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QSize size = KFileItemDelegate::sizeHint(option, index);
        size.setHeight(24);
        size.rwidth() += 14;
        return size;
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QStyleOptionViewItem content(option);
        const bool reverse = option.direction == Qt::RightToLeft;
        content.rect.adjust(reverse ? 14 : 0, 0, reverse ? 0 : -14, 0);
        KFileItemDelegate::paint(painter, content, index);
        const auto entry = index.data(KDirModel::FileItemRole).value<KFileItem>();
        if (!entry.isDir()) return;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(option.palette.color(option.state & QStyle::State_Selected ?
                            QPalette::HighlightedText : QPalette::Text), 1.25));
        const qreal x = reverse ? option.rect.left() + 7 : option.rect.right() - 7;
        const qreal y = option.rect.center().y();
        const qreal direction = reverse ? -1 : 1;
        painter->drawPolyline(QPolygonF{{x - direction * 2, y - 3}, {x + direction * 1, y},
                                      {x - direction * 2, y + 3}});
        painter->restore();
    }
};

template <class View> class DropView : public View {
public:
    using View::View;
    DropHandler handler;
protected:
    void dropEvent(QDropEvent *event) override {
        auto index = this->indexAt(event->position().toPoint());
        if (!index.isValid()) index = this->rootIndex();
        if (handler) handler(event, index);
        else View::dropEvent(event);
    }
};

class ColumnsView : public DropView<QColumnView> {
public:
    using DropView::DropView;
    std::function<void(QAbstractItemView *)> prepare;
protected:
    QAbstractItemView *createColumn(const QModelIndex &index) override {
        auto *column = new DropView<QListView>;
        initializeColumn(column);
        column->setModel(model());
        column->setRootIndex(index);
        column->setIconSize(QSize(18, 18));
        column->setUniformItemSizes(true);
        column->setSpacing(0);
        column->handler = handler;
        if (prepare) prepare(column);
        if (model()->canFetchMore(index)) model()->fetchMore(index);
        return column;
    }
};

QUrl parentUrl(const QUrl &url) {
    return url.adjusted(QUrl::RemoveFilename | QUrl::StripTrailingSlash);
}

QUrl childUrl(QUrl directory, const QString &name) {
    QString path = directory.path();
    if (!path.endsWith('/')) path += '/';
    directory.setPath(path + name);
    return directory;
}

bool validName(const QString &name) {
    return !name.isEmpty() && name != "." && name != ".." &&
           !name.contains('/') && !name.contains(QChar::Null);
}
}

class Browser::Private {
public:
    Browser *q;
    KDirModel *model;
    KDirSortFilterProxyModel *proxy;
    QItemSelectionModel *selection;
    QItemSelectionModel *columnSelection;
    QStackedWidget *stack;
    DropView<QListView> *icons;
    DropView<QTreeView> *list;
    ColumnsView *columns;
    DropView<QListView> *strip;
    QWidget *gallery;
    QWidget *galleryHost;
    QWidget *columnHost;
    KFilePreviewGenerator *iconsPreview;
    KFilePreviewGenerator *galleryPreview;
    QUrl rootUrl;
    QUrl currentUrl;
    Mode currentMode = Icons;
    QList<QUrl> pendingSelection;
    bool changing = false;

    explicit Private(Browser *owner) : q(owner) {}

    KFileItem item(const QModelIndex &index) const {
        return index.isValid() ? model->itemForIndex(proxy->mapToSource(index.siblingAtColumn(0))) : KFileItem();
    }

    QItemSelectionModel *activeSelection() const {
        return currentMode == Columns ? columnSelection : selection;
    }

    QModelIndexList selectedNames() const {
        QModelIndexList names;
        // QListView and QColumnView select the name index; QTreeView selects
        // every metadata column. selectedRows() would miss the former.
        for (const auto &index : activeSelection()->selectedIndexes()) {
            // QColumnView retains ancestor highlights as the active column
            // changes. File actions apply only to this column's selection.
            if (index.column() == KDirModel::Name &&
                (currentMode != Columns || index.parent() == columnSelection->currentIndex().parent())) names.append(index);
        }
        return names;
    }

    void count() { emit q->countChanged(q->itemCount()); }

    void selected() {
        if (changing) return;
        auto items = q->selectedItems();
        if (currentMode == Columns) {
            const auto current = item(columnSelection->currentIndex());
            QUrl active = rootUrl;
            if (!current.isNull()) active = current.isDir() ? current.url() : parentUrl(current.url());
            if (active != currentUrl) {
                currentUrl = active;
                emit q->urlChanged(currentUrl);
                count();
            }
        }
        emit q->selectionChanged(items);
        if (items.size() == 1 && items.first().isFile()) emit q->previewRequested(items.first().url());
        else emit q->previewRequested({});
    }

    void activate(const QModelIndex &index) {
        const auto entry = item(index);
        if (entry.isNull()) return;
        if (entry.isDir()) {
            if (currentMode == Columns) {
                columnSelection->setCurrentIndex(index.siblingAtColumn(0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                if (proxy->canFetchMore(index)) proxy->fetchMore(index);
                columns->scrollTo(index);
                selected();
            } else q->navigate(entry.url());
        } else emit q->openRequested(entry.url());
    }

    void applySelection() {
        if (pendingSelection.isEmpty()) return;
        QItemSelection selected;
        QModelIndex current;
        for (const auto &url : pendingSelection) {
            const auto index = proxy->mapFromSource(model->indexForUrl(url));
            if (!index.isValid()) continue;
            selected.select(index, index);
            if (!current.isValid()) current = index;
        }
        if (!current.isValid()) return;
        pendingSelection.clear();
        activeSelection()->select(selected, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        activeSelection()->setCurrentIndex(current, QItemSelectionModel::NoUpdate);
        q->activeView()->scrollTo(current);
    }

    void job(KIO::Job *job, const QString &message) {
        job->setUiDelegate(KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, q->window()));
        KJobWidgets::setWindow(job, q->window());
        emit q->statusMessage(message);
        QObject::connect(job, &KJob::result, q, [this](KJob *finished) {
            if (finished->error()) emit q->errorOccurred(finished->errorString());
            else emit q->statusMessage(QStringLiteral("Operation complete"));
        });
    }

    void copyJob(KIO::CopyJob *job, const QString &message) {
        KIO::FileUndoManager::self()->recordCopyJob(job);
        this->job(job, message);
    }

    void rename(const QModelIndex &index, const QString &name) {
        const auto entry = item(index);
        if (entry.isNull() || name == entry.name()) return;
        if (!validName(name)) { emit q->errorOccurred("Enter a valid filename without a slash."); return; }
        const auto destination = childUrl(parentUrl(entry.url()), name);
        auto *operation = KIO::moveAs(entry.url(), destination);
        copyJob(operation, "Renaming…");
        QObject::connect(operation, &KJob::result, q, [this, destination](KJob *finished) {
            if (!finished->error()) q->selectUrls({destination});
        });
    }

    void context(QAbstractItemView *view, const QPoint &point) {
        const QModelIndex index = view->indexAt(point);
        auto *selected = activeSelection();
        if (index.isValid() && (!selected->isSelected(index) ||
            (currentMode == Columns && index.parent() != selected->currentIndex().parent()))) {
            selected->setCurrentIndex(index.siblingAtColumn(0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            if (currentMode == Columns) columns->scrollTo(index);
        } else if (!index.isValid()) {
            selected->clearSelection();
        }
        const bool hasSelection = !q->selectedUrls().isEmpty();
        QMenu menu(q);
        auto *open = menu.addAction(QStringLiteral("Open"), [this] { activate(activeSelection()->currentIndex()); });
        open->setEnabled(hasSelection);
        auto *look = menu.addAction(QStringLiteral("Quick Look"), [this] {
            const auto urls = q->selectedUrls();
            if (!urls.isEmpty()) emit q->quickLookRequested(urls);
        });
        look->setEnabled(hasSelection);
        menu.addSeparator();
        menu.addAction(QStringLiteral("New Folder"), q, &Browser::newFolder);
        auto *rename = menu.addAction(QStringLiteral("Rename"), q, &Browser::renameSelected);
        rename->setEnabled(q->selectedUrls().size() == 1);
        menu.addSeparator();
        auto *copy = menu.addAction(QStringLiteral("Copy"), [this] { q->copySelected(false); });
        auto *cut = menu.addAction(QStringLiteral("Cut"), [this] { q->copySelected(true); });
        copy->setEnabled(hasSelection); cut->setEnabled(hasSelection);
        menu.addAction(QStringLiteral("Paste"), [this] { q->paste(); })->setEnabled(QApplication::clipboard()->mimeData()->hasUrls());
        auto *duplicate = menu.addAction(QStringLiteral("Duplicate"), q, &Browser::duplicateSelected);
        duplicate->setEnabled(hasSelection);
        menu.addSeparator();
        auto *trash = menu.addAction(QStringLiteral("Move to Trash"), q, &Browser::trashSelected);
        trash->setEnabled(hasSelection);
        auto *info = menu.addAction(QStringLiteral("Get Info"), [this] {
            const auto items = q->selectedItems();
            auto *dialog = items.isEmpty() ? new KPropertiesDialog(currentUrl, q->window()) : new KPropertiesDialog(items, q->window());
            dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->show();
        });
        info->setEnabled(currentUrl.isValid());
        menu.exec(view->viewport()->mapToGlobal(point));
    }

    void prepare(QAbstractItemView *view, bool childColumn = false) {
        view->setSelectionMode(QAbstractItemView::ExtendedSelection);
        view->setSelectionBehavior(QAbstractItemView::SelectRows);
        view->setDragEnabled(true);
        view->setAcceptDrops(true);
        view->setDropIndicatorShown(true);
        view->setDragDropMode(QAbstractItemView::DragDrop);
        view->setDefaultDropAction(Qt::CopyAction);
        view->setEditTriggers(QAbstractItemView::NoEditTriggers);
        view->setContextMenuPolicy(Qt::CustomContextMenu);
        FileDelegate *delegate = view == icons || view == strip ? new IconDelegate(static_cast<QListView *>(view)) :
                                 childColumn ? new ColumnDelegate(view) : new FileDelegate(view);
        delegate->rename = [this](const QModelIndex &index, const QString &name) { rename(index, name); };
        if (view != icons && view != strip) delegate->setMaximumSize(QSize(10000, 24));
        view->setItemDelegate(delegate);
        view->installEventFilter(q);
        view->viewport()->installEventFilter(q);
        // QColumnView already forwards its child columns' activation signals.
        if (!childColumn) QObject::connect(view, &QAbstractItemView::doubleClicked, q, [this](const auto &index) { activate(index); });
        QObject::connect(view, &QWidget::customContextMenuRequested, q, [this, view](const auto &point) { context(view, point); });
    }
};

Browser::Browser(QWidget *parent) : QWidget(parent), d(std::make_unique<Private>(this)) {
    setObjectName(QStringLiteral("finderBrowser"));
    d->model = new KDirModel(this);
    d->model->setDropsAllowed(KDirModel::DropOnDirectory);
    d->model->dirLister()->setMainWindow(window());
    d->model->dirLister()->setAutoErrorHandlingEnabled(true);
    d->proxy = new KDirSortFilterProxyModel(this);
    d->proxy->setSourceModel(d->model);
    d->proxy->setSortFoldersFirst(false);
    d->proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    d->proxy->setFilterKeyColumn(0);
    d->proxy->sort(KDirModel::Name, Qt::AscendingOrder);
    d->selection = new QItemSelectionModel(d->proxy, this);
    // QColumnView swaps its selection among its child columns. Keep it separate
    // so hidden columns cannot react to selections or clear another view's state.
    d->columnSelection = new QItemSelectionModel(d->proxy, this);
    d->stack = new QStackedWidget(this);
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->addWidget(d->stack);
    d->icons = new DropView<QListView>(d->stack); d->icons->setObjectName("finderIconView");
    d->icons->setViewMode(QListView::IconMode); d->icons->setResizeMode(QListView::Adjust);
    d->icons->setMovement(QListView::Static); d->icons->setIconSize({96, 96});
    d->icons->setGridSize({128, 124 + 2 * fontMetrics().height()}); d->icons->setWordWrap(true);
    d->list = new DropView<QTreeView>(d->stack); d->list->setObjectName("finderListView");
    d->list->setRootIsDecorated(true); d->list->setAlternatingRowColors(true); d->list->setUniformRowHeights(true);
    d->list->setIconSize({18, 18}); d->list->setSortingEnabled(true);
    d->columns = new ColumnsView(d->stack); d->columns->setObjectName("finderColumnView");
    d->columns->setColumnWidths({220, 220, 220, 220, 220}); d->columns->setResizeGripsVisible(true);
    d->columnHost = new QWidget; d->columnHost->setObjectName("finderColumnPreview");
    d->columnHost->setMinimumWidth(250); d->columns->setPreviewWidget(d->columnHost);
    d->columns->prepare = [this](auto *view) { d->prepare(view, true); };
    d->gallery = new QWidget(d->stack); d->gallery->setObjectName("finderGalleryView");
    auto *galleryLayout = new QVBoxLayout(d->gallery); galleryLayout->setContentsMargins(0, 0, 0, 0); galleryLayout->setSpacing(0);
    d->galleryHost = new QWidget(d->gallery); d->galleryHost->setObjectName("finderGalleryPreview");
    galleryLayout->addWidget(d->galleryHost, 1);
    d->strip = new DropView<QListView>(d->gallery); d->strip->setObjectName("finderGalleryStrip");
    d->strip->setViewMode(QListView::IconMode); d->strip->setFlow(QListView::LeftToRight); d->strip->setWrapping(false);
    d->strip->setMovement(QListView::Static); d->strip->setIconSize({96, 96});
    d->strip->setGridSize({112, 124 + 2 * fontMetrics().height()}); d->strip->setWordWrap(true);
    d->strip->setFixedHeight(d->strip->gridSize().height() + 16); galleryLayout->addWidget(d->strip);
    const auto dropped = [this](QDropEvent *event, const QModelIndex &index) {
        const auto entry = d->item(index);
        drop(event, entry.isDir() ? entry.url() : (index.isValid() ? parentUrl(entry.url()) :
             (d->currentMode == Columns ? d->rootUrl : url())));
    };
    d->icons->handler = dropped; d->list->handler = dropped; d->columns->handler = dropped; d->strip->handler = dropped;
    for (auto *view : QList<QAbstractItemView *>{d->icons, d->list, d->columns, d->strip}) {
        view->setModel(d->proxy);
        auto *initialSelection = view->selectionModel();
        view->setSelectionModel(view == d->columns ? d->columnSelection : d->selection);
        initialSelection->deleteLater();
        d->prepare(view);
    }
    d->list->setColumnWidth(KDirModel::Name, 300); d->list->setColumnWidth(KDirModel::Size, 100);
    d->list->setColumnWidth(KDirModel::ModifiedTime, 185); d->list->setColumnWidth(KDirModel::Type, 160);
    for (int column : {KDirModel::Permissions, KDirModel::Owner, KDirModel::Group}) d->list->hideColumn(column);
    d->list->header()->moveSection(d->list->header()->visualIndex(KDirModel::Type), 3);
    d->list->header()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(d->list->header(), &QWidget::customContextMenuRequested, this, [this](const QPoint &point) {
        QMenu menu(d->list->header());
        for (int column = 0; column < KDirModel::ColumnCount; ++column) {
            auto *action = menu.addAction(d->proxy->headerData(column, Qt::Horizontal).toString());
            action->setCheckable(true); action->setChecked(!d->list->isColumnHidden(column));
            action->setEnabled(column != KDirModel::Name);
            connect(action, &QAction::toggled, this, [this, column](bool visible) { d->list->setColumnHidden(column, !visible); });
        }
        menu.exec(d->list->header()->mapToGlobal(point));
    });
    d->stack->addWidget(d->icons); d->stack->addWidget(d->list); d->stack->addWidget(d->columns); d->stack->addWidget(d->gallery);
    d->iconsPreview = new KFilePreviewGenerator(d->icons); d->galleryPreview = new KFilePreviewGenerator(d->strip);
    d->galleryPreview->setPreviewShown(false);
    connect(d->selection, &QItemSelectionModel::selectionChanged, this, [this] { if (d->currentMode != Columns) d->selected(); });
    connect(d->selection, &QItemSelectionModel::currentChanged, this, [this] { if (d->currentMode != Columns) d->selected(); });
    connect(d->columnSelection, &QItemSelectionModel::selectionChanged, this, [this] { if (d->currentMode == Columns) d->selected(); });
    connect(d->columnSelection, &QItemSelectionModel::currentChanged, this, [this] { if (d->currentMode == Columns) d->selected(); });
    connect(d->columns, &QColumnView::updatePreviewWidget, this, [this](const auto &index) {
        if (d->currentMode == Columns) {
            const auto item = d->item(index); emit previewRequested(item.isFile() ? item.url() : QUrl());
        }
    });
    connect(d->model->dirLister(), &KDirLister::jobError, this, [this](KIO::Job *job) { emit errorOccurred(job->errorString()); });
    connect(d->model, &QAbstractItemModel::rowsInserted, this, [this] { d->count(); d->applySelection(); });
    connect(d->model, &QAbstractItemModel::rowsRemoved, this, [this] { d->count(); });
    connect(d->model, &QAbstractItemModel::modelReset, this, [this] { d->count(); });
    connect(d->model->dirLister(), &KDirLister::completed, this, [this] { d->count(); d->applySelection(); emit statusMessage({}); });
    connect(d->model->dirLister(), &KDirLister::started, this, [this](const auto &) { emit statusMessage("Loading…"); });
    setStyleSheet("#finderBrowser QAbstractItemView{background:#1e1e1e;color:#ededf0;border:0;}"
                  "#finderBrowser QTreeView::item{height:24px;}"
                  "#finderBrowser QTreeView{alternate-background-color:#252525;}"
                  "#finderBrowser QAbstractItemView::item:selected{background:#3b3b40;color:white;}"
                  "#finderBrowser QColumnView::item{height:24px;}"
                  "#finderGalleryPreview,#finderColumnPreview{background:#1e1e1e;}"
                  "#finderBrowser QHeaderView::section{background:#29292b;color:#d7d7da;border:0;padding:4px 8px;}");
}

Browser::~Browser() = default;
QUrl Browser::url() const { return d->currentUrl; }
Browser::Mode Browser::mode() const { return d->currentMode; }
int Browser::iconSize() const { return d->icons->iconSize().width(); }
int Browser::sortColumn() const { return d->proxy->sortColumn(); }
bool Browser::sortAscending() const { return d->proxy->sortOrder() == Qt::AscendingOrder; }
QWidget *Browser::galleryPreviewHost() const { return d->galleryHost; }
QWidget *Browser::columnPreviewHost() const { return d->columnHost; }
KDirModel *Browser::directoryModel() const { return d->model; }
QAbstractItemView *Browser::activeView() const {
    switch (d->currentMode) { case List: return d->list; case Columns: return d->columns; case Gallery: return d->strip; default: return d->icons; }
}
KFileItemList Browser::selectedItems() const {
    KFileItemList result;
    for (const auto &index : d->selectedNames()) {
        const auto item = d->item(index);
        if (!item.isNull()) result.append(item);
    }
    return result;
}
QList<QUrl> Browser::selectedUrls() const { QList<QUrl> urls; for (const auto &item : selectedItems()) urls.append(item.url()); return urls; }
int Browser::itemCount() const {
    const auto index = d->currentUrl == d->rootUrl ? QModelIndex() : d->proxy->mapFromSource(d->model->indexForUrl(d->currentUrl));
    return d->proxy->rowCount(index);
}

void Browser::navigate(const QUrl &requested) {
    if (!requested.isValid() || requested.isEmpty()) return;
    QUrl target = requested.adjusted(QUrl::NormalizePathSegments | QUrl::StripTrailingSlash);
    if (target.isLocalFile()) {
        const QFileInfo info(target.toLocalFile());
        if (!info.exists() || !info.isDir()) { emit errorOccurred("Folder is unavailable: " + target.toDisplayString()); return; }
    }
    d->changing = true;
    d->selection->clear(); d->columnSelection->clear(); d->pendingSelection.clear();
    d->rootUrl = target; d->currentUrl = target;
    d->model->openUrl(target);
    // KDirLister can clear rows without a modelReset. Rebuild the column path
    // so invalidated child roots cannot become additional top-level columns.
    d->columns->setRootIndex(QModelIndex());
    d->changing = false;
    emit urlChanged(target); emit selectionChanged({}); emit previewRequested({});
}
void Browser::setMode(Mode mode) {
    if (mode < Icons || mode > Gallery || d->currentMode == mode) return;
    const auto urls = selectedUrls(); const QUrl current = d->currentUrl;
    const bool collapseColumns = d->currentMode == Columns && current != d->rootUrl;
    d->currentMode = mode; d->stack->setCurrentIndex(int(mode));
    // Disable the departing view's generator before enabling the new one;
    // both share the model's cached DecorationRole previews.
    if (mode == Gallery) { d->iconsPreview->setPreviewShown(false); d->galleryPreview->setPreviewShown(true); }
    else { d->galleryPreview->setPreviewShown(false); d->iconsPreview->setPreviewShown(mode == Icons); }
    if (collapseColumns) navigate(current);
    selectUrls(urls);
    activeView()->setFocus(); d->selected();
}
void Browser::setHiddenFiles(bool show) { d->model->dirLister()->setShowHiddenFiles(show); d->model->dirLister()->emitChanges(); }
void Browser::setFilter(const QString &filter) { d->proxy->setFilterFixedString(filter); d->count(); }
void Browser::setSort(int column, bool ascending) { d->proxy->sort(qBound(0, column, int(KDirModel::ColumnCount)-1), ascending ? Qt::AscendingOrder : Qt::DescendingOrder); }
void Browser::setIconSize(int size) {
    size = qBound(32, size, 256);
    d->icons->setIconSize(QSize(size, size));
    d->icons->setGridSize(QSize(qMax(96, size + 32), size + 28 + 2 * d->icons->fontMetrics().height()));
}
void Browser::setFoldersFirst(bool enabled) { d->proxy->setSortFoldersFirst(enabled); }
void Browser::selectUrls(const QList<QUrl> &urls) { d->pendingSelection = urls; if (urls.isEmpty()) d->activeSelection()->clearSelection(); else d->applySelection(); }
void Browser::renameSelected() {
    const auto rows = d->selectedNames(); if (rows.size() != 1) return;
    const QModelIndex index = rows.first();
    QAbstractItemView *view = activeView();
    if (d->currentMode == Columns) {
        for (auto *column : d->columns->findChildren<QListView *>()) {
            if (column->isVisible() && column->rootIndex() == index.parent()) { view = column; break; }
        }
    }
    view->setCurrentIndex(index); view->scrollTo(index); view->edit(index);
}
void Browser::newFolder() {
    if (!url().isValid()) return;
    bool ok = false;
    const auto name = QInputDialog::getText(window(), "New Folder", "Name:", QLineEdit::Normal, "Untitled Folder", &ok);
    if (!ok) return;
    if (!validName(name)) { emit errorOccurred("Enter a valid folder name without a slash."); return; }
    const auto destination = childUrl(url(), name);
    auto *job = KIO::mkdir(destination);
    KIO::FileUndoManager::self()->recordJob(KIO::FileUndoManager::Mkdir, {}, destination, job);
    d->job(job, "Creating folder…");
    connect(job, &KJob::result, this, [this, destination](KJob *job) { if (!job->error()) selectUrls({destination}); });
}
void Browser::copySelected(bool cut) {
    const auto urls = selectedUrls(); if (urls.isEmpty()) return;
    auto *mime = new QMimeData; mime->setUrls(urls);
    mime->setData("application/x-kde-cutselection", cut ? "1" : "0");
    QApplication::clipboard()->setMimeData(mime);
    emit statusMessage(QString::number(urls.size()) + (cut ? " item(s) ready to move" : " item(s) copied"));
}
void Browser::paste(bool move) {
    if (!url().isValid()) return;
    const auto *mime = QApplication::clipboard()->mimeData();
    if (move && mime->hasUrls()) {
        d->copyJob(KIO::move(mime->urls(), url()), "Moving…");
    } else {
        auto *job = KIO::paste(mime, url());
        d->job(job, "Pasting…");
        connect(job, &KIO::PasteJob::itemCreated, this, [this](const QUrl &created) { selectUrls({created}); });
    }
}
void Browser::trashSelected() { const auto urls = selectedUrls(); if (!urls.isEmpty()) d->copyJob(KIO::trash(urls), "Moving to Trash…"); }
void Browser::duplicateSelected() {
    for (const auto &source : selectedUrls()) {
        const QFileInfo info(source.fileName());
        const auto entry = d->model->dirLister()->findByUrl(source);
        QString suffix = entry.isDir() || info.suffix().isEmpty() ? QString() : "." + info.suffix();
        QString stem = suffix.isEmpty() ? source.fileName() : info.completeBaseName();
        const auto parent = parentUrl(source);
        QUrl destination = childUrl(parent, stem + " copy" + suffix);
        int index = 2;
        while (destination.isLocalFile() && QFileInfo::exists(destination.toLocalFile())) destination = childUrl(parent, stem + " copy " + QString::number(index++) + suffix);
        d->copyJob(KIO::copyAs(source, destination), "Duplicating…");
    }
}
void Browser::undo() { auto *manager = KIO::FileUndoManager::self(); manager->uiInterface()->setParentWidget(window()); if (manager->isUndoAvailable()) manager->undo(); }
void Browser::drop(QDropEvent *event, QUrl destination) {
    if (!event || !event->mimeData()->hasUrls()) { if (event) event->ignore(); return; }
    if (destination.isEmpty()) destination = url();
    auto *job = KIO::drop(event, destination);
    d->job(job, "Transferring…"); event->acceptProposedAction();
}
bool Browser::eventFilter(QObject *object, QEvent *event) {
    if (event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter || key->key() == Qt::Key_F2) && key->modifiers() == Qt::NoModifier) {
            renameSelected(); return true;
        }
        if (key->key() == Qt::Key_Space && key->modifiers() == Qt::NoModifier) {
            const auto urls = selectedUrls(); if (!urls.isEmpty()) emit quickLookRequested(urls);
            return true;
        }
    }
    return QWidget::eventFilter(object, event);
}
