// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#include "places.h"

#include <KFilePlacesModel>
#include <QAbstractItemDelegate>
#include <QApplication>
#include <QDir>
#include <QMap>
#include <QPainter>
#include <QPainterPath>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleOptionViewItem>

namespace {
const QMap<QString, QPainterPath> &placeGlyphs() {
    static const QMap<QString, QPainterPath> paths = [] {
        QMap<QString, QPainterPath> all;
        QPainterPath p;
        p.moveTo(2.5, 9); p.lineTo(10, 3); p.lineTo(17.5, 9);
        p.moveTo(4.5, 8); p.lineTo(4.5, 17); p.lineTo(8, 17); p.lineTo(8, 12);
        p.lineTo(12, 12); p.lineTo(12, 17); p.lineTo(15.5, 17); p.lineTo(15.5, 8); all["home"] = p;
        p = {}; p.addRoundedRect(QRectF(2, 3.5, 16, 11), 1.4, 1.4);
        p.moveTo(10, 14.5); p.lineTo(10, 17); p.moveTo(6.5, 17); p.lineTo(13.5, 17); all["desktop"] = p;
        p = {}; p.moveTo(5, 2.5); p.lineTo(11.5, 2.5); p.lineTo(15.5, 6.5); p.lineTo(15.5, 17.5);
        p.lineTo(5, 17.5); p.closeSubpath(); p.moveTo(11.5, 2.5); p.lineTo(11.5, 6.5); p.lineTo(15.5, 6.5);
        p.moveTo(7.5, 10); p.lineTo(13, 10); p.moveTo(7.5, 13); p.lineTo(13, 13); all["documents"] = p;
        p = {}; p.moveTo(10, 2.5); p.lineTo(10, 13); p.moveTo(6, 9); p.lineTo(10, 13); p.lineTo(14, 9);
        p.moveTo(3, 13.5); p.lineTo(3, 17.5); p.lineTo(17, 17.5); p.lineTo(17, 13.5); all["downloads"] = p;
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
        p = {}; p.moveTo(4, 16.5); p.lineTo(10, 3); p.lineTo(16, 16.5); p.moveTo(6.5, 11.5); p.lineTo(13.5, 11.5); all["applications"] = p;
        return all;
    }();
    return paths;
}

QString glyphFor(const KFilePlacesModel *model, const QModelIndex &index) {
    const QUrl url = model->url(index);
    if (model->isDevice(index)) return QStringLiteral("device");
    if (url.scheme() == "trash") return QStringLiteral("trash");
    if (url.scheme() == "recentlyused" || url.scheme() == "timeline") return QStringLiteral("recent");
    if (url.scheme() == "remote" || url.scheme() == "network") return QStringLiteral("network");
    if (url.isLocalFile()) {
        const QString path = QDir::cleanPath(url.toLocalFile());
        if (path == QDir::homePath()) return QStringLiteral("home");
        for (const auto &place : QList<QPair<QStandardPaths::StandardLocation, QString>>{
                 {QStandardPaths::DesktopLocation, "desktop"}, {QStandardPaths::DocumentsLocation, "documents"},
                 {QStandardPaths::DownloadLocation, "downloads"}, {QStandardPaths::MusicLocation, "music"},
                 {QStandardPaths::PicturesLocation, "pictures"}, {QStandardPaths::MoviesLocation, "videos"}}) {
            if (path == QDir::cleanPath(QStandardPaths::writableLocation(place.first))) return place.second;
        }
        if (path == "/usr/share/applications" || path == "/Applications") return QStringLiteral("applications");
        if (model->groupType(index) == KFilePlacesModel::PlacesType) return QStringLiteral("folder");
    }
    return {};
}

class PlacesDelegate : public QAbstractItemDelegate {
public:
    PlacesDelegate(FinderPlacesView *view, QAbstractItemDelegate *native)
        : QAbstractItemDelegate(view), m_view(view), m_native(native) {}

    bool hasHeader(const QModelIndex &index) const {
        if (!index.isValid() || m_view->isRowHidden(index.row())) return false;
        const auto group = index.data(KFilePlacesModel::GroupRole).toString();
        for (int row = index.row() - 1; row >= 0; --row) {
            if (!m_view->isRowHidden(row))
                return group != index.model()->index(row, 0).data(KFilePlacesModel::GroupRole).toString();
        }
        return !group.isEmpty();
    }

