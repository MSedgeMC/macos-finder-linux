// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#include "searchresults.h"

#include <KFileItem>
#include <KIO/PreviewJob>
#include <KJob>
#include <QApplication>
#include <QAbstractItemView>
#include <QCache>
#include <QContextMenuEvent>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QListView>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QScrollBar>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>
#include <utility>

namespace {
QUrl indexUrl(const QModelIndex &index)
{
    return index.isValid() ? index.siblingAtColumn(0).data(Qt::UserRole).toUrl() : QUrl();
}
QList<QUrl> urlsForIndexes(QModelIndexList indexes)
{
    std::sort(indexes.begin(), indexes.end(), [](const QModelIndex &a, const QModelIndex &b) { return a.row() < b.row(); });
    QList<QUrl> urls;
    QSet<QUrl> seen;
    for (const auto &index : indexes) {
        const auto url = indexUrl(index);
        if (!url.isValid() || url.isEmpty() || seen.contains(url)) continue;
        seen.insert(url);
        urls.append(url);
    }
    return urls;
}

class ResultsProxy : public QSortFilterProxyModel {
public:
    explicit ResultsProxy(QObject *parent) : QSortFilterProxyModel(parent)
    {
        setSortCaseSensitivity(Qt::CaseInsensitive);
        setSortLocaleAware(true);
        setDynamicSortFilter(true);
    }
    // One 16 MiB memory cache in addition to the native disk cache. Painting
    // only consults model data and this cache: no stat, MIME sniff or content IO.
    mutable QCache<QUrl, QPixmap> thumbnails{16384}; // costs in KiB
    QVariant data(const QModelIndex &index, int role) const override
    {
        if (index.isValid() && index.column() == 0 && role == Qt::DecorationRole) {
            // Call the base directly; using indexUrl here would recurse.
            const auto url = QSortFilterProxyModel::data(index, Qt::UserRole).toUrl();
            if (const auto *pixmap = thumbnails.object(url)) return QIcon(*pixmap);
        }
        return QSortFilterProxyModel::data(index, role);
    }
    Qt::ItemFlags flags(const QModelIndex &index) const override
    {
        auto result = QSortFilterProxyModel::flags(index) & ~Qt::ItemIsDropEnabled;
        if (indexUrl(index).isValid() && !indexUrl(index).isEmpty()) result |= Qt::ItemIsDragEnabled;
        return result;
    }
    QStringList mimeTypes() const override { return {QStringLiteral("text/uri-list")}; }
    QMimeData *mimeData(const QModelIndexList &indexes) const override
    {
        auto *data = new QMimeData;
        data->setUrls(urlsForIndexes(indexes));
        return data;
    }
    Qt::DropActions supportedDragActions() const override { return Qt::CopyAction | Qt::MoveAction | Qt::LinkAction; }
    void repaintDecorations()
    {
        if (rowCount() > 0) emit dataChanged(index(0, 0), index(rowCount() - 1, 0), {Qt::DecorationRole});
    }
    void repaintUrl(const QUrl &url)
    {
        // Search is capped at 2000 source rows. Delivery, never paint, walks
        // their exact URLs so sorting/appends cannot redirect a thumbnail.
        for (int row = 0; row < rowCount(); ++row) {
            const auto hit = index(row, 0);
            if (indexUrl(hit) == url) emit dataChanged(hit, hit, {Qt::DecorationRole});
        }
    }
};

// Native item-view navigation/drag behavior, with the Finder Meta modifier
// translated to the Linux Control selection modifier. Shift anchors are shared
// across views, including keyboard navigation after a mode change.
template<class Base> class ResultsView : public Base {
public:
    explicit ResultsView(QWidget *parent) : Base(parent) {}
    std::function<void(const QModelIndex &)> anchor;
    std::function<void(const QModelIndex &, bool)> range;
protected:
    QItemSelectionModel::SelectionFlags selectionCommand(const QModelIndex &index, const QEvent *event) const override
    {
        if (m_rangePress) return QItemSelectionModel::NoUpdate;
        if (event && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease)) {
            const auto *mouse = static_cast<const QMouseEvent *>(event);
            if (mouse->modifiers().testFlag(Qt::ShiftModifier)) return QItemSelectionModel::NoUpdate;
            if (mouse->modifiers().testFlag(Qt::MetaModifier)) {
                QMouseEvent mapped(mouse->type(), mouse->position(), mouse->scenePosition(), mouse->globalPosition(),
                    mouse->button(), mouse->buttons(), (mouse->modifiers() & ~Qt::MetaModifier) | Qt::ControlModifier, mouse->pointingDevice());
                return Base::selectionCommand(index, &mapped);
            }
        }
        return Base::selectionCommand(index, event);
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        const auto hit = this->indexAt(event->position().toPoint()).siblingAtColumn(0);
        m_rangePress = event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::ShiftModifier) && hit.isValid();
        if (m_rangePress && range) range(hit, event->modifiers().testFlag(Qt::ControlModifier) || event->modifiers().testFlag(Qt::MetaModifier));
        Base::mousePressEvent(event);
        if (!m_rangePress && event->button() == Qt::LeftButton && hit.isValid() && anchor) anchor(hit);
        m_rangePress = false;
    }
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->modifiers().testFlag(Qt::MetaModifier)) {
            QKeyEvent mapped(event->type(), event->key(), (event->modifiers() & ~Qt::MetaModifier) | Qt::ControlModifier,
                event->text(), event->isAutoRepeat(), event->count());
            key(&mapped);
            event->setAccepted(mapped.isAccepted());
        } else key(event);
    }
