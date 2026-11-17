// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#include "places.h"
#include "appearance.h"
#include "tags.h"
#include "settings.h"
#include <Solid/StorageDrive>
#include <Solid/OpticalDrive>

#include <KFilePlacesModel>
#include <KProtocolInfo>
#include <QAbstractItemDelegate>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMap>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPersistentModelIndex>
#include <QScrollBar>
#include <QSettings>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStyleOptionViewItem>
#include <QTimer>
#include <QTreeView>
#include <functional>

namespace {
enum Role {
    HeadingRole = Qt::UserRole + 1,
    KeyRole,
    SourceRole,
    UrlRole,
    GlyphRole,
    TagRole,
    TagColorRole,
    BusyRole,
    EjectRole,
    HiddenRole,
};
constexpr int RowHeight = 32;
constexpr int HeadingHeight = 32;
constexpr int IconSize = 20;

QRect actionRect(const QRect &row, Qt::LayoutDirection direction) {
    // Painting and pointer handling use this exact rectangle. The device
    // action has its own 24px hit target; section headers have none.
    return direction == Qt::RightToLeft ? QRect(row.left() + 4, row.top(), 24, row.height())
                                       : QRect(row.right() - 27, row.top(), 24, row.height());
}

const QMap<QString, QPainterPath> &placeGlyphs() {
    static const QMap<QString, QPainterPath> paths = [] {
        QMap<QString, QPainterPath> all;
        QPainterPath p;
        p.moveTo(2.5, 9); p.lineTo(10, 3); p.lineTo(17.5, 9);
        p.moveTo(4.5, 8); p.lineTo(4.5, 17); p.lineTo(8, 17); p.lineTo(8, 12);
        p.lineTo(12, 12); p.lineTo(12, 17); p.lineTo(15.5, 17); p.lineTo(15.5, 8); all["home"] = p;
        p = {}; p.addRoundedRect(QRectF(2, 3.5, 16, 13), 2, 2);
        p.moveTo(2, 13); p.lineTo(18, 13); all["desktop"] = p;
        p = {}; p.moveTo(5, 2.5); p.lineTo(11.5, 2.5); p.lineTo(15.5, 6.5); p.lineTo(15.5, 17.5);
        p.lineTo(5, 17.5); p.closeSubpath(); p.moveTo(11.5, 2.5); p.lineTo(11.5, 6.5); p.lineTo(15.5, 6.5);
        all["documents"] = p;
        p = {}; p.addEllipse(QRectF(2, 2, 16, 16));
        p.moveTo(10, 5); p.lineTo(10, 14); p.moveTo(6.5, 10.5); p.lineTo(10, 14); p.lineTo(13.5, 10.5); all["downloads"] = p;
        p = {}; p.moveTo(8, 14); p.lineTo(8, 5); p.lineTo(16, 3); p.lineTo(16, 12);
        p.addEllipse(QRectF(3, 13, 5, 4)); p.addEllipse(QRectF(11, 11, 5, 4)); all["music"] = p;
        p = {}; p.addRoundedRect(QRectF(2.5, 3, 15, 14), 1.2, 1.2); p.addEllipse(QRectF(12, 5.5, 2.5, 2.5));
        p.moveTo(3, 14); p.lineTo(7.5, 9); p.lineTo(11, 13); p.lineTo(13.5, 10.5); p.lineTo(17, 14); all["pictures"] = p;
        p = {}; p.addRoundedRect(QRectF(2.5, 4, 15, 12), 1, 1); p.moveTo(6, 4); p.lineTo(6, 16);
        p.moveTo(14, 4); p.lineTo(14, 16); for (int y : {7, 10, 13}) {p.moveTo(2.5, y);p.lineTo(6, y);p.moveTo(14, y);p.lineTo(17.5, y);} all["videos"] = p;
        p = {}; p.moveTo(5, 6); p.lineTo(6, 17); p.lineTo(14, 17); p.lineTo(15, 6);
        p.moveTo(3.5, 5.5); p.lineTo(16.5, 5.5); p.moveTo(7, 5.5); p.lineTo(7, 2.5); p.lineTo(13, 2.5); p.lineTo(13, 5.5);
        p.moveTo(8, 8.5); p.lineTo(8.5, 14); p.moveTo(12, 8.5); p.lineTo(11.5, 14); all["trash"] = p;
        p = {}; p.addEllipse(QRectF(2.5, 2.5, 15, 15)); p.moveTo(10, 5.5); p.lineTo(10, 10); p.lineTo(13.5, 12); all["recent"] = p;
        p = {}; p.addEllipse(QRectF(2.5, 2.5, 15, 15)); p.addEllipse(QRectF(6, 2.5, 8, 15));
        p.moveTo(3, 7.5); p.lineTo(17, 7.5); p.moveTo(3, 12.5); p.lineTo(17, 12.5); all["network"] = p;
        p = {}; p.moveTo(4.5, 5); p.lineTo(15.5, 5); p.lineTo(18, 13); p.lineTo(18, 16.5); p.lineTo(2, 16.5); p.lineTo(2, 13); p.closeSubpath();
        p.moveTo(2, 13); p.lineTo(18, 13); p.moveTo(14.5, 14.8); p.lineTo(16, 14.8); all["device"] = p;
        p = {}; p.moveTo(2.5, 6); p.lineTo(2.5, 3.5); p.lineTo(8, 3.5); p.lineTo(10, 6); p.lineTo(17.5, 6); p.lineTo(17.5, 16.5); p.lineTo(2.5, 16.5); p.closeSubpath(); all["folder"] = p;
        p = {}; p.moveTo(4, 17); p.lineTo(11, 3); p.moveTo(9, 3); p.lineTo(16, 17);
        p.moveTo(2.5, 13); p.lineTo(17.5, 13); all["applications"] = p;
        p = {}; p.addEllipse(QRectF(3, 3, 5, 5)); p.addEllipse(QRectF(12, 4, 4, 4));
        p.moveTo(1, 17); p.cubicTo(1, 9, 10, 9, 10, 17); p.closeSubpath();
        p.moveTo(11, 11); p.cubicTo(15, 9, 19, 11, 19, 17); p.lineTo(13, 17); all["shared"] = p;
        return all;
    }();
    return paths;
}


QString glyphFor(KFilePlacesModel *model, const QModelIndex &index) {
    const QUrl url = model->url(index);
    if (model->isDevice(index)) return QStringLiteral("device");
    if (url.scheme() == "trash") return QStringLiteral("trash");
    if (url.scheme() == "recentlyused" || url.scheme() == "timeline") return QStringLiteral("recent");
    if (url.scheme() == "remote" || url.scheme() == "network" || url.scheme() == "smb") return QStringLiteral("network");
    if (url.isLocalFile()) {
        const QString path = QDir::cleanPath(url.toLocalFile());
        if (path == QDir::homePath()) return QStringLiteral("home");
        for (const auto &place : QList<QPair<QStandardPaths::StandardLocation, QString>>{
                 {QStandardPaths::DesktopLocation, "desktop"}, {QStandardPaths::DocumentsLocation, "documents"},
                 {QStandardPaths::DownloadLocation, "downloads"}, {QStandardPaths::MusicLocation, "music"},
                 {QStandardPaths::PicturesLocation, "pictures"}, {QStandardPaths::MoviesLocation, "videos"}}) {
            if (path == QDir::cleanPath(QStandardPaths::writableLocation(place.first))) return place.second;
        }
        if (path == "/usr/share/applications") return QStringLiteral("applications");
        if (path == "/") return QStringLiteral("device");
    }
    return QStringLiteral("folder");
}

class PlacesDelegate final : public QAbstractItemDelegate {
public:
    explicit PlacesDelegate(QTreeView *view) : QAbstractItemDelegate(view) {}
    int busyAngle = 0;

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const override {
        return QSize(180, index.data(HeadingRole).toBool() ? HeadingHeight : RowHeight);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const bool reverse = option.direction == Qt::RightToLeft;
        if (index.data(HeadingRole).toBool()) {
            QFont font(option.font); font.setPixelSize(11); font.setWeight(QFont::Medium);
            p->setFont(font);
            p->setPen(option.palette.color(QPalette::PlaceholderText));
            const QRect label = option.rect.adjusted(10, 10, -10, 0);
            p->drawText(label, (reverse ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
                        QFontMetrics(font).elidedText(index.data().toString(), Qt::ElideRight, label.width()));
            p->restore();
            return;
        }
        const QRect row = option.rect;
        QPainterPath contour; contour.addRoundedRect(QRectF(row.adjusted(4, 1, -4, -1)), 8, 8);
        if (option.state & QStyle::State_Selected) {
            p->fillPath(contour, FinderAppearance::color(FinderAppearance::SidebarSelection));
        } else if (option.state & QStyle::State_MouseOver) {
            p->fillPath(contour, FinderAppearance::color(FinderAppearance::SidebarHover));
        }
        if (index.data(HiddenRole).toBool() && !FinderAppearance::highContrast()) p->setOpacity(0.6);
        const QRect iconRect(reverse ? row.right() - 29 : row.left() + 10,
                             row.top() + (row.height() - IconSize) / 2, IconSize, IconSize);
        const auto color = index.data(TagColorRole).value<QColor>();
        if (color.isValid()) {
            p->setPen(Qt::NoPen); p->setBrush(color);
            p->drawEllipse(QRectF(iconRect).adjusted(4, 4, -4, -4));
        } else {
            p->save(); p->translate(iconRect.topLeft());
            const QString glyph = index.data(GlyphRole).toString();
            const bool gray = glyph == "trash" || glyph == "device";
            const bool contrastSelection = FinderAppearance::highContrast() && (option.state & QStyle::State_Selected);
            p->setPen(QPen(contrastSelection ? option.palette.color(QPalette::HighlightedText)
                          : gray ? option.palette.color(QPalette::PlaceholderText) : FinderAppearance::color(FinderAppearance::Accent),
                           1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p->setBrush(Qt::NoBrush); p->drawPath(placeGlyphs().value(glyph)); p->restore();
        }
        const bool hasAction = index.data(EjectRole).toBool() || index.data(BusyRole).toBool();
        const int trailing = hasAction ? 32 : 10;
        QRect textRect = row.adjusted(reverse ? trailing : 34, 0, reverse ? -34 : -trailing, 0);
        QFont font(option.font);
        font.setWeight(option.state & QStyle::State_Selected ? QFont::Bold : QFont::Medium);
        p->setFont(font);
        const bool contrastSelection = FinderAppearance::highContrast() && (option.state & QStyle::State_Selected);
        p->setPen(option.palette.color(option.state & QStyle::State_Enabled ? QPalette::Active : QPalette::Disabled,
                                        contrastSelection ? QPalette::HighlightedText : QPalette::Text));
        p->drawText(textRect, (reverse ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
                    QFontMetrics(font).elidedText(index.data().toString(), Qt::ElideRight, textRect.width()));
        if (hasAction) {
            const QPointF center = actionRect(row, option.direction).center();
            p->save(); p->setOpacity(1); p->translate(center);
            p->setPen(QPen(option.palette.color(contrastSelection ? QPalette::HighlightedText : QPalette::Text), 1.25, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p->setBrush(Qt::NoBrush);
            if (index.data(BusyRole).toBool()) {
                p->rotate(busyAngle); p->drawArc(QRectF(-6, -6, 12, 12), 0, 260 * 16);
            } else {
                QPainterPath eject;
                eject.moveTo(-5, 2); eject.lineTo(0, -4); eject.lineTo(5, 2); eject.closeSubpath();
                p->drawPath(eject); p->drawLine(QPointF(-5, 5), QPointF(5, 5));
            }
            p->restore();
        }
        if (option.state & QStyle::State_HasFocus) {
            QColor focus = contrastSelection ? option.palette.color(QPalette::HighlightedText)
                : FinderAppearance::color(FinderAppearance::Focus);
            p->setPen(QPen(focus, FinderAppearance::highContrast() ? 2 : 1)); p->setBrush(Qt::NoBrush); p->drawPath(contour);
        }
        p->restore();
    }
};

// This view never inherits or invokes KFilePlacesView's private event watcher.
// Headers are real nonselectable model rows, so their geometry is safe for
// mouse, keyboard, context menus, eject targets, and drops alike.
class SidebarTree final : public QTreeView {
public:
    explicit SidebarTree(QWidget *parent) : QTreeView(parent) {
        setObjectName(QStringLiteral("finderPlacesPresentation"));
        setAccessibleName(QStringLiteral("Sidebar"));
        setAccessibleDescription(QStringLiteral("Recents, shared locations, favorites, disks, network, trash and file tags"));
        setHeaderHidden(true); setRootIsDecorated(false); setIndentation(0);
        setItemsExpandable(false); setExpandsOnDoubleClick(false); setAllColumnsShowFocus(true);
        setEditTriggers(QAbstractItemView::NoEditTriggers); setSelectionMode(QAbstractItemView::SingleSelection);
        setFrameShape(QFrame::NoFrame); setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded); setMouseTracking(true);
        setAutoFillBackground(false); viewport()->setAutoFillBackground(false);
        viewport()->setAttribute(Qt::WA_OpaquePaintEvent, false);
        setStyleSheet(QStringLiteral(
            "QTreeView { background: transparent; border: 0; }"
            "QScrollBar:vertical { background: transparent; width: 10px; margin: 1px; }"
            "QScrollBar::handle:vertical { background: palette(midlight); border-radius: 4px; min-height: 28px; }"
            "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical { height: 0; }"
            "QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical { background: transparent; }"));
        setDragEnabled(true); setAcceptDrops(true); setDropIndicatorShown(false);
        setDragDropMode(QAbstractItemView::DragDrop);
        m_hoverTimer.setSingleShot(true);
        connect(&m_hoverTimer, &QTimer::timeout, this, [this] {
            if (m_hover.isValid() && activate) activate(m_hover, Qt::NoModifier, Qt::LeftButton);
        });
    }

    std::function<void(QModelIndex, Qt::KeyboardModifiers, Qt::MouseButton)> activate;
    std::function<void(QModelIndex)> eject;
    std::function<void(QModelIndex, QPoint)> menu;
    std::function<bool(QModelIndex, QDropEvent *, bool)> drop;
    std::function<int()> hoverDelay;
    std::function<bool()> dropOnPlace;
    std::function<QMimeData *(QModelIndex)> dragData;

protected:
    void mousePressEvent(QMouseEvent *event) override {
        const QModelIndex index = indexAt(event->position().toPoint());
        m_pressed = QPersistentModelIndex(); m_action = false;
        if (index.isValid() && !index.data(HeadingRole).toBool()) {
            m_pressed = index; m_button = event->button();
            m_action = event->button() == Qt::LeftButton && (index.data(EjectRole).toBool() || index.data(BusyRole).toBool())
                && actionRect(visualRect(index), layoutDirection()).contains(event->position().toPoint());
            if (m_action || event->button() == Qt::MiddleButton) {
                event->accept(); return;
            }
        }
        QTreeView::mousePressEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        const QModelIndex index = indexAt(event->position().toPoint());
        const bool matches = m_pressed.isValid() && m_pressed == index && event->button() == m_button;
        if (matches && m_action) {
            if (actionRect(visualRect(index), layoutDirection()).contains(event->position().toPoint()) && eject)
                eject(index);
            event->accept();
        } else {
            QTreeView::mouseReleaseEvent(event);
            if (matches && (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton) && activate)
                activate(index, event->modifiers(), event->button());
        }
        m_pressed = QPersistentModelIndex(); m_action = false;
    }
    void mouseDoubleClickEvent(QMouseEvent *event) override {
        // One sidebar click already navigates; a double click has no second
        // navigation or tree expansion side effect.
        event->accept();
    }
    void keyPressEvent(QKeyEvent *event) override {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_Space)
            && currentIndex().isValid()) {
            if (activate) activate(currentIndex(), event->modifiers(), Qt::LeftButton);
            event->accept(); return;
        }
        QTreeView::keyPressEvent(event);
    }
    void contextMenuEvent(QContextMenuEvent *event) override {
        const QModelIndex index = event->reason() == QContextMenuEvent::Keyboard ? currentIndex() : indexAt(event->pos());
        if (menu) menu(index, event->globalPos());
        event->accept();
    }
    void startDrag(Qt::DropActions) override {
        if (!dragData || !currentIndex().isValid()) return;
        QMimeData *mime = dragData(currentIndex());
        if (!mime) return;
        m_pressed = QPersistentModelIndex(); m_action = false;
        QDrag drag(this); drag.setMimeData(mime);
        // A bookmark drag is a link/copy, never a move of the actual folder.
        drag.exec(Qt::CopyAction | Qt::LinkAction, Qt::LinkAction);
    }
    void dragEnterEvent(QDragEnterEvent *event) override {
        bool bookmark = false;
        for (const QString &format : event->mimeData()->formats()) bookmark |= format.startsWith("application/x-kfileplacesmodel-");
        if (event->mimeData()->hasUrls() || bookmark) event->acceptProposedAction();
        else event->ignore();
    }
    void dragMoveEvent(QDragMoveEvent *event) override {
        const QModelIndex index = indexAt(event->position().toPoint());
        const bool target = index.isValid() && !index.data(HeadingRole).toBool() && index.data(TagRole).toString().isEmpty();
        if (!target) { stopHover(); event->ignore(); return; }
        const QRect row = visualRect(index);
        bool bookmark = false;
        for (const QString &format : event->mimeData()->formats()) bookmark |= format.startsWith("application/x-kfileplacesmodel-");
        const bool onPlace = dropOnPlace && dropOnPlace() && !bookmark
            && event->position().y() >= row.top() + 4 && event->position().y() <= row.bottom() - 4;
        if (!onPlace) stopHover();
        else if (m_hover != index) {
            stopHover(); m_hover = index;
            const int delay = hoverDelay ? hoverDelay() : 0;
            if (delay > 0) m_hoverTimer.start(delay);
        }
        event->acceptProposedAction();
    }
    void dragLeaveEvent(QDragLeaveEvent *event) override {
        stopHover(); QTreeView::dragLeaveEvent(event);
    }
    void dropEvent(QDropEvent *event) override {
        stopHover();
        const QModelIndex index = indexAt(event->position().toPoint());
        const QRect row = visualRect(index);
        const bool onPlace = dropOnPlace && dropOnPlace() && index.isValid() && !index.data(HeadingRole).toBool()
            && event->position().y() >= row.top() + 4 && event->position().y() <= row.bottom() - 4;
        if (drop && drop(index, event, onPlace)) event->acceptProposedAction();
        else event->ignore();
    }
private:
    void stopHover() { m_hoverTimer.stop(); m_hover = QPersistentModelIndex(); }
    QPersistentModelIndex m_pressed, m_hover;
    Qt::MouseButton m_button = Qt::NoButton;
    bool m_action = false;
    QTimer m_hoverTimer;
};
}

class FinderPlacesView::Private {
public:
    explicit Private(FinderPlacesView *view) : q(view) {
        tree = new SidebarTree(q->viewport());
        rows = new QStandardItemModel(tree);
        tree->setModel(rows);
        delegate = new PlacesDelegate(tree);
        tree->setItemDelegate(delegate);
        rebuildTimer.setSingleShot(true);
        QObject::connect(&rebuildTimer, &QTimer::timeout, q, [this] { rebuild(); });
        busyTimer.setInterval(80);
        QObject::connect(&busyTimer, &QTimer::timeout, q, [this] {
            delegate->busyAngle = (delegate->busyAngle + 24) % 360;
            tree->viewport()->update();
        });
        tree->activate = [this](const QModelIndex &index, Qt::KeyboardModifiers modifiers, Qt::MouseButton button) {
            activate(index, modifiers, button);
        };
        tree->eject = [this](const QModelIndex &index) {
            const auto source = sourceIndex(index);
            if (!model || !source.isValid() || index.data(BusyRole).toBool()) return;
            if (model->isEjectAllowed(source)) model->requestEject(source);
            else if (model->isTeardownAllowed(source)) {
                if (teardown) teardown(source);
                else model->requestTeardown(source);
            }
        };
        tree->menu = [this](const QModelIndex &index, const QPoint &position) { contextMenu(index, position); };
        tree->drop = [this](const QModelIndex &index, QDropEvent *event, bool onPlace) { return handleDrop(index, event, onPlace); };
        tree->hoverDelay = [this] { return q->dragAutoActivationDelay(); };
        tree->dropOnPlace = [this] { return q->isDropOnPlaceEnabled(); };
        tree->dragData = [this](const QModelIndex &index) -> QMimeData * {
            const auto source = sourceIndex(index);
            if (model && source.isValid()) return model->mimeData({source});
            const QUrl url = index.data(UrlRole).toUrl();
            if (url.isEmpty() || !index.data(TagRole).toString().isEmpty()) return nullptr;
            auto *mime = new QMimeData; mime->setUrls({url}); return mime;
        };
        QObject::connect(q, &KFilePlacesView::allPlacesShownChanged, q, [this] { schedule(); });
        QObject::connect(q, &KFilePlacesView::urlChanged, q, [this](const QUrl &url) {
            currentUrl = url; selectedTag.clear(); selectCurrent();
        });
    }
    ~Private() {
        for (const auto &connection : connections) QObject::disconnect(connection);
        delete tree;
    }

    FinderPlacesView *q;
    SidebarTree *tree;
    QStandardItemModel *rows;
    PlacesDelegate *delegate;
    KFilePlacesModel *model = nullptr;
    QList<QMetaObject::Connection> connections;
    QTimer rebuildTimer, busyTimer;
    QUrl currentUrl;
    QString selectedTag, preferredKey;
    bool nativeMenu = false;
    TeardownFunction teardown;
    QPersistentModelIndex pendingMount;
    enum Activation { Current, Tab, ActiveTab, Window };
    Activation pendingActivation = Current;

    QModelIndex sourceIndex(const QModelIndex &index) const { return index.data(SourceRole).value<QPersistentModelIndex>(); }
    void schedule() { if (!rebuildTimer.isActive()) rebuildTimer.start(0); }

    void attach(KFilePlacesModel *source) {
        for (const auto &connection : connections) QObject::disconnect(connection);
        connections.clear(); pendingMount = QPersistentModelIndex(); model = source;
        if (model) {
            const auto update = [this] { schedule(); };
            connections << QObject::connect(model, &QAbstractItemModel::rowsInserted, q, update)
                        << QObject::connect(model, &QAbstractItemModel::rowsRemoved, q, update)
                        << QObject::connect(model, &QAbstractItemModel::rowsMoved, q, update)
                        << QObject::connect(model, &QAbstractItemModel::modelReset, q, update)
                        << QObject::connect(model, &QAbstractItemModel::layoutChanged, q, update)
                        << QObject::connect(model, &QAbstractItemModel::dataChanged, q, update)
                        << QObject::connect(model, &KFilePlacesModel::reloaded, q, update)
                        << QObject::connect(model, &KFilePlacesModel::groupHiddenChanged, q, update)
                        << QObject::connect(model, &KFilePlacesModel::errorMessage, q, &FinderPlacesView::errorMessage)
                        << QObject::connect(model, &KFilePlacesModel::setupDone, q, [this](const QModelIndex &index, bool success) {
                            if (pendingMount != index) return;
                            const Activation activation = pendingActivation;
                            pendingMount = QPersistentModelIndex();
                            if (success) emitActivation(KFilePlacesModel::convertedUrl(model->url(index)), activation);
                        })
                        << QObject::connect(model, &QObject::destroyed, q, [this] { model = nullptr; schedule(); });
            // This also tracks calls made through the existing KFilePlacesView*
            // member in older main.cpp, whose setUrl is nonvirtual.
            connections << QObject::connect(q->selectionModel(), &QItemSelectionModel::selectionChanged, q, [this] {
                if (nativeMenu) return;
                selectedTag.clear();
                const auto selected = q->selectionModel()->selectedIndexes();
                if (!selected.isEmpty() && model) currentUrl = model->url(selected.first());
                selectCurrent(selected.isEmpty());
            });
        }
        rebuild();
    }

    void rebuild() {
        const int scroll = tree->verticalScrollBar()->value();
        rows->clear();
        if (!model) { busyTimer.stop(); return; }
        QList<QModelIndex> available;
        for (int row = 0; row < model->rowCount(); ++row) {
            const QModelIndex index = model->index(row, 0);
            if (q->allPlacesShown() || !model->isHidden(index) || model->url(index).matches(currentUrl, QUrl::StripTrailingSlash))
                available.append(index);
        }
        QList<QModelIndex> used;
        auto find = [&](const QUrl &url) {
            for (const auto &index : available)
                if (model->url(index).matches(url, QUrl::StripTrailingSlash)) { used.append(index); return index; }
            return QModelIndex();
        };
        auto append = [&](const QString &key, const QString &label, const QUrl &url, const QString &glyph, const QModelIndex &source = QModelIndex()) {
            auto *item = new QStandardItem(label);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
            item->setData(key, KeyRole); item->setData(url, UrlRole); item->setData(glyph, GlyphRole);
            item->setData(QVariant::fromValue(QPersistentModelIndex(source)), SourceRole);
            item->setData(url.toDisplayString(QUrl::PreferLocalFile), Qt::ToolTipRole);
            if (source.isValid()) {
                const auto state = model->deviceAccessibility(source);
                item->setData(state == KFilePlacesModel::SetupInProgress || state == KFilePlacesModel::TeardownInProgress, BusyRole);
                item->setData(model->isTeardownOverlayRecommended(source)
                    && (model->isEjectAllowed(source) || model->isTeardownAllowed(source)), EjectRole);
                item->setData(model->isHidden(source), HiddenRole);
            }
            rows->appendRow(item);
            return item;
        };
        auto section = [&](const QString &name) {
            auto *item = new QStandardItem(name);
            item->setData(true, HeadingRole); item->setData("section:" + name, KeyRole);
            item->setFlags(Qt::NoItemFlags); rows->appendRow(item);
        };
        auto defaultPlace = [&](const QString &key, const QString &label, const QUrl &url, const QString &glyph) -> QStandardItem * {
            const auto source = find(url);
            if (!source.isValid() && !q->allPlacesShown()) {
                for (int row = 0; row < model->rowCount(); ++row) {
                    const auto candidate = model->index(row, 0);
                    if (model->url(candidate).matches(url, QUrl::StripTrailingSlash) && model->isHidden(candidate))
                        return nullptr;
                }
            }
            return append(key, label, url, glyph, source);
        };
        QUrl recent("recentlyused:/files");
        if (!KProtocolInfo::isKnownProtocol("recentlyused"))
            recent = QUrl::fromLocalFile(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/RecentDocuments");
        auto *recentItem = defaultPlace("recents", "Recents", recent, "recent");
        if (recentItem && recent.isLocalFile())
            recentItem->setToolTip("Open KDE's recent-document links. The recentlyused KIO worker is unavailable.");
        const QUrl shared(KProtocolInfo::isKnownProtocol("smb") ? "smb:/" : "remote:/");
        auto *sharedItem = append("shared", "Shared", shared, "shared");
        sharedItem->setToolTip(shared.scheme() == "smb" ? "Browse shared folders on the Linux SMB network."
                                                      : "Browse saved Linux network folders. Add a shared folder using Go to Folder.");
        section("Favorites");
        const QUrl applications = QUrl::fromLocalFile("/usr/share/applications");
        defaultPlace("applications", "Applications", applications, "applications");
        for (const auto &place : QList<QPair<QStandardPaths::StandardLocation, QString>>{
                 {QStandardPaths::DesktopLocation, "Desktop"}, {QStandardPaths::MoviesLocation, "Movies"},
                 {QStandardPaths::PicturesLocation, "Pictures"}, {QStandardPaths::MusicLocation, "Music"},
                 {QStandardPaths::DocumentsLocation, "Documents"}, {QStandardPaths::DownloadLocation, "Downloads"},
                 {QStandardPaths::HomeLocation, "Home"}}) {
            const QUrl url = QUrl::fromLocalFile(QStandardPaths::writableLocation(place.first));
            defaultPlace(place.second == "Movies" ? "videos" : place.second.toLower(), place.second, url,
                         place.second == "Movies" ? "videos" : place.second.toLower());
        }
        // Keep real local bookmarks and useful saved queries, in source order.
        // Presentation defaults do not reorder or write KDE's shared bookmarks.
        for (const auto &index : available) {
            if (used.contains(index) || model->isDevice(index)) continue;
            const auto group = model->groupType(index);
            const QUrl url = model->url(index);
            if (url.scheme() == "trash" || group == KFilePlacesModel::RemoteType || group == KFilePlacesModel::TagsType) continue;
            if (url.scheme() == "recentlyused" && url.path() == "/files") continue;
            if (url.isLocalFile() && url.toLocalFile() == "/") continue;
            append("place:" + url.toString(), model->text(index), url, glyphFor(model, index), index);
            used.append(index);
        }
        section("Locations");
        auto hardwareKey = [](Solid::Device device) {
            for (int depth = 0; device.isValid() && depth < 16; ++depth) {
                if (device.is<Solid::OpticalDrive>()) return QStringLiteral("opticalMedia");
                if (auto *drive = device.as<Solid::StorageDrive>()) {
                    if (drive->driveType() == Solid::StorageDrive::CdromDrive) return QStringLiteral("opticalMedia");
                    return drive->isRemovable() || drive->isHotpluggable() ? QStringLiteral("externalDisks") : QStringLiteral("internalDisks");
                }
                device = device.parent();
            }
            return QStringLiteral("externalDisks");
        };
        bool hasRootDisk = false;
        for (const auto &index : available) {
            if (!model->isDevice(index)) continue;
            if (!FinderPreferences::sidebarItemVisible(hardwareKey(model->deviceForIndex(index)))) continue;
            const QUrl url = model->url(index);
            hasRootDisk |= url.isLocalFile() && url.toLocalFile() == "/";
            append("device:" + model->deviceForIndex(index).udi(), model->text(index), url, "device", index);
            used.append(index);
        }
        if (!hasRootDisk && FinderPreferences::sidebarItemVisible("internalDisks")) {
            const QUrl root = QUrl::fromLocalFile("/");
            defaultPlace("filesystem", "Linux Filesystem", root, "device");
        }
        const QUrl network("remote:/");
        defaultPlace("network", "Network", network, "network");
        for (const auto &index : available) {
            if (used.contains(index) || model->groupType(index) != KFilePlacesModel::RemoteType || model->url(index) == shared) continue;
            if (!FinderPreferences::sidebarItemVisible("connectedServers")) continue;
            append("remote:" + model->url(index).toString(), model->text(index), model->url(index), glyphFor(model, index), index);
            used.append(index);
        }
        const QUrl trash("trash:/");
        defaultPlace("trash", "Trash", trash, "trash");
        section("Tags");
        QStringList discovered;
        for (const auto &index : available)
            if (model->groupType(index) == KFilePlacesModel::TagsType && !model->url(index).path().isEmpty() && model->url(index).path() != "/")
                discovered << model->text(index);
        FinderPreferences::registerObservedTags(discovered);
        for (const auto &tag : FinderPreferences::tagRegistry()) {
            auto *item = append("tag:" + tag.id, tag.name, {}, {});
            item->setData(tag.name, TagRole); item->setData(tag.color, TagColorRole);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            item->setToolTip("Show local files tagged " + tag.name + ".");
        }
        const auto hiddenKeys = QSettings().value("Sidebar/hiddenKeys").toStringList();
        const auto hiddenTags = QSettings().value("Sidebar/hiddenTags").toStringList();
        for (int row = rows->rowCount() - 1; row >= 0; --row) {
            auto *item = rows->item(row);
            const auto key = item->data(KeyRole).toString();
            const auto url = item->data(UrlRole).toUrl();
            const auto tag = item->data(TagRole).toString();
            bool hide = !FinderPreferences::sidebarItemVisible(key) || (!tag.isEmpty() && hiddenTags.contains(tag));
            if (key == "filesystem") hide |= !FinderPreferences::sidebarItemVisible("internalDisks");
            for (const auto &entry : QList<QPair<QString, QStandardPaths::StandardLocation>>{
                {"home", QStandardPaths::HomeLocation}, {"music", QStandardPaths::MusicLocation},
                {"pictures", QStandardPaths::PicturesLocation}, {"videos", QStandardPaths::MoviesLocation}})
                if (url == QUrl::fromLocalFile(QStandardPaths::writableLocation(entry.second)))
                    hide |= hiddenKeys.contains(entry.first);
            if (hide) rows->removeRow(row);
        }
        bool busy = false;
        for (int row = 0; row < rows->rowCount(); ++row) busy |= rows->index(row, 0).data(BusyRole).toBool();
        if (busy && !busyTimer.isActive()) busyTimer.start();
        if (!busy) busyTimer.stop();
        selectCurrent();
        tree->verticalScrollBar()->setValue(scroll);
    }

    void selectCurrent(bool clear = false) {
        QModelIndex selected;
        if (!clear) {
            for (int row = 0; row < rows->rowCount(); ++row) {
                const QModelIndex index = rows->index(row, 0);
                const QString tag = index.data(TagRole).toString();
                const bool matches = !selectedTag.isEmpty() ? tag == selectedTag
                    : tag.isEmpty() && !currentUrl.isEmpty() && index.data(UrlRole).toUrl().matches(currentUrl, QUrl::StripTrailingSlash);
                if (matches) {
                    selected = index;
                    if (index.data(KeyRole).toString() == preferredKey) break;
                }
            }
        }
        tree->selectionModel()->setCurrentIndex(selected, QItemSelectionModel::ClearAndSelect);
    }

    Activation activationFor(Qt::KeyboardModifiers modifiers, Qt::MouseButton button) const {
        const bool newTab = button == Qt::MiddleButton || modifiers.testFlag(Qt::ControlModifier) || modifiers.testFlag(Qt::MetaModifier);
        if (newTab) return modifiers.testFlag(Qt::ShiftModifier) ? ActiveTab : Tab;
        return modifiers.testFlag(Qt::ShiftModifier) ? Window : Current;
    }
    void emitActivation(const QUrl &url, Activation activation) {
        if (url.isEmpty()) return;
        if (activation == Current) {
            currentUrl = url; selectedTag.clear();
            q->KFilePlacesView::setUrl(url); selectCurrent();
            emit q->urlChanged(url); emit q->placeActivated(url);
        } else if (activation == Tab) emit q->tabRequested(url);
        else if (activation == ActiveTab) emit q->activeTabRequested(url);
        else emit q->newWindowRequested(url);
    }
    void activate(const QModelIndex &index, Qt::KeyboardModifiers modifiers, Qt::MouseButton button) {
        if (!index.isValid() || index.data(HeadingRole).toBool() || index.data(BusyRole).toBool()) return;
        preferredKey = index.data(KeyRole).toString();
        const QString tag = index.data(TagRole).toString();
        if (!tag.isEmpty()) {
            selectedTag = tag; selectCurrent(); emit q->tagActivated(tag); return;
        }
        const Activation activation = activationFor(modifiers, button);
        const QModelIndex source = sourceIndex(index);
        if (source.isValid() && model->setupNeeded(source)) {
            pendingMount = source; pendingActivation = activation;
            model->requestSetup(source); return;
        }
        emitActivation(KFilePlacesModel::convertedUrl(index.data(UrlRole).toUrl()), activation);
    }

    void contextMenu(const QModelIndex &index, const QPoint &global) {
        const QString tag = index.data(TagRole).toString();
        if (!tag.isEmpty()) {
            QMenu menu(tree); menu.addAction("Show Files Tagged " + tag, q, [this, tag] {
                selectedTag = tag; selectCurrent(); emit q->tagActivated(tag);
            });
            menu.exec(global); return;
        }
        const QModelIndex source = sourceIndex(index);
        if (source.isValid()) {
            // Native bookmark editing, hide/show, device actions, Empty Trash
            // confirmation and properties remain in KDE. Map only the source
            // row and use the actual visible pointer position for menu placement.
            nativeMenu = true;
            q->selectionModel()->setCurrentIndex(source, QItemSelectionModel::ClearAndSelect);
            q->scrollTo(source); q->doItemsLayout();
            const QRect nativeRow = q->visualRect(source);
            QContextMenuEvent event(QContextMenuEvent::Mouse,
                QPoint(nativeRow.center().x(), nativeRow.bottom() - 4), global);
            q->KFilePlacesView::contextMenuEvent(&event);
            nativeMenu = false;
            selectCurrent();
            schedule(); return;
        }
        QMenu menu(tree);
        if (index.isValid() && !index.data(HeadingRole).toBool()) {
            const QUrl url = index.data(UrlRole).toUrl();
            menu.addAction("Open", q, [this, url] { emitActivation(url, Current); });
            menu.addAction("Open in New Tab", q, [this, url] { emitActivation(url, Tab); });
            menu.addAction("Open in New Window", q, [this, url] { emitActivation(url, Window); });
            if (!url.isEmpty()) menu.addAction("Add to Favorites", q, [this, index, url] {
                model->addPlace(index.data().toString(), url);
            });
        } else {
            QAction *showAll = menu.addAction("Show Hidden Places");
            showAll->setCheckable(true); showAll->setChecked(q->allPlacesShown());
            QObject::connect(showAll, &QAction::toggled, q, &KFilePlacesView::setShowAll);
        }
        emit q->contextMenuAboutToShow(source, &menu);
        menu.exec(global);
    }

    bool handleDrop(const QModelIndex &index, QDropEvent *event, bool onPlace) {
        if (!model || !index.data(TagRole).toString().isEmpty()) return false;
        const QModelIndex source = sourceIndex(index);
        // KFilePlacesModel owns its private bookmark MIME type. Let its public
        // drop API recognize bookmark reordering before treating external URLs
        // as a file-operation drop.
        bool bookmarkDrag = false;
        for (const QString &format : model->mimeTypes())
            if (format != "text/uri-list") bookmarkDrag |= event->mimeData()->hasFormat(format);
        if (bookmarkDrag || !onPlace) {
            const int destination = source.isValid() ? source.row() + (event->position().y() > tree->visualRect(index).center().y() ? 1 : 0) : model->rowCount();
            return model->dropMimeData(event->mimeData(), event->proposedAction(), destination, 0, {});
        }
        const QUrl url = index.data(UrlRole).toUrl();
        if (!event->mimeData()->hasUrls()) return false;
        if (source.isValid()) {
            // The original wrapper owns deferred drops: it copies the event,
            // requests a mount, then emits urlsDropped only on successful setup.
            // It also retains KIO's original error/conflict/Undo flow.
            q->scrollTo(source); q->doItemsLayout();
            const QRect nativeRow = q->visualRect(source);
            QMimeData mime;
            for (const QString &format : event->mimeData()->formats())
                mime.setData(format, event->mimeData()->data(format));
            QDropEvent nativeEvent(QPointF(nativeRow.center().x(), nativeRow.bottom() - 16),
                event->possibleActions(), &mime, event->buttons(), event->modifiers());
            nativeEvent.setDropAction(event->dropAction());
            q->KFilePlacesView::dropEvent(&nativeEvent);
            return nativeEvent.isAccepted();
        }
        if (url.isEmpty()) return false;
        emit q->urlsDropped(url, event, tree);
        return event->isAccepted();
    }
};

FinderPlacesView::FinderPlacesView(QWidget *parent) : KFilePlacesView(parent), d(std::make_unique<Private>(this)) {
    setObjectName(QStringLiteral("finderPlaces"));
    setAccessibleName(QStringLiteral("Sidebar"));
    setAccessibleDescription(d->tree->accessibleDescription());
    setAutoResizeItemsEnabled(false); setIconSize(QSize(IconSize, IconSize));
    setFrameShape(QFrame::NoFrame);
    // Only the presentation scrolls. The backing view remains fully laid out
    // for public native context-menu APIs, covered by the transparent tree.
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setAutoFillBackground(false); viewport()->setAutoFillBackground(false);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent, false);
    setFocusProxy(d->tree);
}
FinderPlacesView::~FinderPlacesView() = default;

void FinderPlacesView::refreshPreferences() { d->schedule(); }
void FinderPlacesView::setModel(QAbstractItemModel *model) {
    auto *places = qobject_cast<KFilePlacesModel *>(model);
    if (!places) { emit errorMessage("The sidebar requires a native KFilePlacesModel."); return; }
    KFilePlacesView::setModel(model); d->attach(places);
}
QTreeView *FinderPlacesView::presentationView() const { return d->tree; }
void FinderPlacesView::setTeardownFunction(TeardownFunction teardown) {
    d->teardown = teardown; KFilePlacesView::setTeardownFunction(std::move(teardown));
}
void FinderPlacesView::setUrl(const QUrl &url) {
    d->currentUrl = url; d->selectedTag.clear();
    KFilePlacesView::setUrl(url); d->selectCurrent();
}
void FinderPlacesView::setTag(const QString &tag) {
    d->selectedTag = tag; d->selectCurrent();
}
void FinderPlacesView::resizeEvent(QResizeEvent *event) {
    KFilePlacesView::resizeEvent(event);
    d->tree->setGeometry(viewport()->rect());
}
void FinderPlacesView::scrollContentsBy(int dx, int dy) {
    // Native context menus/drop routing scroll their source row into view.
    // QWidget::scroll also moves child widgets: restore the presentation to
    // the viewport rather than letting a backing scroll offset move it.
    QListView::scrollContentsBy(dx, dy);
    if (d) d->tree->setGeometry(viewport()->rect());
}
void FinderPlacesView::paintEvent(QPaintEvent *) {
    // The native delegate's capacity/group drawing is deliberately not painted
    // behind this transparent presentation; the parent supplies the material.
}
void FinderPlacesView::showEvent(QShowEvent *event) {
    // QWidget show handling avoids native capacity polling for covered rows.
    // Device animation in the presentation runs only during an actual request.
    QListView::showEvent(event);
    d->tree->setGeometry(viewport()->rect()); d->tree->show();
}
