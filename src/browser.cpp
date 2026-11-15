// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
// Public Qt/KIO views and jobs; no private Dolphin ABI or shell file mutations.
#include "browser.h"
#include "tags.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QColumnView>
#include <QCollator>
#include <QDate>
#include <QDir>
#include <QDropEvent>
#include <QFileInfo>
#include <QHeaderView>
#include <QHash>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QListView>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QProcess>
#include <QScrollBar>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QThread>
#include <QTimer>
#include <QTreeView>
#include <QTextEdit>
#include <QTextLayout>
#include <QVBoxLayout>
#include <KDirModel>
#include <KDirLister>
#include <KDirSortFilterProxyModel>
#include <KCategorizedView>
#include <KCategoryDrawer>
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
#include <KIO/PreviewJob>

#include <functional>
#include <algorithm>

namespace {
using DropHandler = std::function<void(QDropEvent *, const QModelIndex &)>;
constexpr int TagsRole = Qt::UserRole + 131;
constexpr int SectionStartRole = Qt::UserRole + 132;
constexpr int SectionHeight = 30;

// Files remain real KDirModel rows. Categorization changes their order and
// supplies section metadata, without inserting selectable dummy file indexes.
class FinderProxy : public KDirSortFilterProxyModel {
public:
    explicit FinderProxy(QObject *parent) : KDirSortFilterProxyModel(parent) {
        collator.setNumericMode(true);
        collator.setCaseSensitivity(Qt::CaseInsensitive);
        tagTimer.setSingleShot(true);
        tagTimer.setInterval(40);
        QObject::connect(&tagTimer, &QTimer::timeout, this, [this] { readTags(); });
    }
    Browser::Group grouping = Browser::GroupNone;

    void setGroup(Browser::Group value) {
        grouping = value;
        setCategorizedModel(value != Browser::GroupNone);
        invalidate();
    }
    void watchSource() {
        QObject::connect(sourceModel(), &QAbstractItemModel::rowsInserted, this, [this] { tagTimer.start(); });
        QObject::connect(sourceModel(), &QAbstractItemModel::modelReset, this, [this] { refreshTags(); });
        // DecorationRole updates can omit the roles list in KDirModel. Never
        // treat thumbnail delivery as a reason to reread filesystem metadata.
        // The host calls refreshTags after its real tag-writing job finishes.
    }
    void refreshTags() {
        ++tagRevision;
        tagCache.clear();
        tagErrors.clear();
        if (grouping == Browser::GroupTags) invalidate();
        tagTimer.start();
    }
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override {
        if (role == TagsRole) return tagCache.value(index.data(KDirModel::FileItemRole).value<KFileItem>().url());
        if (role == CategoryDisplayRole || role == CategorySortRole) {
            const auto info = category(mapToSource(index));
            return role == CategoryDisplayRole ? info.second : info.first;
        }
        if (role == SectionStartRole) {
            if (grouping == Browser::GroupNone || index.parent().isValid()) return false;
            return index.row() == 0 || category(mapToSource(index)).first !=
                category(mapToSource(index.sibling(index.row() - 1, 0))).first;
        }
        return KDirSortFilterProxyModel::data(index, role);
    }
protected:
    int compareCategories(const QModelIndex &left, const QModelIndex &right) const override {
        return collator.compare(category(left).first, category(right).first);
    }
private:
    QCollator collator;
    QHash<QUrl, QStringList> tagCache;
    QHash<QUrl, QString> tagErrors;
    QTimer tagTimer;
    bool readingTags = false;
    quint64 tagRevision = 0;