private:
    bool m_rangePress = false;
    void key(QKeyEvent *event)
    {
        typename Base::CursorAction action = Base::MoveNext;
        bool navigation = true;
        switch (event->key()) {
        case Qt::Key_Up: action = Base::MoveUp; break;
        case Qt::Key_Down: action = Base::MoveDown; break;
        case Qt::Key_Left: action = Base::MoveLeft; break;
        case Qt::Key_Right: action = Base::MoveRight; break;
        case Qt::Key_Home: action = Base::MoveHome; break;
        case Qt::Key_End: action = Base::MoveEnd; break;
        case Qt::Key_PageUp: action = Base::MovePageUp; break;
        case Qt::Key_PageDown: action = Base::MovePageDown; break;
        default: navigation = false; break;
        }
        if (navigation && event->modifiers().testFlag(Qt::ShiftModifier) && range) {
            const auto next = this->moveCursor(action, event->modifiers()).siblingAtColumn(0);
            if (next.isValid()) { range(next, event->modifiers().testFlag(Qt::ControlModifier)); this->scrollTo(next); }
            event->accept();
            return;
        }
        Base::keyPressEvent(event);
        if (navigation && !event->modifiers().testFlag(Qt::ControlModifier) && anchor) anchor(this->currentIndex().siblingAtColumn(0));
    }
};

class LocationDelegate : public QStyledItemDelegate {
public:
    explicit LocationDelegate(QObject *parent) : QStyledItemDelegate(parent) {}
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override
    {
        return {160, qMax(46, 2 * option.fontMetrics.height() + 12)};
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem item(option);
        initStyleOption(&item, index);
        const auto name = item.text;
        item.text.clear();
        const auto *widget = option.widget;
        auto *style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &item, painter, widget);
        const QRect text = option.rect.adjusted(40, 5, -8, -5);
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const auto group = !option.state.testFlag(QStyle::State_Enabled) ? QPalette::Disabled :
            option.state.testFlag(QStyle::State_Active) ? QPalette::Active : QPalette::Inactive;
        painter->save();
        painter->setFont(option.font);
        painter->setPen(option.palette.color(group, selected ? QPalette::HighlightedText : QPalette::Text));
        painter->drawText(text, Qt::AlignLeft | Qt::AlignTop, option.fontMetrics.elidedText(name, Qt::ElideMiddle, text.width()));
        painter->setPen(option.palette.color(group, selected ? QPalette::HighlightedText : QPalette::PlaceholderText));
        const auto location = index.siblingAtColumn(2).data().toString();
        painter->drawText(text, Qt::AlignLeft | Qt::AlignBottom, option.fontMetrics.elidedText(location, Qt::ElideMiddle, text.width()));
        painter->restore();
    }
};
}