    int headerHeight(const QModelIndex &index) const {
        // The base view's private watcher uses its original delegate for
        // header/eject hit testing. Match KF6 6.24's public view geometry here.
        return hasHeader(index) ? m_view->fontMetrics().height() + 12 : 0;
    }

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const override {
        return QSize(m_view->viewport()->width(), 32 + headerHeight(index));
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const auto *model = qobject_cast<const KFilePlacesModel *>(index.model());
        if (!model) return;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const bool reverse = option.direction == Qt::RightToLeft;
        const int header = headerHeight(index);
        if (header) {
            QFont font(option.font); font.setPointSizeF(11); font.setWeight(QFont::DemiBold);
            painter->setFont(font);
            QColor text = option.palette.color(QPalette::WindowText); text.setAlphaF(0.55);
            painter->setPen(text);
            QString group = index.data(KFilePlacesModel::GroupRole).toString();
            if (model->groupType(index) == KFilePlacesModel::PlacesType) group = QStringLiteral("Favorites");
            if (model->isGroupHidden(index)) group += QStringLiteral(" (hidden)");
            QRect label = option.rect.adjusted(10, 0, -10, 0);
            label.setHeight(header - 5);
            painter->drawText(label, (reverse ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignBottom,
                              QFontMetrics(font).elidedText(group, Qt::ElideRight, label.width()));
        }
        QRect row = option.rect;
        row.setTop(row.top() + header);
        QRectF selection = row.adjusted(4, 1, -4, -1);
        QPainterPath contour; contour.addRoundedRect(selection, 8, 8);
        if (option.state & QStyle::State_Selected) {
            QColor fill = option.palette.color(QPalette::WindowText); fill.setAlphaF(0.11);
            painter->fillPath(contour, fill);
        } else if (option.state & QStyle::State_MouseOver) {
            QColor fill = option.palette.color(QPalette::WindowText); fill.setAlphaF(0.045);
            painter->fillPath(contour, fill);
        }
        if (model->isHidden(index)) painter->setOpacity(0.6);
        const int leading = reverse ? row.right() - 29 : row.left() + 10;
        const QRect iconRect(leading, row.top() + (row.height() - 20) / 2, 20, 20);
        const QString glyph = glyphFor(model, index);
        if (glyph.isEmpty()) model->icon(index).paint(painter, iconRect, Qt::AlignCenter,
                                option.state & QStyle::State_Enabled ? QIcon::Normal : QIcon::Disabled);
        else {
            painter->save(); painter->translate(iconRect.topLeft());
            painter->setPen(QPen(glyph == "trash" || glyph == "device" ? QColor("#d2d2d7") : QColor("#529dff"),
                                1.35, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter->setBrush(Qt::NoBrush); painter->drawPath(placeGlyphs().value(glyph)); painter->restore();
        }

        // Reserve the native eject/busy region even on fixed devices. Keeping
        // the same trailing24px as its watcher prevents decorative hit targets.
        const int actionWidth = 8 + QApplication::style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, m_view);
        QRect textRect = row.adjusted(reverse ? actionWidth : 34, 0, reverse ? -34 : -actionWidth, 0);
        painter->setFont(option.font);
        painter->setPen(option.palette.color(option.state & QStyle::State_Enabled ? QPalette::Active : QPalette::Disabled,
                                            QPalette::Text));
        painter->drawText(textRect, (reverse ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
                          option.fontMetrics.elidedText(model->text(index), Qt::ElideRight, textRect.width()));

        // Let the original delegate render real eject/mount/trash progress.
        // Its view remains alive and receives native device animation signals.
        painter->save(); painter->setOpacity(1);
        QRect actionRect = row;
        if (reverse) actionRect.setWidth(actionWidth);
        else actionRect.setLeft(row.right() - actionWidth + 1);
        painter->setClipRect(actionRect);
        painter->setClipPath(contour, Qt::IntersectClip);
        QStyleOptionViewItem nativeOption(option);
        nativeOption.state &= ~(QStyle::State_Selected | QStyle::State_MouseOver | QStyle::State_HasFocus);
        const auto state = model->deviceAccessibility(index);
        if (model->isTeardownOverlayRecommended(index) || state == KFilePlacesModel::SetupInProgress ||
            state == KFilePlacesModel::TeardownInProgress || model->url(index).scheme() == "trash")
            m_native->paint(painter, nativeOption, index);
        painter->restore();
        if (option.state & QStyle::State_HasFocus) {
            QColor focus = option.palette.color(QPalette::Highlight); focus.setAlphaF(0.8);
            painter->setPen(QPen(focus, 1)); painter->drawPath(contour);
        }
        painter->restore();
    }

private:
    FinderPlacesView *m_view;
    QAbstractItemDelegate *m_native;
};
}

FinderPlacesView::FinderPlacesView(QWidget *parent) : KFilePlacesView(parent) {
    setObjectName(QStringLiteral("finderPlaces"));
    setAccessibleName(QStringLiteral("Sidebar"));
    setAccessibleDescription(QStringLiteral("Favorites, network locations, mounted devices, and tags"));
    setAutoResizeItemsEnabled(false);
    setIconSize(QSize(20, 20));
    setSpacing(0);
    setUniformItemSizes(false);
    setTextElideMode(Qt::ElideRight);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    viewport()->setAutoFillBackground(false);
    // The native event watcher casts itemDelegate() to its private type during
    // pointer input. Keep that default installed; Qt's per-column override
    // supplies our presentation without replacing its native input delegate.
    setItemDelegateForColumn(0, new PlacesDelegate(this, itemDelegate()));
}