    QPair<QString, QString> category(const QModelIndex &source) const {
        const auto item = source.data(KDirModel::FileItemRole).value<KFileItem>();
        switch (grouping) {
        case Browser::GroupName: {
            const auto first = item.text().trimmed().left(1).toUpper();
            return {first.isEmpty() || !first.at(0).isLetter() ? QStringLiteral("0") : first,
                    first.isEmpty() || !first.at(0).isLetter() ? QStringLiteral("#") : first};
        }
        case Browser::GroupKind: {
            const auto mime = item.mimetype();
            if (item.isDir()) return {"0", "Folders"};
            if (mime.startsWith("image/")) return {"1", "Images"};
            if (mime == "application/pdf") return {"2", "PDF Documents"};
            if (mime.startsWith("text/") || mime.contains("document") || mime.contains("spreadsheet") || mime.contains("presentation"))
                return {"3", "Documents"};
            if (mime.startsWith("audio/")) return {"4", "Music"};
            if (mime.startsWith("video/")) return {"5", "Movies"};
            if (mime.contains("zip") || mime.contains("tar") || mime.contains("compressed") || mime.contains("archive")) return {"6", "Archives"};
            if (mime.contains("executable") || mime == "application/x-desktop") return {"7", "Applications"};
            const QString kind = source.siblingAtColumn(KDirModel::Type).data().toString();
            return {"8" + kind, kind.isEmpty() ? QStringLiteral("Other") : kind};
        }
        case Browser::GroupDateModified: {
            const QDate date = item.time(KFileItem::ModificationTime).date();
            const QDate today = QDate::currentDate();
            if (!date.isValid()) return {"9", "Unknown Date"};
            if (date >= today) return {"0", "Today"};
            if (date == today.addDays(-1)) return {"1", "Yesterday"};
            if (date >= today.addDays(1 - today.dayOfWeek())) return {"2", "This Week"};
            if (date >= today.addDays(1 - today.dayOfWeek() - 7)) return {"3", "Last Week"};
            if (date >= QDate(today.year(), today.month(), 1)) return {"4", "This Month"};
            if (date >= QDate(today.year(), today.month(), 1).addMonths(-1)) return {"5", "Last Month"};
            if (date.year() == today.year()) return {"6", "Earlier This Year"};
            return {"7", "Earlier"};
        }
        case Browser::GroupSize: {
            if (item.isDir()) return {"0", "Folders"};
            const auto size = item.size();
            if (size == 0) return {"1", "Zero Bytes"};
            if (size < 1024 * 1024) return {"2", "Less than 1 MB"};
            if (size < 100 * 1024 * 1024) return {"3", "1–100 MB"};
            if (size < 1024LL * 1024 * 1024) return {"4", "100 MB–1 GB"};
            return {"5", "1 GB or Larger"};
        }
        case Browser::GroupTags: {
            if (!item.url().isLocalFile() || item.isLink() || tagErrors.contains(item.url()))
                return {"3", "Tags Unavailable"};
            if (!tagCache.contains(item.url())) return {"2", "Reading Tags…"};
            QStringList tags = tagCache.value(item.url());
            if (tags.isEmpty()) return {"1", "No Tags"};
            // A file has one real index, including when it has several tags.
            // Group by the canonical full tag set rather than duplicate rows.
            std::sort(tags.begin(), tags.end(), [this](const QString &a, const QString &b) { return collator.compare(a, b) < 0; });
            const QString names = tags.join(", ");
            return {"0" + names, names};
        }
        default: return {};
        }
    }
    void readTags() {
        if (readingTags) return; // The finishing worker schedules the next scan.
        QList<QUrl> urls;
        for (int row = 0; row < sourceModel()->rowCount(); ++row) {
            const auto item = sourceModel()->index(row, 0).data(KDirModel::FileItemRole).value<KFileItem>();
            if (!item.isNull() && item.url().isLocalFile() && !tagCache.contains(item.url())) urls.append(item.url());
            if (urls.size() >= 128) break;
        }
        if (urls.isEmpty()) return;
        readingTags = true;
        const auto revision = tagRevision;
        auto result = std::make_shared<QHash<QUrl, QStringList>>();
        auto errors = std::make_shared<QHash<QUrl, QString>>();
        auto *worker = QThread::create([urls, result, errors] {
            for (const auto &url : urls) {
                QString error;
                result->insert(url, FileTags::tags(url, &error));
                if (!error.isEmpty()) errors->insert(url, error);
            }
        });
        QObject::connect(worker, &QThread::finished, this, [this, revision, result, errors] {
            readingTags = false;
            if (revision == tagRevision) {
                for (auto it = result->cbegin(); it != result->cend(); ++it) tagCache.insert(it.key(), it.value());
                for (auto it = errors->cbegin(); it != errors->cend(); ++it) tagErrors.insert(it.key(), it.value());
                if (grouping == Browser::GroupTags) invalidate();
                if (rowCount()) emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1), {TagsRole});
            }
            tagTimer.start();
        });
        QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        worker->start();
    }
};