class SearchResults::Private {
public:
    explicit Private(SearchResults *owner) : q(owner) {}
    SearchResults *q;
    ResultsProxy *proxy = nullptr;
    QItemSelectionModel *selection = nullptr;
    QStackedWidget *stack = nullptr;
    ResultsView<QListView> *icons = nullptr;
    ResultsView<QTreeView> *list = nullptr;
    ResultsView<QListView> *columns = nullptr;
    ResultsView<QListView> *strip = nullptr;
    QWidget *gallery = nullptr;
    QWidget *galleryHost = nullptr;
    QLabel *locationName = nullptr;
    QLabel *locationKind = nullptr;
    QLabel *locationPath = nullptr;
    QLabel *locationUrl = nullptr;
    QPersistentModelIndex selectionAnchor;
    int mode = SearchResults::List;
    QList<QMetaObject::Connection> sourceConnections;
    QTimer thumbnailTimer;
    QPointer<KIO::PreviewJob> thumbnailJob;
    QSet<QUrl> failures;
    QSet<QUrl> visibleRequests;
    quint64 generation = 0;

    void selectRange(const QModelIndex &hit, bool additive)
    {
        if (!hit.isValid()) return;
        auto start = QModelIndex(selectionAnchor);
        if (!start.isValid()) start = selection->currentIndex().siblingAtColumn(0);
        if (!start.isValid()) start = hit;
        selectionAnchor = start;
        const int first = qMin(start.row(), hit.row());
        const int last = qMax(start.row(), hit.row());
        const QItemSelection span(proxy->index(first, 0), proxy->index(last, 0));
        selection->setCurrentIndex(hit, QItemSelectionModel::NoUpdate);
        selection->select(span, (additive ? QItemSelectionModel::Select : QItemSelectionModel::ClearAndSelect) | QItemSelectionModel::Rows);
    }
    template<class View> void configure(View *view)
    {
        view->setModel(proxy);
        auto *unused = view->selectionModel();
        view->setSelectionModel(selection);
        if (unused && unused != selection && unused->parent() == view) unused->deleteLater();
        view->setFrameShape(QFrame::NoFrame);
        view->setEditTriggers(QAbstractItemView::NoEditTriggers);
        view->setSelectionMode(QAbstractItemView::ExtendedSelection);
        view->setDragEnabled(true);
        view->setDragDropMode(QAbstractItemView::DragOnly);
        view->setDefaultDropAction(Qt::CopyAction);
        view->setAcceptDrops(false);
        view->anchor = [this](const QModelIndex &hit) { selectionAnchor = hit; };
        view->range = [this](const QModelIndex &hit, bool additive) { selectRange(hit, additive); };
        view->installEventFilter(q);
        view->viewport()->installEventFilter(q);
        view->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(view, &QWidget::customContextMenuRequested, q, [this, view](const QPoint &point) {
            const auto hit = view->indexAt(point).siblingAtColumn(0);
            if (hit.isValid()) {
                if (!selection->isSelected(hit)) selection->setCurrentIndex(hit, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                else selection->setCurrentIndex(hit, QItemSelectionModel::NoUpdate);
                selectionAnchor = hit;
            }
            emit q->contextMenuRequested(view->viewport()->mapToGlobal(point));
        });
        QObject::connect(view, &QAbstractItemView::doubleClicked, q, [this](const QModelIndex &hit) {
            const auto url = indexUrl(hit);
            if (url.isValid() && !url.isEmpty()) emit q->activated(url);
        });
        for (auto *bar : {view->verticalScrollBar(), view->horizontalScrollBar()})
            QObject::connect(bar, &QScrollBar::valueChanged, q, [this] { scheduleThumbnails(true); });
    }
    void updateLocation()
    {
        const auto current = selection->currentIndex().siblingAtColumn(0);
        const auto url = indexUrl(current);
        const bool valid = current.isValid() && selection->isSelected(current);
        locationName->setText(valid ? current.data().toString() : q->tr("Select a search result"));
        locationKind->setText(valid ? current.siblingAtColumn(1).data().toString() : QString());
        locationPath->setText(valid ? current.siblingAtColumn(2).data().toString() : QString());
        locationUrl->setText(valid ? url.toDisplayString(QUrl::RemovePassword) : QString());
    }
    void scheduleThumbnails(bool viewportChanged = false)
    {
        // A viewport larger than the memory cache must still settle. Retry
        // evicted successes only after scrolling/geometry/mode changes, never
        // repeatedly evict and regenerate them on a stationary huge screen.
        if (viewportChanged) visibleRequests.clear();
        if (q->isVisible() && !thumbnailTimer.isActive()) thumbnailTimer.start(40);
    }
    void invalidateThumbnails()
    {
        ++generation;
        failures.clear();
        visibleRequests.clear();
        proxy->thumbnails.clear();
        proxy->repaintDecorations();
        scheduleThumbnails();
    }
    void loadVisibleThumbnails()
    {
        if (thumbnailJob || !q->isVisible() || !proxy->sourceModel()) return;
        const auto *view = q->activeView();
        const auto viewport = view->viewport()->rect();
        if (viewport.isEmpty()) return;
        const bool tiles = mode == SearchResults::Icons || mode == SearchResults::Gallery;
        const auto grid = tiles ? static_cast<const QListView *>(view)->gridSize() : QSize();
        const int dx = tiles ? qMax(16, grid.width() / 2) : qMax(1, viewport.width());
        const int dy = tiles ? qMax(16, grid.height() / 2) : qMax(8, (view->fontMetrics().height() + view->iconSize().height() + 8) / 2);
        KFileItemList visible;
        QSet<QUrl> sampled;
        const auto sample = [&](const QPoint &point) {
            const auto hit = view->indexAt(point).siblingAtColumn(0);
            const auto url = indexUrl(hit);
            // Search currently supplies local hits. Remote results retain
            // their source icons; never download arbitrary remote content.
            if (!hit.isValid() || !url.isLocalFile() || url.isEmpty() || sampled.contains(url)
                || failures.contains(url) || visibleRequests.contains(url) || proxy->thumbnails.contains(url)) return;
            sampled.insert(url);
            // Extension-only MIME identification explicitly avoids content
            // reads/stat here. KIO's asynchronous worker owns source checks.
            visible.append(KFileItem(url, KFileItem::SkipMimeTypeFromContent));
        };
        for (int y = 1; y < viewport.height() && visible.size() < 8; y += dy)
            for (int x = 1; x < viewport.width() && visible.size() < 8; x += dx) sample({x, y});
        if (visible.isEmpty()) return; // single-shot scheduling, no idle loop
        const auto plugins = KIO::PreviewJob::defaultPlugins();
        if (plugins.isEmpty()) { failures.unite(sampled); return; }
        const auto revision = generation;
        const auto requested = sampled;
        visibleRequests.unite(requested);
        auto *job = KIO::filePreview(visible, {128, 128}, &plugins);
        job->setScaleType(KIO::PreviewJob::ScaledAndCached);
        job->setIgnoreMaximumSize(false);
        job->setDevicePixelRatio(qBound(1.0, q->devicePixelRatioF(), 2.0));
        thumbnailJob = job;
        // Keep one bounded batch in flight, even through model/mode changes.
        // KF6's native job can have pending transfer timeouts: let it finish,
        // guarded by QObject context and generation, rather than kill/delete
        // it while those timeouts still reference a transfer job.
        QObject::connect(job, &KIO::PreviewJob::gotPreview, q,
            [this, revision, requested](const KFileItem &item, const QPixmap &pixmap) {
                if (revision != generation || !requested.contains(item.url()) || pixmap.isNull()) return;
                const QPixmap bounded = pixmap.width() > 256 || pixmap.height() > 256 ?
                    pixmap.scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation) : pixmap;
                const int cost = qMax(1, int(qint64(bounded.width()) * bounded.height() * 4 / 1024));
                proxy->thumbnails.insert(item.url(), new QPixmap(bounded), cost);
                proxy->repaintUrl(item.url());
            });
        QObject::connect(job, &KJob::result, q, [this, revision, requested, job] {
            if (thumbnailJob == job) thumbnailJob.clear();
            if (revision == generation)
                for (const auto &url : requested) if (!proxy->thumbnails.contains(url)) failures.insert(url);
            scheduleThumbnails();
        });
    }
};

SearchResults::SearchResults(QWidget *parent) : QWidget(parent), d(std::make_unique<Private>(this))
{
    setObjectName(QStringLiteral("finderSearchPresentation"));
    setBackgroundRole(QPalette::Base);
    setAutoFillBackground(true);
    d->proxy = new ResultsProxy(this);
    d->selection = new QItemSelectionModel(d->proxy, this);
    d->stack = new QStackedWidget(this);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(d->stack);

    d->icons = new ResultsView<QListView>(this);
    d->icons->setObjectName(QStringLiteral("finderSearchIcons"));
    d->configure(d->icons);
    d->icons->setViewMode(QListView::IconMode);
    d->icons->setResizeMode(QListView::Adjust);
    d->icons->setMovement(QListView::Static);
    d->icons->setUniformItemSizes(true);
    d->icons->setIconSize({64, 64});
    d->icons->setGridSize({124, 112});
    d->icons->setWordWrap(true);
    d->icons->setTextElideMode(Qt::ElideMiddle);
    d->stack->addWidget(d->icons);

    d->list = new ResultsView<QTreeView>(this);
    d->list->setObjectName(QStringLiteral("finderSearchResults"));
    d->configure(d->list);
    d->list->setRootIsDecorated(false);
    d->list->setItemsExpandable(false);
    d->list->setUniformRowHeights(true);
    d->list->setSelectionBehavior(QAbstractItemView::SelectRows);
    d->list->setAlternatingRowColors(true);
    d->list->setIconSize({20, 20});
    d->list->setSortingEnabled(true);
    d->list->header()->setStretchLastSection(true);
    d->stack->addWidget(d->list);

    auto *columnPage = new QWidget(this);
    auto *columnLayout = new QVBoxLayout(columnPage);
    columnLayout->setContentsMargins(0, 0, 0, 0);
    auto *splitter = new QSplitter(Qt::Horizontal, columnPage);
    d->columns = new ResultsView<QListView>(splitter);
    d->columns->setObjectName(QStringLiteral("finderSearchColumns"));
    d->columns->setAccessibleName(tr("Flat search results with locations"));
    d->configure(d->columns);
    d->columns->setUniformItemSizes(true);
    d->columns->setIconSize({24, 24});
    d->columns->setItemDelegate(new LocationDelegate(d->columns));
    auto *details = new QWidget(splitter);
    details->setObjectName(QStringLiteral("finderSearchLocation"));
    auto *detailsLayout = new QVBoxLayout(details);
    detailsLayout->setContentsMargins(16, 16, 16, 16);
    auto *heading = new QLabel(tr("Result location"), details);
    detailsLayout->addWidget(heading);
    for (auto **label : {&d->locationName, &d->locationKind, &d->locationPath, &d->locationUrl}) {
        *label = new QLabel(details);
        (*label)->setTextFormat(Qt::PlainText);
        (*label)->setWordWrap(true);
        (*label)->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        detailsLayout->addWidget(*label);
    }
    d->locationUrl->setObjectName(QStringLiteral("finderSearchLocationUrl"));
    detailsLayout->addStretch();
    splitter->addWidget(d->columns);
    splitter->addWidget(details);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    columnLayout->addWidget(splitter);
    d->stack->addWidget(columnPage);

    d->gallery = new QWidget(this);
    auto *galleryLayout = new QVBoxLayout(d->gallery);
    galleryLayout->setContentsMargins(0, 0, 0, 0);
    galleryLayout->setSpacing(0);
    d->galleryHost = new QWidget(d->gallery);
    d->galleryHost->setObjectName(QStringLiteral("finderSearchGalleryHost"));
    auto *hostLayout = new QVBoxLayout(d->galleryHost);
    hostLayout->setContentsMargins(0, 0, 0, 0);
    galleryLayout->addWidget(d->galleryHost, 1);
    d->strip = new ResultsView<QListView>(d->gallery);
    d->strip->setObjectName(QStringLiteral("finderSearchGalleryStrip"));
    d->configure(d->strip);
    d->strip->setViewMode(QListView::IconMode);
    d->strip->setFlow(QListView::LeftToRight);
    d->strip->setWrapping(false);
    d->strip->setMovement(QListView::Static);
    d->strip->setResizeMode(QListView::Adjust);
    d->strip->setUniformItemSizes(true);
    d->strip->setIconSize({96, 96});
    d->strip->setGridSize({140, 140});
    d->strip->setFixedHeight(162);
    d->strip->setTextElideMode(Qt::ElideMiddle);
    galleryLayout->addWidget(d->strip);
    d->stack->addWidget(d->gallery);

    d->thumbnailTimer.setSingleShot(true);
    connect(&d->thumbnailTimer, &QTimer::timeout, this, [this] { d->loadVisibleThumbnails(); });
    connect(d->selection, &QItemSelectionModel::selectionChanged, this, [this] { d->updateLocation(); emit selectionChanged(); });
    connect(d->selection, &QItemSelectionModel::currentChanged, this, [this] {
        d->updateLocation();
        // Primary navigation within an existing selection also changes the
        // Gallery/Quick Look item, even when the set of selected URLs stays.
        emit selectionChanged();
    });
    connect(d->proxy, &QAbstractItemModel::layoutChanged, this, [this] { d->scheduleThumbnails(); d->updateLocation(); });
    d->updateLocation();
    d->stack->setCurrentIndex(d->mode);
    d->list->sortByColumn(0, Qt::AscendingOrder);
    setFocusProxy(activeView());
}