void paintHeading(QPainter *painter, const QRect &rect, const QPalette &palette, QFont font, const QString &text) {
    painter->save();
    painter->fillRect(rect, palette.color(QPalette::Base));
    font.setWeight(QFont::DemiBold);
    painter->setFont(font);
    painter->setPen(palette.color(QPalette::Text));
    painter->drawText(rect.adjusted(12, 0, -12, -2), Qt::AlignLeft | Qt::AlignVCenter, text);
    QColor rule = palette.color(QPalette::Text); rule.setAlpha(35);
    painter->setPen(rule);
    painter->drawLine(rect.bottomLeft() + QPoint(12, 0), rect.bottomRight() - QPoint(12, 0));
    painter->restore();
}

class FinderCategoryDrawer : public KCategoryDrawer {
public:
    using KCategoryDrawer::KCategoryDrawer;
    int categoryHeight(const QModelIndex &, const QStyleOption &) const override { return SectionHeight; }
    int leftMargin() const override { return 8; }
    int rightMargin() const override { return 8; }
    void drawCategory(const QModelIndex &index, int, const QStyleOption &option, QPainter *painter) const override {
        paintHeading(painter, QRect(option.rect.left(), option.rect.top(), option.rect.width(), SectionHeight), option.palette, view()->font(),
                     index.data(KCategorizedSortFilterProxyModel::CategoryDisplayRole).toString());
    }
};

class FileDelegate : public KFileItemDelegate {
public:
    using KFileItemDelegate::KFileItemDelegate;
    std::function<void(const QModelIndex &, const QString &)> rename;
    bool sectionRows = false;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QSize size = KFileItemDelegate::sizeHint(option, index);
        size.setHeight(24 + (sectionRows && index.data(SectionStartRole).toBool() ? SectionHeight : 0));
        return size;
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const QStringList tags = index.column() == KDirModel::Name ? index.data(TagsRole).toStringList() : QStringList();
        QStyleOptionViewItem content(option);
        if (sectionRows && index.data(SectionStartRole).toBool()) content.rect.setTop(content.rect.top() + SectionHeight);
        const int dotsWidth = tags.isEmpty() ? 0 : qMin(3, int(tags.size())) * 9 + 6;
        content.rect.adjust(0, 0, -dotsWidth, 0);
        KFileItemDelegate::paint(painter, content, index);
        painter->save(); painter->setRenderHint(QPainter::Antialiasing); painter->setPen(Qt::NoPen);
        for (int n = 0; n < qMin(3, int(tags.size())); ++n) {
            painter->setBrush(FileTags::tagColor(tags.at(n)));
            painter->drawEllipse(QPointF(content.rect.right() + 7 + n * 9, content.rect.center().y()), 3, 3);
        }
        painter->restore();
    }
    void setModelData(QWidget *editor, QAbstractItemModel *, const QModelIndex &index) const override {
        if (auto *text = qobject_cast<QTextEdit *>(editor); text && rename) rename(index, text->toPlainText());
    }
    void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        // FinderListView::visualRect already excludes its preceding heading.
        KFileItemDelegate::updateEditorGeometry(editor, option, index);
    }
};

struct IconOptions {
    int spacing = 16;
    int textSize = 13;
    Browser::LabelPosition label = Browser::LabelBelow;
};

class IconDelegate : public FileDelegate {
public:
    explicit IconDelegate(QListView *view, IconOptions *options, bool gallery) :
        FileDelegate(view), m_view(view), m_options(options), m_gallery(gallery) {}

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override {
        Q_UNUSED(option);
        return m_view->gridSize();
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setRenderHint(QPainter::SmoothPixmapTransform);
        const int size = m_view->iconSize().width();
        const bool rightLabel = !m_gallery && m_options->label == Browser::LabelRight;
        const QRect iconRect(rightLabel ? option.rect.left() + 8 : option.rect.center().x() - size / 2,
                             rightLabel ? option.rect.center().y() - size / 2 : option.rect.top() + 8, size, size);
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
        // Paint decoded native thumbnails directly, preserving their pixels,
        // aspect ratio and device scale. A MIME icon remains the honest fallback.
        QPixmap preview;
        if (decoration.metaType() == QMetaType::fromType<QPixmap>()) preview = decoration.value<QPixmap>();
        else if (decoration.metaType() == QMetaType::fromType<QImage>()) preview = QPixmap::fromImage(decoration.value<QImage>());
        if (!preview.isNull()) {
            QSizeF extent = preview.deviceIndependentSize();
            extent.scale(QSizeF(iconRect.size()), Qt::KeepAspectRatio);
            const QRectF target(QPointF(iconRect.center()) - QPointF(extent.width()/2, extent.height()/2), extent);
            painter->drawPixmap(target, preview, QRectF(preview.rect()));
        } else decoration.value<QIcon>().paint(painter, iconRect, Qt::AlignCenter, enabled ? QIcon::Normal : QIcon::Disabled);

        QFont font(option.font);
        font.setPointSize(m_options->textSize);
        const auto entry = index.data(KDirModel::FileItemRole).value<KFileItem>();
        if (entry.isLink()) font.setItalic(true);
        const QFontMetrics metrics(font);
        const QStringList tags = index.data(TagsRole).toStringList();
        const int dotsWidth = tags.isEmpty() ? 0 : qMin(3, int(tags.size())) * 9 + 4;
        const int labelLeft = rightLabel ? iconRect.right() + 12 : option.rect.left() + 6;
        const int labelWidth = option.rect.right() - labelLeft - 5;
        const int width = qMax(16, labelWidth - dotsWidth);
        const QString name = index.data(Qt::DisplayRole).toString();
        QTextLayout wrapped(name, font);
        QTextOption textOption(rightLabel ? Qt::AlignLeft : Qt::AlignHCenter);
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
        const int labelTop = rightLabel ? option.rect.center().y() - lines.size() * lineHeight / 2 : iconRect.bottom() + 7;
        int widest = 0;
        for (const auto &line : lines) widest = qMax(widest, metrics.horizontalAdvance(line));
        const qreal contentLeft = rightLabel ? labelLeft : labelLeft + (labelWidth - widest - dotsWidth) / 2.0;
        QRectF label(contentLeft + dotsWidth - 3, labelTop - 1, widest + 6, lines.size() * lineHeight + 2);
        if (selected) {
            painter->setPen(Qt::NoPen); painter->setBrush(QColor("#296ec6"));
            painter->drawRoundedRect(label, 3, 3);
        }
        painter->setPen(selected ? QColor(Qt::white) :
                        option.palette.color(enabled ? QPalette::Active : QPalette::Disabled, QPalette::Text));
        for (int number = 0; number < lines.size(); ++number) {
            const QRectF textRect(contentLeft + dotsWidth, labelTop + number * lineHeight, widest, lineHeight);
            painter->drawText(textRect, (rightLabel ? Qt::AlignLeft : Qt::AlignHCenter) | Qt::AlignVCenter, lines.at(number));
        }
        painter->setPen(Qt::NoPen);
        for (int n = 0; n < qMin(3, int(tags.size())); ++n) {
            painter->setBrush(FileTags::tagColor(tags.at(n)));
            painter->drawEllipse(QPointF(contentLeft + 3 + n * 9, labelTop + lineHeight / 2.0), 3.5, 3.5);
        }
        painter->restore();
    }