SearchResults::~SearchResults()
{
    d->thumbnailTimer.stop();
    if (d->thumbnailJob) d->thumbnailJob->disconnect(this);
    for (const auto &connection : std::as_const(d->sourceConnections)) disconnect(connection);
}
void SearchResults::setModel(QAbstractItemModel *model)
{
    if (model == d->proxy->sourceModel()) return;
    // source connections are deliberately replaced, including destroyed().
    for (const auto &connection : std::as_const(d->sourceConnections)) disconnect(connection);
    d->sourceConnections.clear();
    d->selectionAnchor = QPersistentModelIndex();
    d->proxy->setSourceModel(model);
    d->invalidateThumbnails();
    d->updateLocation();
    if (model) {
        const auto refresh = [this] { d->selectionAnchor = QPersistentModelIndex(); d->invalidateThumbnails(); d->updateLocation(); };
        d->sourceConnections.append(connect(model, &QAbstractItemModel::modelReset, this, refresh));
        d->sourceConnections.append(connect(model, &QAbstractItemModel::rowsRemoved, this, refresh));
        d->sourceConnections.append(connect(model, &QAbstractItemModel::dataChanged, this, [refresh] { refresh(); }));
        d->sourceConnections.append(connect(model, &QAbstractItemModel::rowsInserted, this, [this] { d->scheduleThumbnails(); }));
        d->sourceConnections.append(connect(model, &QObject::destroyed, this, [this] {
            // Some Qt versions clear the proxy's source without notifying a
            // QTreeView to discard its cached row indexes. Reset public view
            // state immediately, before a queued paint can use those indexes.
            d->selectionAnchor = QPersistentModelIndex();
            d->selection->reset();
            for (auto *view : {static_cast<QAbstractItemView *>(d->icons), static_cast<QAbstractItemView *>(d->list),
                              static_cast<QAbstractItemView *>(d->columns), static_cast<QAbstractItemView *>(d->strip)}) view->reset();
            d->invalidateThumbnails();
            d->updateLocation();
            emit selectionChanged();
        }));
    }
    d->list->setColumnWidth(0, 260);
    d->list->setColumnWidth(1, 140);
    emit selectionChanged();
}
QAbstractItemModel *SearchResults::sourceModel() const { return d->proxy->sourceModel(); }
QSortFilterProxyModel *SearchResults::proxyModel() const { return d->proxy; }
void SearchResults::setMode(int mode)
{
    if (mode < Icons || mode > Gallery || mode == d->mode) return;
    const bool focused = activeView()->hasFocus();
    d->mode = mode;
    d->stack->setCurrentIndex(mode);
    setFocusProxy(activeView());
    if (focused) activeView()->setFocus(Qt::OtherFocusReason);
    const auto current = d->selection->currentIndex();
    if (current.isValid()) activeView()->scrollTo(current);
    d->scheduleThumbnails(true);
}
int SearchResults::mode() const { return d->mode; }
void SearchResults::sort(int column, Qt::SortOrder order)
{
    if (column >= 0 && column < d->proxy->columnCount()) d->list->sortByColumn(column, order);
}
QAbstractItemView *SearchResults::activeView() const
{
    switch (d->mode) {
    case Icons: return d->icons;
    case Columns: return d->columns;
    case Gallery: return d->strip;
    default: return d->list;
    }
}
QTreeView *SearchResults::primaryView() const { return d->list; }
QItemSelectionModel *SearchResults::selectionModel() const { return d->selection; }
QList<QUrl> SearchResults::selectedUrls() const
{
    QModelIndexList indexes;
    // QListView can select column 0 alone; QTreeView selects whole rows. Both
    // are represented by the same selection without remapping through source.
    for (const auto &span : d->selection->selection()) {
        if (span.parent().isValid() || span.left() > 0) continue;
        for (int row = span.top(); row <= span.bottom(); ++row) indexes.append(d->proxy->index(row, 0));
    }
    return urlsForIndexes(indexes);
}
void SearchResults::setUrls(const QList<QUrl> &urls, const QUrl &primary)
{
    const QSet<QUrl> wanted(urls.cbegin(), urls.cend());
    QItemSelection selected;
    QModelIndex current;
    for (int row = 0; row < d->proxy->rowCount(); ++row) {
        const auto hit = d->proxy->index(row, 0);
        const auto url = indexUrl(hit);
        if (!wanted.contains(url) || url.isEmpty()) continue;
        selected.select(hit, hit);
        if (!current.isValid() || url == primary) current = hit;
    }
    // Set current before notifying the host so PreviewPane sees the actual
    // primary URL in the selectionChanged callback.
    d->selection->setCurrentIndex(current, QItemSelectionModel::NoUpdate);
    d->selectionAnchor = current;
    d->selection->select(selected, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    d->updateLocation();
    if (current.isValid()) activeView()->scrollTo(current);
}
QMimeData *SearchResults::mimeData() const { return d->proxy->mimeData(d->selection->selectedIndexes()); }
QWidget *SearchResults::galleryHost() const { return d->galleryHost; }
QWidget *SearchResults::host() const { return galleryHost(); }
void SearchResults::refreshThumbnails() { d->invalidateThumbnails(); }
bool SearchResults::eventFilter(QObject *object, QEvent *event)
{
    for (auto *view : {static_cast<QAbstractItemView *>(d->icons), static_cast<QAbstractItemView *>(d->list),
                      static_cast<QAbstractItemView *>(d->columns), static_cast<QAbstractItemView *>(d->strip)}) {
        if (object != view && object != view->viewport()) continue;
        if (event->type() == QEvent::KeyPress && object == view) {
            const auto *key = static_cast<const QKeyEvent *>(event);
            if (key->modifiers() == Qt::NoModifier) {
                if (key->key() == Qt::Key_Space) { emit quickLookRequested(selectedUrls()); return true; }
                if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) { emit renameRequested(); return true; }
            }
        }
        if (event->type() == QEvent::ContextMenu && static_cast<QContextMenuEvent *>(event)->reason() == QContextMenuEvent::Keyboard) {
            const auto hit = d->selection->currentIndex().siblingAtColumn(0);
            const auto rect = view->visualRect(hit);
            emit contextMenuRequested(view->viewport()->mapToGlobal(rect.isValid() ? rect.center() : QPoint(8, 8)));
            return true;
        }
        if (event->type() == QEvent::Resize || event->type() == QEvent::Show || event->type() == QEvent::LayoutRequest)
            d->scheduleThumbnails(true);
        break;
    }
    return QWidget::eventFilter(object, event);
}
void SearchResults::showEvent(QShowEvent *event) { QWidget::showEvent(event); d->scheduleThumbnails(true); }
void SearchResults::hideEvent(QHideEvent *event) { d->thumbnailTimer.stop(); QWidget::hideEvent(event); }
void SearchResults::resizeEvent(QResizeEvent *event) { QWidget::resizeEvent(event); d->scheduleThumbnails(true); }
void SearchResults::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::ScreenChangeInternal || event->type() == QEvent::DevicePixelRatioChange) d->invalidateThumbnails();
}