    void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option, const QModelIndex &) const override {
        QFont font(option.font); font.setPointSize(m_options->textSize);
        const int height = qMax(24, 2 * QFontMetrics(font).height() + 6);
        const bool right = !m_gallery && m_options->label == Browser::LabelRight;
        editor->setGeometry(right ? option.rect.left() + m_view->iconSize().width() + 20 : option.rect.left() + 5,
                            right ? option.rect.center().y() - height / 2 : option.rect.top() + 8 + m_view->iconSize().height() + 6,
                            option.rect.width() - (right ? m_view->iconSize().width() + 25 : 10), height);
    }

private:
    QListView *m_view;
    IconOptions *m_options;
    bool m_gallery;
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
        FileDelegate::paint(painter, content, index);
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

class FinderListView : public DropView<QTreeView> {
public:
    using DropView::DropView;
    QRect visualRect(const QModelIndex &index) const override {
        QRect rect = QTreeView::visualRect(index);
        if (index.data(SectionStartRole).toBool()) rect.setTop(rect.top() + SectionHeight);
        return rect;
    }
    QModelIndex indexAt(const QPoint &point) const override {
        const QModelIndex index = QTreeView::indexAt(point);
        if (index.data(SectionStartRole).toBool() && point.y() < QTreeView::visualRect(index).top() + SectionHeight)
            return {};
        return index;
    }
protected:
    void mousePressEvent(QMouseEvent *event) override {
        if (onHeading(event->position().toPoint())) { event->accept(); return; }
        QTreeView::mousePressEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (onHeading(event->position().toPoint())) { event->accept(); return; }
        QTreeView::mouseReleaseEvent(event);
    }
    void mouseDoubleClickEvent(QMouseEvent *event) override {
        if (onHeading(event->position().toPoint())) { event->accept(); return; }
        QTreeView::mouseDoubleClickEvent(event);
    }
    void setSelection(const QRect &rectangle, QItemSelectionModel::SelectionFlags flags) override {
        QRect content = rectangle.normalized();
        const QModelIndex first = QTreeView::indexAt(content.topLeft());
        const QModelIndex last = QTreeView::indexAt(content.bottomRight());
        // Qt's base tree substitutes the first/last model row for an invalid
        // endpoint. A section heading is interior space, so trim it instead.
        if (first.data(SectionStartRole).toBool() && content.top() < visualRect(first).top())
            content.setTop(visualRect(first).top());
        if (last.data(SectionStartRole).toBool() && content.bottom() < visualRect(last).top()) {
            const QModelIndex previous = indexAbove(last);
            if (!previous.isValid()) return;
            content.setBottom(visualRect(previous).bottom());
        }
        if (content.bottom() < content.top()) return;
        QTreeView::setSelection(content, flags);
    }
    void drawRow(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QTreeView::drawRow(painter, option, index);
        if (index.data(SectionStartRole).toBool()) {
            paintHeading(painter, QRect(0, option.rect.top(), viewport()->width(), SectionHeight),
                         option.palette, option.font, index.data(KCategorizedSortFilterProxyModel::CategoryDisplayRole).toString());
        }
    }
    void drawBranches(QPainter *painter, const QRect &rect, const QModelIndex &index) const override {
        QRect content(rect);
        if (index.data(SectionStartRole).toBool()) content.setTop(content.top() + SectionHeight);
        QTreeView::drawBranches(painter, content, index);
    }
private:
    bool onHeading(const QPoint &point) const {
        const QModelIndex index = QTreeView::indexAt(point);
        return index.data(SectionStartRole).toBool() && point.y() < visualRect(index).top();
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
    FinderProxy *proxy;
    QItemSelectionModel *selection;
    QItemSelectionModel *columnSelection;
    QStackedWidget *stack;
    DropView<KCategorizedView> *icons;
    FinderListView *list;
    ColumnsView *columns;
    DropView<QListView> *strip;
    QWidget *gallery;
    QWidget *galleryHost;
    QWidget *columnHost;
    QWidget *sidePreviewHost;
    KFilePreviewGenerator *thumbnailGenerator = nullptr;
    QUrl rootUrl;
    QUrl currentUrl;
    Mode currentMode = Icons;
    QList<QUrl> pendingSelection;
    bool changing = false;
    bool previews = true;
    bool sidePreview = false;
    IconOptions iconOptions;

    explicit Private(Browser *owner) : q(owner) {}

    void layoutIcons() {
        QFont font = icons->font(); font.setPointSize(iconOptions.textSize);
        const int lineHeight = QFontMetrics(font).height();
        const int size = icons->iconSize().width();
        const int width = iconOptions.label == LabelRight ? size + 144 + iconOptions.spacing : size + 40 + iconOptions.spacing;
        const int height = iconOptions.label == LabelRight ? qMax(size + 16, 2 * lineHeight + 20) : size + 24 + 2 * lineHeight;
        icons->setGridSize(QSize(width, height));
        icons->viewport()->update();
        // Gallery uses 64px thumbnails independently of the Icons zoom, with
        // modest extra space spread across the visible horizontal filmstrip.
        const int available = strip->viewport()->width();
        const int columns = qMax(1, available / 96);
        const int stripWidth = qBound(96, available / columns, 112);
        const QSize stripGrid(stripWidth, 64 + 20 + 2 * lineHeight);
        if (strip->gridSize() != stripGrid) strip->setGridSize(stripGrid);
        strip->setFixedHeight(stripGrid.height() + 16);
        strip->viewport()->update();
    }

    void updatePreviewVisibility() {
        sidePreviewHost->setVisible(sidePreview && currentMode != Gallery && currentMode != Columns);
    }

    void updateThumbnailGenerator() {
        // A disabled generator still responds to shared KDirModel changes and
        // resolves generic icons. Keep exactly one generator for the active
        // thumbnail view so it cannot overwrite another view's decoded pixels.
        delete thumbnailGenerator;
        thumbnailGenerator = nullptr;
        if (currentMode != Icons && currentMode != Gallery) return;
        thumbnailGenerator = new KFilePreviewGenerator(currentMode == Icons ? static_cast<QAbstractItemView *>(icons) : strip);
        thumbnailGenerator->setEnabledPlugins(KIO::PreviewJob::defaultPlugins());
        thumbnailGenerator->setPreviewShown(previews);
        thumbnailGenerator->updateIcons();
    }

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
        auto *compress = menu.addAction(QStringLiteral("Compress"), q, &Browser::compressSelected);
        compress->setEnabled(q->canCompressSelection());
        menu.addSeparator();
        auto *trash = menu.addAction(QStringLiteral("Move to Trash"), q, &Browser::trashSelected);
        trash->setEnabled(hasSelection);
        auto *info = menu.addAction(QStringLiteral("Get Info"), [this] {
            const auto items = q->selectedItems();
            auto *dialog = items.isEmpty() ? new KPropertiesDialog(currentUrl, q->window()) : new KPropertiesDialog(items, q->window());
            dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->show();
        });
        info->setEnabled(currentUrl.isValid());
        menu.addSeparator();
        auto *groups = menu.addMenu(QStringLiteral("Group By"));
        const QStringList groupNames{"None", "Name", "Kind", "Date Modified", "Size", "Tags"};
        for (int number = 0; number < groupNames.size(); ++number) {
            auto *action = groups->addAction(groupNames.at(number));
            action->setCheckable(true); action->setChecked(q->group() == Group(number));
            QObject::connect(action, &QAction::triggered, q, [this, number] { q->setGroup(Group(number)); });
        }
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
        FileDelegate *delegate = view == icons || view == strip ? new IconDelegate(static_cast<QListView *>(view), &iconOptions, view == strip) :
                                 childColumn ? new ColumnDelegate(view) : new FileDelegate(view);
        delegate->rename = [this](const QModelIndex &index, const QString &name) { rename(index, name); };
        delegate->sectionRows = view == list;
        if (view != icons && view != strip) delegate->setMaximumSize(QSize(10000, 24 + SectionHeight));
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
    d->proxy = new FinderProxy(this);
    d->proxy->setSourceModel(d->model);
    d->proxy->watchSource();
    d->proxy->setSortFoldersFirst(false);
    d->proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    d->proxy->setFilterKeyColumn(0);
    d->proxy->sort(KDirModel::Name, Qt::AscendingOrder);
    d->selection = new QItemSelectionModel(d->proxy, this);
    // QColumnView swaps its selection among its child columns. Keep it separate
    // so hidden columns cannot react to selections or clear another view's state.
    d->columnSelection = new QItemSelectionModel(d->proxy, this);
    d->stack = new QStackedWidget(this);
    auto *layout = new QHBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0); layout->addWidget(d->stack, 1);
    d->sidePreviewHost = new QWidget(this); d->sidePreviewHost->setObjectName("finderSidePreview");
    d->sidePreviewHost->setFixedWidth(280); layout->addWidget(d->sidePreviewHost); d->sidePreviewHost->hide();
    d->icons = new DropView<KCategorizedView>(d->stack); d->icons->setObjectName("finderIconView");
    d->icons->setViewMode(QListView::IconMode); d->icons->setResizeMode(QListView::Adjust);
    d->icons->setMovement(QListView::Static); d->icons->setIconSize({64, 64});
    d->icons->setWordWrap(true); d->icons->setUniformItemSizes(true); d->icons->setCategoryDrawer(new FinderCategoryDrawer(d->icons));
    d->icons->setCategorySpacing(8); d->icons->setCollapsibleBlocks(false);
    d->list = new FinderListView(d->stack); d->list->setObjectName("finderListView");
    d->list->setRootIsDecorated(true); d->list->setAlternatingRowColors(true); d->list->setUniformRowHeights(false);
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
    d->strip->setMovement(QListView::Static); d->strip->setIconSize({64, 64});
    d->strip->setResizeMode(QListView::Adjust); d->strip->setWordWrap(true); d->strip->setUniformItemSizes(true);
    d->strip->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    galleryLayout->addWidget(d->strip);
    d->layoutIcons();
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
    // A private KDE profile need not have PreviewSettings populated. Native
    // default thumbnailers include installed image/PDF workers and their cache.
    d->updateThumbnailGenerator();
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
                  "#finderBrowser QTreeView{alternate-background-color:#252525;}"
                  "#finderBrowser QAbstractItemView::item:selected{background:#3b3b40;color:white;}"
                  "#finderBrowser QColumnView::item{height:24px;}"
                  "#finderGalleryPreview,#finderColumnPreview,#finderSidePreview{background:#1e1e1e;}"
                  "#finderBrowser QHeaderView::section{background:#29292b;color:#d7d7da;border:0;padding:4px 8px;}");
}

Browser::~Browser() = default;
QUrl Browser::url() const { return d->currentUrl; }
Browser::Mode Browser::mode() const { return d->currentMode; }
int Browser::iconSize() const { return d->icons->iconSize().width(); }
Browser::Group Browser::group() const { return d->proxy->grouping; }
int Browser::gridSpacing() const { return d->iconOptions.spacing; }
int Browser::textSize() const { return d->iconOptions.textSize; }
Browser::LabelPosition Browser::labelPosition() const { return d->iconOptions.label; }
bool Browser::iconPreviewsShown() const { return d->previews; }
bool Browser::previewShown() const { return d->sidePreview; }
bool Browser::canCompressSelection() const {
    const auto urls = selectedUrls();
    return !urls.isEmpty() && !QStandardPaths::findExecutable(QStringLiteral("ark")).isEmpty() &&
        std::all_of(urls.cbegin(), urls.cend(), [](const QUrl &url) { return url.isLocalFile(); });
}
int Browser::sortColumn() const { return d->proxy->sortColumn(); }
bool Browser::sortAscending() const { return d->proxy->sortOrder() == Qt::AscendingOrder; }
QWidget *Browser::galleryPreviewHost() const { return d->galleryHost; }
QWidget *Browser::columnPreviewHost() const { return d->columnHost; }
QWidget *Browser::previewHost() const { return d->sidePreviewHost; }
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
    d->proxy->refreshTags();
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
    d->updateThumbnailGenerator();
    d->updatePreviewVisibility(); d->layoutIcons();
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
    d->layoutIcons();
    if (d->thumbnailGenerator && mode() == Icons) d->thumbnailGenerator->updateIcons();
}
void Browser::setGroup(Group value) {
    if (value < GroupNone || value > GroupTags || group() == value) return;
    const auto urls = selectedUrls();
    const bool previousChanging = d->changing;
    d->changing = true;
    d->proxy->setGroup(value);
    d->list->doItemsLayout();
    d->icons->doItemsLayout();
    selectUrls(urls);
    d->changing = previousChanging;
    d->selected();
}
void Browser::setGridSpacing(int spacing) {
    d->iconOptions.spacing = qBound(0, spacing, 128);
    d->layoutIcons();
}
void Browser::setTextSize(int points) {
    d->iconOptions.textSize = qBound(9, points, 20);
    QFont font = d->icons->font(); font.setPointSize(d->iconOptions.textSize);
    d->icons->setFont(font); d->strip->setFont(font);
    d->list->setFont(font); d->columns->setFont(font);
    d->layoutIcons(); d->list->doItemsLayout();
}
void Browser::setLabelPosition(LabelPosition position) {
    if (position < LabelBelow || position > LabelRight) return;
    d->iconOptions.label = position;
    d->layoutIcons();
}
void Browser::setIconPreviewsShown(bool shown) {
    d->previews = shown;
    if (d->thumbnailGenerator) d->thumbnailGenerator->setPreviewShown(shown);
}
void Browser::setPreviewShown(bool shown) {
    d->sidePreview = shown;
    d->updatePreviewVisibility();
    d->selected();
}
void Browser::refreshTags() { d->proxy->refreshTags(); }
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
void Browser::compressSelected() {
    if (!canCompressSelection()) {
        emit errorOccurred(QStringLiteral("Compression requires Ark and a selection of local files."));
        return;
    }
    // Ark supplies native archive progress, format handling and errors. Each
    // URL is a separate process argument; filenames never become shell code.
    const auto urls = selectedUrls();
    QStringList arguments{"--add", "--changetofirstpath", "--autofilename", "zip", "--"};
    for (const auto &url : urls) arguments.append(url.toLocalFile());
    auto *process = new QProcess(this);
    process->setProgram(QStandardPaths::findExecutable(QStringLiteral("ark")));
    process->setArguments(arguments);
    process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            emit errorOccurred(QStringLiteral("Could not start the archive tool: ") + process->errorString());
            process->deleteLater();
        }
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
        [this, process](int code, QProcess::ExitStatus status) {
            if (code != 0 || status != QProcess::NormalExit) emit errorOccurred(QStringLiteral("The archive tool could not finish compression."));
            else emit statusMessage(QStringLiteral("Compression complete"));
            process->deleteLater();
        });
    emit statusMessage(QStringLiteral("Compressing…"));
    process->start();
}
void Browser::undo() { auto *manager = KIO::FileUndoManager::self(); manager->uiInterface()->setParentWidget(window()); if (manager->isUndoAvailable()) manager->undo(); }
void Browser::drop(QDropEvent *event, QUrl destination) {
    if (!event || !event->mimeData()->hasUrls()) { if (event) event->ignore(); return; }
    if (destination.isEmpty()) destination = url();
    auto *job = KIO::drop(event, destination);
    d->job(job, "Transferring…"); event->acceptProposedAction();
}
bool Browser::eventFilter(QObject *object, QEvent *event) {
    if (event->type() == QEvent::Resize && object == d->strip->viewport()) {
        // Defer until Qt has established the viewport geometry; no animation
        // loop or background polling is needed for a stationary filmstrip.
        QTimer::singleShot(0, this, [this] { d->layoutIcons(); });
    }
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
