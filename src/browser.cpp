// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
// Public Qt/KIO views and jobs; no private Dolphin ABI or shell file mutations.
#include "alerts.h"
#include "browser.h"
#include "filetarget.h"
#include "appearance.h"
#include "tags.h"
#include "preferences.h"
#include "batchrename.h"
#include "filedetails.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QCache>
#include <QColumnView>
#include <QCollator>
#include <QDate>
#include <QDateTime>
#include <QLocale>
#include <QDir>
#include <QDrag>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHeaderView>
#include <QHash>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QMimeDatabase>
#include <QMimeData>
#include <QMimeType>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPersistentModelIndex>
#include <QProcess>
#include <QProxyStyle>
#include <QPushButton>
#include <QQueue>
#include <QScrollBar>
#include <QScopedValueRollback>
#include <QSettings>
#include <QSet>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QSplitter>
#include <QThread>
#include <QTimer>
#include <QTreeView>
#include <QUuid>
#include <QTextEdit>
#include <QTextLayout>
#include <QVBoxLayout>
#include <KDirModel>
#include <KDirLister>
#include <KDirSortFilterProxyModel>
#include <KCategorizedView>
#include <KCategoryDrawer>
#include <KFileItemDelegate>
#include <KFileItemActions>
#include <KFileItemListProperties>
#include <KPropertiesDialog>
#include <KProtocolManager>
#include <KJobWidgets>
#include <KUrlMimeData>
#include <KIO/CopyJob>
#include <KIO/DropJob>
#include <KIO/FileUndoManager>
#include <KIO/Job>
#include <KIO/JobUiDelegateFactory>
#include <KIO/MkdirJob>
#include <KIO/SimpleJob>
#include <KIO/PasteJob>
#include <KIO/PreviewJob>
#include <KIO/UDSEntry>

#include <functional>
#include <algorithm>
#include <array>
#include <KIO/Global>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <limits>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using DropHandler = std::function<void(QDropEvent *, const QModelIndex &)>;
constexpr int TagsRole = Browser::TagsRole;
constexpr int SectionStartRole = Qt::UserRole + 132;
constexpr int CutRole = Qt::UserRole + 133;
constexpr int SectionHeight = 30;

QString itemType(const KFileItem &item) {
    const auto supplied = item.entry().stringValue(KIO::UDSEntry::UDS_DISPLAY_TYPE);
    // Unlike mimeComment(), this does not open .desktop/.directory files.
    return supplied.isEmpty() ? item.currentMimeType().comment() : supplied;
}

// A mouse-transparent presentation layer: native views keep their selection,
// background context menu, drag/drop, keyboard and scrolling surfaces.
class DirectoryState final : public QWidget {
public:
    QString title, detail, symbol;
    explicit DirectoryState(QWidget *parent) : QWidget(parent) {
        setObjectName(QStringLiteral("finderDirectoryState"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
        hide();
    }
    void showState(QString heading, QString message, QString icon) {
        title = std::move(heading); detail = std::move(message); symbol = std::move(icon);
        setAccessibleName(title); setAccessibleDescription(detail);
        setProperty("state", symbol); setProperty("title", title); setProperty("detail", detail);
        setVisible(!title.isEmpty()); update();
    }
protected:
    void paintEvent(QPaintEvent *) override {
        if (title.isEmpty() || width() < 120 || height() < 55) return;
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        const int textWidth = qMin(340, width() - 32);
        QFont heading = font(); heading.setPixelSize(15); heading.setWeight(QFont::DemiBold);
        QFont body = font(); body.setPixelSize(13);
        const QFontMetrics hm(heading), bm(body);
        const QRect bounds(0,0,textWidth,1000);
        const auto h = hm.boundingRect(bounds,Qt::AlignHCenter | Qt::TextWordWrap,title);
        const auto b = bm.boundingRect(bounds,Qt::AlignHCenter | Qt::TextWordWrap,detail);
        const bool iconVisible = height() >= h.height() + b.height() + 108;
        const int total = h.height() + b.height() + 8 + (iconVisible ? 70 : 0);
        const int top = qMax(12,(height()-total)/2);
        const QColor secondary = FinderAppearance::color(FinderAppearance::SecondaryText);
        if (iconVisible) {
            p.save(); p.translate(width()/2.0-24,top);
            p.setPen(QPen(secondary,1.8,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin)); p.setBrush(Qt::NoBrush);
            if (symbol == "error") {
                QPainterPath triangle; triangle.moveTo(24,3);triangle.lineTo(47,43);triangle.lineTo(1,43);triangle.closeSubpath();
                p.drawPath(triangle); p.drawLine(QPointF(24,16),QPointF(24,29));p.drawPoint(QPointF(24,36));
            } else if (symbol == "loading" || symbol == "recents") {
                p.drawEllipse(QRectF(3,3,42,42)); p.drawLine(QPointF(24,11),QPointF(24,24));p.drawLine(QPointF(24,24),QPointF(33,29));
            } else if (symbol == "filter") {
                p.drawEllipse(QRectF(3,3,30,30));p.drawLine(QPointF(29,29),QPointF(45,45));
            } else {
                QPainterPath folder; folder.moveTo(2,14);folder.lineTo(2,9);folder.quadTo(2,6,5,6);
                folder.lineTo(18,6);folder.lineTo(24,12);folder.lineTo(43,12);folder.quadTo(46,12,46,15);
                folder.lineTo(46,39);folder.quadTo(46,42,43,42);folder.lineTo(5,42);folder.quadTo(2,42,2,39);folder.closeSubpath();
                p.drawPath(folder);p.drawLine(QPointF(3,18),QPointF(45,18));
            }
            p.restore();
        }
        const int y = top + (iconVisible ? 70 : 0);
        p.setPen(FinderAppearance::palette().color(QPalette::Text));p.setFont(heading);
        p.drawText(QRect((width()-textWidth)/2,y,textWidth,h.height()),Qt::AlignHCenter | Qt::TextWordWrap,title);
        p.setPen(secondary);p.setFont(body);
        p.drawText(QRect((width()-textWidth)/2,y+h.height()+8,textWidth,b.height()),Qt::AlignHCenter | Qt::TextWordWrap,detail);
    }
};

// Keep the KDirModel tree and its native column identities. Additional indexes
// share the native row pointer; all native KIO/selection/thumbnail paths continue
// to map straight through FinderProxy to this real directory model.
class MetadataDirModel : public KDirModel {
public:
    explicit MetadataDirModel(QObject *parent) : KDirModel(parent) {
        birthTimer.setSingleShot(true);
        birthTimer.setInterval(40);
        QObject::connect(&birthTimer, &QTimer::timeout, this, [this] { readBirthTimes(); });
        QObject::connect(this, &QAbstractItemModel::rowsInserted, this,
            [this](const QModelIndex &parent, int first, int last) { enqueueBirthTimes(parent, first, last); });
        QObject::connect(this, &QAbstractItemModel::rowsAboutToBeRemoved, this,
            [this](const QModelIndex &parent, int first, int last) {
                for (int row = first; row <= last; ++row) {
                    const auto item = itemForIndex(index(row, 0, parent));
                    births.remove(item.url()); pendingBirths.remove(item.url());
                }
            });
        QObject::connect(this, &QAbstractItemModel::modelReset, this, [this] { refreshMetadata(); });
        QObject::connect(dirLister(), &KDirLister::refreshItems, this,
            [this](const QList<QPair<KFileItem, KFileItem>> &items) {
                if (items.isEmpty()) return;
                for (const auto &pair : items) { births.remove(pair.first.url()); births.remove(pair.second.url()); }
                restartBirthTimes();
            });
    }
    ~MetadataDirModel() override { if (birthWorker) birthWorker->requestInterruption(); }
    int columnCount(const QModelIndex & = {}) const override { return Browser::ListColumnCount; }
    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override {
        if (column < 0 || column >= Browser::ListColumnCount) return {};
        if (column < KDirModel::ColumnCount) return KDirModel::index(row, column, parent);
        const auto native = KDirModel::index(row, 0, parent);
        return native.isValid() ? createIndex(row, column, native.internalPointer()) : QModelIndex();
    }
    QModelIndex sibling(int row, int column, const QModelIndex &from) const override {
        return index(row, column, parent(from));
    }
    Qt::ItemFlags flags(const QModelIndex &index) const override {
        const auto native = index.siblingAtColumn(0);
        const auto flags = KDirModel::flags(native);
        return index.column() >= KDirModel::ColumnCount ? flags & ~Qt::ItemIsEditable : KDirModel::flags(index);
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override {
        if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
            switch (section) {
            case Browser::NameColumn: return QStringLiteral("Name");
            case Browser::DateModifiedColumn: return QStringLiteral("Date Modified");
            case Browser::KindColumn: return QStringLiteral("Kind");
            case Browser::DateCreatedColumn: return QStringLiteral("Date Created");
            case Browser::DateAddedColumn: return QStringLiteral("Date Added");
            case Browser::LastOpenedColumn: return QStringLiteral("Last Opened");
            case Browser::TagsColumn: return QStringLiteral("Tags");
            default: break;
            }
        }
        return KDirModel::headerData(section, orientation, role);
    }
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override {
        if (!index.isValid()) return {};
        const auto item = itemForIndex(index.siblingAtColumn(0));
        const auto entry = item.entry();
        const auto timestamp = [&entry](uint field) -> QVariant {
            return entry.contains(field) ? QVariant(QDateTime::fromSecsSinceEpoch(entry.numberValue(field))) : QVariant();
        };
        if (role == Browser::DateCreatedRole) {
            if (!item.url().isLocalFile() && entry.contains(KIO::UDSEntry::UDS_CREATION_TIME)) return timestamp(KIO::UDSEntry::UDS_CREATION_TIME);
            const auto date = births.value(item.url());
            return date.isValid() ? QVariant(date) : QVariant();
        }
        if (role == Browser::DateAddedRole || role == Browser::LastOpenedRole) return {};
        if (role == Browser::ModificationTimeRole) return timestamp(KIO::UDSEntry::UDS_MODIFICATION_TIME);
        if (role == Browser::AccessTimeRole) return timestamp(KIO::UDSEntry::UDS_ACCESS_TIME);
        if (role == Browser::LogicalSizeRole)
            return !item.isDir() && entry.contains(KIO::UDSEntry::UDS_SIZE) ? QVariant::fromValue(qulonglong(item.size())) : QVariant();
        if (role == Browser::MetadataPendingRole)
            return pendingBirths.contains(item.url());
        if (role == Browser::RawValueRole) {
            switch (index.column()) {
            case Browser::SizeColumn: return data(index, Browser::LogicalSizeRole);
            case Browser::DateModifiedColumn: return data(index, Browser::ModificationTimeRole);
            case Browser::DateCreatedColumn: return data(index, Browser::DateCreatedRole);
            case Browser::DateAddedColumn: return data(index, Browser::DateAddedRole);
            case Browser::LastOpenedColumn: return data(index, Browser::LastOpenedRole);
            default: return index.column() < KDirModel::ColumnCount ? KDirModel::data(index, Qt::DisplayRole) : QVariant();
            }
        }
        if (index.column() >= KDirModel::ColumnCount) {
            if (role == Qt::TextAlignmentRole) return int(Qt::AlignLeft | Qt::AlignVCenter);
            if (role == Qt::DisplayRole || role == Qt::ToolTipRole) {
                const auto date = data(index, Browser::RawValueRole).toDateTime();
                if (date.isValid()) return QLocale().toString(date, QLocale::ShortFormat);
                if (role == Qt::ToolTipRole) {
                    if (index.column() == Browser::DateAddedColumn) return QStringLiteral("Unavailable: no date-added provider is connected.");
                    if (index.column() == Browser::LastOpenedColumn) return QStringLiteral("Unavailable: no application-open history provider is connected. Filesystem access time is not last opened.");
                    return QStringLiteral("Unavailable: the filesystem/provider supplies no real creation time.");
                }
                return index.column() == Browser::DateCreatedColumn && pendingBirths.contains(item.url()) ?
                    QStringLiteral("Reading…") : QStringLiteral("—");
            }
            if (role == KDirModel::FileItemRole || role == KDirModel::ChildCountRole ||
                role == KDirModel::HasJobRole || role == KDirModel::HandleSequencesRole)
                return KDirModel::data(index.siblingAtColumn(0), role);
            return {};
        }
        return KDirModel::data(index, role);
    }
    void refreshMetadata() {
        births.clear();
        restartBirthTimes();
    }
private:
    QHash<QUrl, QDateTime> births;
    QSet<QUrl> pendingBirths;
    QQueue<QUrl> birthQueue;
    QTimer birthTimer;
    QPointer<QThread> birthWorker;
    quint64 birthRevision = 0;
    void restartBirthTimes() {
        ++birthRevision;
        if (birthWorker) birthWorker->requestInterruption();
        birthQueue.clear(); pendingBirths.clear(); birthTimer.stop();
        enqueueBirthTimes({}, 0, rowCount() - 1);
    }
    void enqueueBirthTimes(const QModelIndex &parent, int first, int last) {
        for (int row = first; row <= last; ++row) {
            const auto index = this->index(row, 0, parent);
            const auto item = itemForIndex(index);
            if (item.url().isLocalFile() && !item.isLink() &&
                !births.contains(item.url()) && !pendingBirths.contains(item.url())) {
                pendingBirths.insert(item.url()); birthQueue.enqueue(item.url());
            }
            const int children = rowCount(index);
            if (children) enqueueBirthTimes(index, 0, children - 1);
        }
        if (!birthWorker && !birthQueue.isEmpty() && !birthTimer.isActive()) birthTimer.start();
    }
    void readBirthTimes() {
        if (birthWorker) return;
        QList<QUrl> urls;
        while (!birthQueue.isEmpty() && urls.size() < 128) {
            const auto url = birthQueue.dequeue();
            if (pendingBirths.contains(url) && !births.contains(url)) urls.append(url);
        }
        if (urls.isEmpty()) return;
        const auto revision = birthRevision;
        auto result = std::make_shared<QHash<QUrl, QDateTime>>();
        auto *worker = QThread::create([urls, result] {
            for (const auto &url : urls) {
                if (QThread::currentThread()->isInterruptionRequested()) break;
                // QFileInfo birthTime returns invalid when unsupported; ctime
                // and atime are never used to fabricate Finder metadata.
                result->insert(url, QFileInfo(url.toLocalFile()).birthTime());
            }
        });
        birthWorker = worker;
        QObject::connect(worker, &QThread::finished, this, [this, revision, result] {
            birthWorker = nullptr;
            if (revision == birthRevision) {
                for (auto it = result->cbegin(); it != result->cend(); ++it) {
                    births.insert(it.key(), it.value()); pendingBirths.remove(it.key());
                    const auto index = indexForUrl(it.key());
                    if (index.isValid()) emit dataChanged(index, index.siblingAtColumn(Browser::ListColumnCount - 1),
                        {Qt::DisplayRole, Qt::ToolTipRole, Browser::DateCreatedRole, Browser::RawValueRole, Browser::MetadataPendingRole});
                }
            }
            if (!birthQueue.isEmpty()) birthTimer.start(0);
        });
        QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        worker->start();
    }
};

struct DirectorySize {
    Browser::SizeState state = Browser::SizeNotCalculated;
    quint64 bytes = 0;
    QString error;
};
struct SizeScan {
    QHash<QUrl, DirectorySize> sizes;
    std::atomic<quint64> entries{0};
    std::atomic<int> completed{0};
    std::atomic<bool> cancelRequested{false};
    bool canceled = false;
    bool limited = false;
};

// Use directory descriptors and no-follow stat/open calls. Symlink loops never
// enter the traversal, and path replacement cannot redirect an opened parent.
// The worker owns only a bounded stack of DIR handles, never any GUI/model.
void scanDirectorySizes(const QList<QUrl> &urls, Browser::SizeCalculationLimits limits, const std::shared_ptr<SizeScan> &scan) {
    QElapsedTimer elapsed; elapsed.start();
    const auto stopped = [&] {
        if (scan->cancelRequested.load() || QThread::currentThread()->isInterruptionRequested()) { scan->canceled = true; return true; }
        if (scan->entries.load() >= limits.maxEntries || elapsed.elapsed() >= limits.maxMilliseconds) {
            scan->limited = true; return true;
        }
        return false;
    };
    for (const auto &url : urls) {
        DirectorySize size;
        size.state = Browser::SizeComplete;
        if (stopped()) {
            size.state = Browser::SizeIncomplete;
            size.error = scan->canceled ? QStringLiteral("Calculation canceled.") : QStringLiteral("Calculation budget reached.");
            scan->sizes.insert(url, size);
            continue;
        }
        const auto path = QFile::encodeName(url.toLocalFile());
        const int rootFd = ::open(path.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        struct stat rootStat{};
        if (rootFd < 0 || ::fstat(rootFd, &rootStat) != 0) {
            size.state = Browser::SizeUnavailable;
            size.error = QString::fromLocal8Bit(std::strerror(errno));
            if (rootFd >= 0) ::close(rootFd);
            scan->sizes.insert(url, size); ++scan->completed;
            continue;
        }
        DIR *root = ::fdopendir(rootFd);
        if (!root) {
            size.state = Browser::SizeUnavailable;
            size.error = QString::fromLocal8Bit(std::strerror(errno)); ::close(rootFd);
            scan->sizes.insert(url, size); ++scan->completed;
            continue;
        }
        struct Frame { DIR *directory; int depth; };
        QList<Frame> stack{{root, 0}};
        const auto incomplete = [&size](const QString &reason) {
            size.state = Browser::SizeIncomplete;
            if (size.error.isEmpty()) size.error = reason;
        };
        while (!stack.isEmpty()) {
            if (stopped()) {
                incomplete(scan->canceled ? QStringLiteral("Calculation canceled.") : QStringLiteral("Calculation budget reached."));
                break;
            }
            const auto frame = stack.last();
            errno = 0;
            const auto *entry = ::readdir(frame.directory);
            if (!entry) {
                if (errno) incomplete(QString::fromLocal8Bit(std::strerror(errno)));
                ::closedir(frame.directory); stack.removeLast();
                continue;
            }
            if (!std::strcmp(entry->d_name, ".") || !std::strcmp(entry->d_name, "..")) continue;
            ++scan->entries;
            struct stat info{};
            if (::fstatat(::dirfd(frame.directory), entry->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
                incomplete(QString::fromLocal8Bit(std::strerror(errno))); continue;
            }
            if (S_ISLNK(info.st_mode)) continue;
            if (info.st_dev != rootStat.st_dev) { incomplete(QStringLiteral("A mounted filesystem was skipped.")); continue; }
            if (S_ISREG(info.st_mode)) {
                const auto bytes = quint64(qMax<off_t>(0, info.st_size));
                if (bytes > std::numeric_limits<quint64>::max() - size.bytes) incomplete(QStringLiteral("Logical size overflow."));
                else size.bytes += bytes;
            } else if (S_ISDIR(info.st_mode)) {
                if (frame.depth >= limits.maxDepth) { scan->limited = true; incomplete(QStringLiteral("Directory depth limit reached.")); continue; }
                const int fd = ::openat(::dirfd(frame.directory), entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
                struct stat opened{};
                if (fd < 0 || ::fstat(fd, &opened) != 0) {
                    incomplete(QString::fromLocal8Bit(std::strerror(errno))); if (fd >= 0) ::close(fd); continue;
                }
                if (opened.st_dev != rootStat.st_dev) { ::close(fd); incomplete(QStringLiteral("A mounted filesystem was skipped.")); continue; }
                DIR *child = ::fdopendir(fd);
                if (!child) { incomplete(QString::fromLocal8Bit(std::strerror(errno))); ::close(fd); continue; }
                stack.append({child, frame.depth + 1});
            }
        }
        for (const auto &frame : stack) ::closedir(frame.directory);
        scan->sizes.insert(url, size);
        ++scan->completed;
    }
}

// Files remain real KDirModel rows. Categorization changes their order and
// supplies section metadata, without inserting selectable dummy file indexes.
class FinderProxy : public KDirSortFilterProxyModel {
public:
    explicit FinderProxy(QObject *parent) : KDirSortFilterProxyModel(parent) {
        details = new FileDetailsCache(this);
        detailsTimer.setSingleShot(true); detailsTimer.setInterval(40);
        QObject::connect(&detailsTimer, &QTimer::timeout, this, [this] { loadDetails(); });
        QObject::connect(details, &FileDetailsCache::detailsChanged, this, [this](const QList<QUrl> &urls) {
            for (const auto &url : urls) {
                nameCache.remove(url);
                const auto index = mapFromSource(static_cast<KDirModel *>(sourceModel())->indexForUrl(url));
                if (index.isValid()) emit dataChanged(index, index, {Qt::DisplayRole});
            }
        });
        collator.setNumericMode(true);
        collator.setCaseSensitivity(Qt::CaseInsensitive);
        tagTimer.setSingleShot(true);
        tagTimer.setInterval(40);
        QObject::connect(&tagTimer, &QTimer::timeout, this, [this] { readTags(); });
        groupTimer.setSingleShot(true);
        groupTimer.setInterval(200);
        QObject::connect(&groupTimer, &QTimer::timeout, this, [this] {
            if (grouping == Browser::GroupTags || sortColumn() == Browser::TagsColumn) invalidate();
        });
        refreshPreferences();
    }
    ~FinderProxy() override { if (tagWorker) tagWorker->requestInterruption(); }
    Browser::Group grouping = Browser::GroupNone;
    QSet<QUrl> cutUrls;
    bool foldersFirstPreference = false;
    QHash<QUrl, DirectorySize> directorySizes;
    QSize decorationSize{64, 64};
    qreal decorationScale = 1;

    QMimeData *mimeData(const QModelIndexList &indexes) const override {
        auto *mime = KDirSortFilterProxyModel::mimeData(indexes);
        if (!mime) return nullptr;
        QList<QUrl> targets;
        QSet<QUrl> seen;
        for (const auto &index : indexes) {
            if (index.column() != 0) continue;
            const auto entry = index.data(KDirModel::FileItemRole).value<KFileItem>();
            const auto target = FileTarget::url(entry);
            if (!entry.isNull() && !seen.contains(target)) { seen.insert(target); targets.append(target); }
        }
        // Replace both URL flavors so another native app cannot resurrect a
        // synthetic listing URL via the preferred local-URL clipboard format.
        KUrlMimeData::setUrls(targets, targets, mime);
        return mime;
    }

    void refreshPreferences() {
        showAllExtensions = QSettings().value("Advanced/showAllExtensions", false).toBool();
        tintFolders = QSettings().value("Tags/tintFolders", false).toBool();
        nameCache.clear(); tintCache.clear();
        FinderPreferences::tagRegistry(); // Warm the O(1) color lookup off paint.
    }

    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override {
        setSortFoldersFirst(foldersFirstPreference && column == KDirModel::Name);
        KDirSortFilterProxyModel::sort(column, order);
    }
    void setFoldersFirstPreference(bool enabled) {
        foldersFirstPreference = enabled;
        setSortFoldersFirst(enabled && sortColumn() == KDirModel::Name);
    }

    void sizesChanged(const QList<QUrl> &urls) {
        for (const auto &url : urls) {
            const auto source = static_cast<KDirModel *>(sourceModel())->indexForUrl(url);
            const auto index = mapFromSource(source).siblingAtColumn(Browser::SizeColumn);
            if (index.isValid()) emit dataChanged(index, index,
                {Qt::DisplayRole, Qt::ToolTipRole, Browser::LogicalSizeRole, Browser::CalculatedBytesRole,
                 Browser::DirectorySizeStateRole, Browser::MetadataErrorRole, Browser::RawValueRole});
        }
        if (sortColumn() == Browser::SizeColumn) invalidate();
    }
    void setGroup(Browser::Group value) {
        grouping = value;
        setCategorizedModel(value != Browser::GroupNone);
        invalidate();
    }
    void watchSource() {
        QObject::connect(static_cast<KDirModel *>(sourceModel())->dirLister(), &KDirLister::refreshItems, this,
            [this](const QList<QPair<KFileItem, KFileItem>> &items) {
                if (items.isEmpty()) return;
                for (const auto &pair : items) {
                    for (const auto &url : {pair.first.url(), pair.second.url()}) {
                        tagCache.remove(url); tagErrors.remove(url); tagCategories.remove(url);
                    }
                }
                restartTagReads();
                detailsRefresh = true; detailsTimer.start();
            });
        QObject::connect(sourceModel(), &QAbstractItemModel::rowsInserted, this, [this](const QModelIndex &parent, int first, int last) {
            enqueueTags(parent, first, last);
            detailsTimer.start();
        });
        QObject::connect(sourceModel(), &QAbstractItemModel::rowsAboutToBeRemoved, this, [this](const QModelIndex &parent, int first, int last) {
            forgetTags(parent, first, last);
            detailsTimer.start();
        });
        QObject::connect(sourceModel(), &QAbstractItemModel::modelReset, this, [this] {
            refreshTags(); nameCache.clear(); details->setUrls({}); detailsTimer.start();
        });
        QObject::connect(static_cast<KDirModel *>(sourceModel())->dirLister(), qOverload<>(&KDirLister::completed), this,
            [this] { detailsTimer.start(); });
        // DecorationRole updates can omit the roles list in KDirModel. Never
        // treat thumbnail delivery as a reason to reread filesystem metadata.
        // The host calls refreshTags after its real tag-writing job finishes.
    }
    void refreshTags() {
        tagCache.clear(); tagErrors.clear(); tagCategories.clear();
        restartTagReads();
    }
    void refreshFileDetails() { detailsRefresh = true; detailsTimer.start(); }
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override {
        if (!index.isValid()) return {};
        const auto item = mapToSource(index).siblingAtColumn(0).data(KDirModel::FileItemRole).value<KFileItem>();
        const auto url = item.url();
        if (role == Browser::DirectorySizeStateRole)
            return item.isDir() ? directorySizes.value(url).state : Browser::SizeNotCalculated;
        if (item.isDir() && (role == Browser::LogicalSizeRole || role == Browser::CalculatedBytesRole ||
            (role == Browser::RawValueRole && index.column() == Browser::SizeColumn))) {
            const auto found = directorySizes.constFind(url);
            return found != directorySizes.cend() && (found->state == Browser::SizeComplete ||
                (role == Browser::CalculatedBytesRole && found->state == Browser::SizeIncomplete)) ?
                QVariant::fromValue(qulonglong(found->bytes)) : QVariant();
        }
        if (item.isDir() && index.column() == Browser::SizeColumn && (role == Qt::DisplayRole || role == Qt::ToolTipRole)) {
            const auto size = directorySizes.value(url);
            if (role == Qt::ToolTipRole) return size.error.isEmpty() ?
                (size.state == Browser::SizeNotCalculated ? QStringLiteral("Use Calculate All Sizes to measure folder contents.") :
                 QStringLiteral("Logical file sizes; symlinks excluded, other filesystem devices skipped.")) : size.error;
            switch (size.state) {
            case Browser::SizeCalculating: return QStringLiteral("Calculating…");
            case Browser::SizeComplete: return KIO::convertSize(size.bytes);
            case Browser::SizeIncomplete: return QStringLiteral("≥ %1 (partial)").arg(KIO::convertSize(size.bytes));
            default: return QStringLiteral("—");
            }
        }
        if (role == TagsRole || (role == Browser::RawValueRole && index.column() == Browser::TagsColumn))
            return tagCache.contains(url) && !tagErrors.contains(url) ? QVariant(tagCache.value(url)) : QVariant();
        if (role == Browser::TagColorsRole) {
            if (!tagCache.contains(url) || tagErrors.contains(url)) return {};
            QVariantList colors;
            for (const auto &tag : tagCache.value(url)) colors.append(FileTags::tagColor(tag));
            return colors;
        }
        if (role == Browser::MetadataPendingRole && index.column() == Browser::TagsColumn) return queuedTags.contains(url);
        if (role == Browser::MetadataErrorRole) return index.column() == Browser::SizeColumn && item.isDir() ?
            directorySizes.value(url).error : tagErrors.value(url);
        if (index.column() == Browser::TagsColumn && (role == Qt::DisplayRole || role == Qt::ToolTipRole)) {
            if (tagErrors.contains(url)) return role == Qt::ToolTipRole ? tagErrors.value(url) : QStringLiteral("—");
            if (!url.isLocalFile() || item.isLink()) return role == Qt::ToolTipRole ?
                QStringLiteral("Unavailable: tags require a supported local inode.") : QStringLiteral("—");
            if (!tagCache.contains(url)) return QStringLiteral("Reading…");
            return tagCache.value(url).join(QStringLiteral(", "));
        }
        if (role == CutRole) return cutUrls.contains(index.data(KDirModel::FileItemRole).value<KFileItem>().url());
        if ((role == Qt::DisplayRole || role == Qt::ToolTipRole) && index.column() == KDirModel::Name) {
            const auto item = mapToSource(index).data(KDirModel::FileItemRole).value<KFileItem>();
            const auto name = item.text();
            if (role == Qt::ToolTipRole || showAllExtensions || item.isDir()) return name;
            const auto found = nameCache.constFind(item.url());
            if (found != nameCache.cend() && found->first == name) return found->second;
            // Filename-only MIME globs never read the file. Keep unknown suffixes
            // visible, and preserve EditRole, FileItemRole and URLs unchanged.
            const auto suffix = QMimeDatabase().suffixForFileName(name);
            const auto metadata = details->cached(item.url());
            const auto override = metadata && metadata->available() && metadata->extensionError.ok() ?
                metadata->extensionOverride : FileDetails::ExtensionOverride::Inherit;
            const auto display = FileDetails::displayName(name, item.isDir(), suffix, override, showAllExtensions);
            if (nameCache.size() >= 8192) nameCache.clear();
            nameCache.insert(item.url(), {name, display});
            return display;
        }
        if (role == Qt::DecorationRole && index.column() == KDirModel::Name && tintFolders) {
            const auto item = mapToSource(index).data(KDirModel::FileItemRole).value<KFileItem>();
            const auto tags = tagCache.value(item.url());
            const auto decoration = KDirSortFilterProxyModel::data(index, role);
            if (!item.isDir() || tags.isEmpty()) return decoration;
            const auto icon = decoration.value<QIcon>();
            if (icon.isNull()) return decoration;
            const auto color = FileTags::tagColor(tags.first());
            const auto key = QStringLiteral("%1:%2:%3:%4:%5").arg(icon.cacheKey()).arg(decorationSize.width())
                .arg(decorationSize.height()).arg(decorationScale).arg(color.rgba());
            if (const auto *cached = tintCache.object(key)) return *cached;
            auto pixmap = icon.pixmap(decorationSize, decorationScale);
            QPainter painter(&pixmap);
            painter.setCompositionMode(QPainter::CompositionMode_SourceAtop);
            auto tint = color; tint.setAlpha(170);
            painter.fillRect(pixmap.rect(), tint); painter.end();
            auto *tinted = new QIcon(pixmap);
            tintCache.insert(key, tinted);
            return *tinted;
        }
        if (role == Qt::DisplayRole && index.column() == KDirModel::Type)
            return itemType(mapToSource(index).data(KDirModel::FileItemRole).value<KFileItem>());
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
    bool subSortLessThan(const QModelIndex &left, const QModelIndex &right) const override {
        const int column = left.column();
        if (column != KDirModel::Type && column != KDirModel::Size && column != KDirModel::ModifiedTime && column < KDirModel::ColumnCount)
            return KDirSortFilterProxyModel::subSortLessThan(left, right);
        const auto a = left.data(KDirModel::FileItemRole).value<KFileItem>();
        const auto b = right.data(KDirModel::FileItemRole).value<KFileItem>();
        const bool ascending = sortOrder() == Qt::AscendingOrder;
        if (sortHiddenFilesLast() && a.isHidden() != b.isHidden()) return a.isHidden() != ascending;
        if (sortFoldersFirst() && a.isDir() != b.isDir()) return a.isDir() == ascending;
        // KDirSortFilterProxyModel's Type comparison calls mimetype(), which
        // can sniff file contents. The lister's delayed MIME types are enough
        // for ordering; native thumbnail delivery can refine them afterwards.
        int result = 0;
        if (column == KDirModel::ModifiedTime) {
            // Compare provider timestamps directly. KFileItem::time creates
            // localized QDateTime values for every comparison in a large sort.
            const auto ae = a.entry(), be = b.entry();
            const auto field = KIO::UDSEntry::UDS_MODIFICATION_TIME;
            const bool av = ae.contains(field), bv = be.contains(field);
            if (av != bv) return av == ascending;
            if (av) {
                const auto at = ae.numberValue(field), bt = be.numberValue(field);
                result = at < bt ? -1 : at > bt ? 1 : 0;
            }
        } else if (column >= KDirModel::ColumnCount || column == KDirModel::Size) {
            const auto raw = [this, column](const QModelIndex &index, const KFileItem &item) -> QVariant {
                if (column == Browser::TagsColumn) return tagCache.contains(item.url()) && !tagErrors.contains(item.url()) ?
                    QVariant(tagCache.value(item.url())) : QVariant();
                if (column == Browser::SizeColumn && item.isDir()) {
                    const auto found = directorySizes.constFind(item.url());
                    return found != directorySizes.cend() && found->state == Browser::SizeComplete ?
                        QVariant::fromValue(qulonglong(found->bytes)) : QVariant();
                }
                return index.data(Browser::RawValueRole);
            };
            const auto av = raw(left, a);
            const auto bv = raw(right, b);
            if (av.isValid() != bv.isValid()) return av.isValid() == ascending; // Unavailable always last.
            if (column == Browser::SizeColumn && av.isValid()) result = av.toULongLong() < bv.toULongLong() ? -1 : av.toULongLong() > bv.toULongLong() ? 1 : 0;
            else if (column == Browser::TagsColumn) result = collator.compare(av.toStringList().join(", "), bv.toStringList().join(", "));
            else if (av.isValid()) result = av.toDateTime() < bv.toDateTime() ? -1 : av.toDateTime() > bv.toDateTime() ? 1 : 0;
        } else {
            const auto aMime = a.currentMimeType();
            const auto bMime = b.currentMimeType();
            result = aMime.name() == bMime.name() ? 0 : collator.compare(itemType(a), itemType(b));
        }
        if (!result) result = collator.compare(a.text(), b.text());
        if (!result) result = QString::compare(a.text(), b.text(), Qt::CaseSensitive);
        if (!result) result = QString::compare(a.url().toString(), b.url().toString(), Qt::CaseSensitive);
        return result < 0;
    }
    int compareCategories(const QModelIndex &left, const QModelIndex &right) const override {
        return collator.compare(category(left).first, category(right).first);
    }
private:
    FileDetailsCache *details = nullptr;
    QTimer detailsTimer;
    bool detailsRefresh = false;
    void loadDetails() {
        QList<QUrl> urls;
        QQueue<QModelIndex> parents; parents.enqueue({});
        while (!parents.isEmpty() && urls.size() < FileDetails::maximumMetadataItems) {
            const auto parent = parents.dequeue();
            for (int row = 0; row < sourceModel()->rowCount(parent) && urls.size() < FileDetails::maximumMetadataItems; ++row) {
                const auto index = sourceModel()->index(row, 0, parent);
                const auto item = index.data(KDirModel::FileItemRole).value<KFileItem>();
                if (!item.isNull() && item.url().isLocalFile() && !item.isLink()) urls << item.url();
                if (sourceModel()->rowCount(index)) parents.enqueue(index);
            }
        }
        if (urls != details->urls()) details->setUrls(urls);
        else if (detailsRefresh) details->refresh();
        detailsRefresh = false;
    }
    QCollator collator;
    QHash<QUrl, QStringList> tagCache;
    QHash<QUrl, QString> tagErrors;
    QHash<QUrl, QPair<QString, QString>> tagCategories;
    mutable QHash<QUrl, QPair<QString, QString>> nameCache;
    mutable QCache<QString, QIcon> tintCache{512};
    bool showAllExtensions = false;
    bool tintFolders = false;
    QQueue<QUrl> tagQueue;
    QSet<QUrl> queuedTags;
    QTimer tagTimer;
    QTimer groupTimer;
    QPointer<QThread> tagWorker;
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
            if (item.isDir()) return {"0", "Folders"};
            const auto mime = item.currentMimeType().name();
            if (mime.startsWith("image/")) return {"1", "Images"};
            if (mime == "application/pdf") return {"2", "PDF Documents"};
            if (mime.startsWith("text/") || mime.contains("document") || mime.contains("spreadsheet") || mime.contains("presentation"))
                return {"3", "Documents"};
            if (mime.startsWith("audio/")) return {"4", "Music"};
            if (mime.startsWith("video/")) return {"5", "Movies"};
            if (mime.contains("zip") || mime.contains("tar") || mime.contains("compressed") || mime.contains("archive")) return {"6", "Archives"};
            if (mime.contains("executable") || mime == "application/x-desktop") return {"7", "Applications"};
            const QString kind = itemType(item);
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
            if (tagCache.value(item.url()).isEmpty()) return {"1", "No Tags"};
            // A file has one real index, including when it has several tags.
            // Group by the canonical full tag set rather than duplicate rows.
            return tagCategories.value(item.url());
        }
        default: return {};
        }
    }
    void restartTagReads() {
        ++tagRevision;
        if (tagWorker) tagWorker->requestInterruption();
        tagTimer.stop(); groupTimer.stop(); tagQueue.clear(); queuedTags.clear(); tintCache.clear();
        if (grouping == Browser::GroupTags || sortColumn() == Browser::TagsColumn) invalidate();
        enqueueTags({}, 0, sourceModel()->rowCount() - 1);
    }
    void enqueueTags(const QModelIndex &parent, int first, int last) {
        for (int row = first; row <= last; ++row) {
            const auto index = sourceModel()->index(row, 0, parent);
            const auto item = index.data(KDirModel::FileItemRole).value<KFileItem>();
            if (!item.isNull() && item.url().isLocalFile() && !item.isLink() &&
                !tagCache.contains(item.url()) && !queuedTags.contains(item.url())) {
                tagQueue.enqueue(item.url());
                queuedTags.insert(item.url());
            }
            // Only visit already loaded children; never fetch directories for tags.
            const int children = sourceModel()->rowCount(index);
            if (children) enqueueTags(index, 0, children - 1);
        }
        if (!readingTags && !tagQueue.isEmpty() && !tagTimer.isActive()) tagTimer.start();
    }
    void forgetTags(const QModelIndex &parent, int first, int last) {
        for (int row = first; row <= last; ++row) {
            const auto index = sourceModel()->index(row, 0, parent);
            const auto url = index.data(KDirModel::FileItemRole).value<KFileItem>().url();
            tagCache.remove(url); tagErrors.remove(url); tagCategories.remove(url); queuedTags.remove(url);
            const int children = sourceModel()->rowCount(index);
            if (children) forgetTags(index, 0, children - 1);
        }
    }
    void readTags() {
        if (readingTags) return;
        QList<QUrl> urls;
        while (!tagQueue.isEmpty() && urls.size() < 128) {
            const auto url = tagQueue.dequeue();
            if (!queuedTags.contains(url) || tagCache.contains(url)) continue;
            urls.append(url);
        }
        if (urls.isEmpty()) return;
        readingTags = true;
        const auto revision = tagRevision;
        auto result = std::make_shared<QHash<QUrl, QStringList>>();
        auto errors = std::make_shared<QHash<QUrl, QString>>();
        auto *worker = QThread::create([urls, result, errors] {
            // The native batch helper snapshots mount metadata once per
            // bounded batch, instead of reparsing mountinfo for every inode.
            if (!QThread::currentThread()->isInterruptionRequested())
                *result = FileTags::tagsForUrls(urls, errors.get());
        });
        tagWorker = worker;
        QObject::connect(worker, &QThread::finished, this, [this, revision, urls, result, errors] {
            readingTags = false;
            tagWorker = nullptr;
            if (revision == tagRevision) {
                QModelIndexList changed;
                QStringList observedTags;
                for (const auto &url : urls) {
                    queuedTags.remove(url);
                    const auto source = static_cast<KDirModel *>(sourceModel())->indexForUrl(url);
                    if (!source.isValid()) continue;
                    if (!result->contains(url)) continue;
                    const auto tags = result->value(url);
                    observedTags.append(tags);
                    tagCache.insert(url, tags);
                    if (errors->contains(url)) tagErrors.insert(url, errors->value(url));
                    auto ordered = tags;
                    std::sort(ordered.begin(), ordered.end(), [this](const auto &a, const auto &b) { return collator.compare(a, b) < 0; });
                    const auto names = ordered.join(", ");
                    tagCategories.insert(url, {"0" + names, names});
                    const auto mapped = mapFromSource(source);
                    if (mapped.isValid()) changed.append(mapped);
                }
                observedTags.removeDuplicates();
                FinderPreferences::registerObservedTags(observedTags);
                // Repaint only delivered names. A full-model signal per batch
                // makes large list views repeatedly lay out unchanged rows.
                std::sort(changed.begin(), changed.end(), [](const auto &a, const auto &b) {
                    if (a.parent() != b.parent()) return a.parent() < b.parent();
                    return a.row() < b.row();
                });
                for (int first = 0; first < changed.size();) {
                    int last = first;
                    while (last + 1 < changed.size() && changed[last + 1].parent() == changed[first].parent() &&
                           changed[last + 1].row() == changed[last].row() + 1) ++last;
                    emit dataChanged(changed[first], changed[last].siblingAtColumn(Browser::ListColumnCount - 1),
                        {TagsRole, Browser::TagColorsRole, Browser::RawValueRole, Browser::MetadataPendingRole,
                         Browser::MetadataErrorRole, Qt::DisplayRole, Qt::ToolTipRole, Qt::DecorationRole});
                    first = last + 1;
                }
                if ((grouping == Browser::GroupTags || sortColumn() == Browser::TagsColumn) && !groupTimer.isActive()) groupTimer.start();
            }
            if (!tagQueue.isEmpty()) tagTimer.start(0);
            else if (groupTimer.isActive()) groupTimer.start(0);
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
    QColor rule = FinderAppearance::highContrast() ? palette.color(QPalette::Mid) : palette.color(QPalette::Text);
    if (!FinderAppearance::highContrast()) rule.setAlpha(35);
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
        size.setHeight(qMax(sectionRows ? 20 : 24, qMax(option.decorationSize.height() + (sectionRows ? 4 : 6), option.fontMetrics.height() + (sectionRows ? 2 : 6))) +
                       (sectionRows && index.data(SectionStartRole).toBool() ? SectionHeight : 0));
        return size;
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const QStringList tags = index.column() == KDirModel::Name || index.column() == Browser::TagsColumn ? index.data(TagsRole).toStringList() : QStringList();
        QStyleOptionViewItem content(option);
        if (sectionRows && index.data(SectionStartRole).toBool()) content.rect.setTop(content.rect.top() + SectionHeight);
        // The tree paints one continuous rounded stripe/selection per row.
        // Keep native text, icons, focus and editing, without square cell fills.
        if (sectionRows) {
            content.features &= ~QStyleOptionViewItem::Alternate;
            content.backgroundBrush = Qt::NoBrush;
            content.palette.setColor(QPalette::AlternateBase, Qt::transparent);
            const bool selected = content.state & QStyle::State_Selected;
            content.state &= ~QStyle::State_Selected;
            if (selected) content.palette.setColor(QPalette::Text, FinderAppearance::color(FinderAppearance::SelectedText));
        }
        const int dotsWidth = tags.isEmpty() ? 0 : qMin(3, int(tags.size())) * 9 + 6;
        content.rect.adjust(0, 0, -dotsWidth, 0);
        painter->save();
        if (index.data(CutRole).toBool()) painter->setOpacity(painter->opacity() * 0.5);
        KFileItemDelegate::paint(painter, content, index);
        painter->restore();
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
    int spacing = 7;
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
            painter->setPen(Qt::NoPen); painter->setBrush(FinderAppearance::color(FinderAppearance::IconSelection));
            painter->drawRoundedRect(iconHighlight, 8, 8);
        }
        if (option.state & QStyle::State_HasFocus) {
            const QColor focus = FinderAppearance::highContrast() && selected
                ? FinderAppearance::palette().color(QPalette::HighlightedText) : FinderAppearance::color(FinderAppearance::Focus);
            painter->setPen(QPen(focus, FinderAppearance::highContrast() ? 2 : 1)); painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(iconHighlight, 8, 8);
        }
        const QVariant decoration = index.data(Qt::DecorationRole);
        painter->save();
        if (index.data(CutRole).toBool()) painter->setOpacity(painter->opacity() * 0.5);
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
        painter->restore();
        if (m_gallery && m_view->property("finderShowFilenames").isValid() && !m_view->property("finderShowFilenames").toBool()) {
            painter->restore(); return;
        }

        QFont font(option.font);
        font.setPixelSize(m_options->textSize);
        const auto entry = index.data(KDirModel::FileItemRole).value<KFileItem>();
        if (entry.isLink()) font.setItalic(true);
        const QFontMetrics metrics(font);
        const QStringList tags = index.data(TagsRole).toStringList();
        const int dotsWidth = tags.isEmpty() ? 0 : qMin(3, int(tags.size())) * 9 + 4;
        const int labelLeft = rightLabel ? iconRect.right() + 12 : option.rect.left();
        const int labelWidth = option.rect.right() - labelLeft;
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
            lines.append(metrics.elidedText(text.trimmed(), lineNumber == 1 ? Qt::ElideMiddle : Qt::ElideRight, width));
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
            painter->setPen(Qt::NoPen); painter->setBrush(FinderAppearance::color(FinderAppearance::IconLabelSelection));
            painter->drawRoundedRect(label, 3, 3);
        }
        painter->setPen(selected ? FinderAppearance::palette().color(QPalette::HighlightedText) :
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
        QFont font(option.font); font.setPixelSize(m_options->textSize);
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
        size.setHeight(qMax(24, option.fontMetrics.height() + 6));
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
    std::function<void(QMimeData *, const QPoint &)> prepareDrag;
    std::function<void()> dragFinished;
    void endDragState() { this->stopAutoScroll(); this->setState(QAbstractItemView::NoState); this->viewport()->update(); }
    bool keepRangeAnchor = false;
protected:
    void mousePressEvent(QMouseEvent *event) override {
        QScopedValueRollback<bool> handling(handlingPress, true);
        QScopedValueRollback<Qt::KeyboardModifiers> modifiers(pressModifiers, event->modifiers());
        const auto target = this->indexAt(event->position().toPoint());
        if (event->button() == Qt::LeftButton) dragOrigin = event->position().toPoint();
        const bool range = keepRangeAnchor && event->button() == Qt::LeftButton &&
            event->modifiers().testFlag(Qt::ShiftModifier) && rangeAnchor.isValid() && target.isValid() &&
            rangeAnchor.parent() == target.parent();
        if (keepRangeAnchor && !event->modifiers().testFlag(Qt::ShiftModifier) && target.isValid()) rangeAnchor = target;
        rangeTarget = range ? target : QModelIndex();
        rangeAdditive = event->modifiers() & (Qt::MetaModifier | Qt::ControlModifier);
        View::mousePressEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        View::mouseReleaseEvent(event);
        if (rangeTarget.isValid() && rangeAnchor.isValid() && rangeTarget.parent() == rangeAnchor.parent()) {
            // QColumnView replaces its child selection models after navigation.
            // Retain the clicked anchor in the column and use public selection
            // ranges after Qt has transferred the current selection model.
            const auto parent = rangeTarget.parent();
            QItemSelection selected(this->model()->index(qMin(rangeAnchor.row(), rangeTarget.row()), 0, parent),
                                    this->model()->index(qMax(rangeAnchor.row(), rangeTarget.row()), 0, parent));
            this->selectionModel()->select(selected, (rangeAdditive ? QItemSelectionModel::Select : QItemSelectionModel::ClearAndSelect) |
                                                     QItemSelectionModel::Rows);
        }
        rangeTarget = QModelIndex();
    }
    QItemSelectionModel::SelectionFlags selectionCommand(const QModelIndex &index, const QEvent *event = nullptr) const override {
        // currentChanged() re-enters this query with no event while Qt handles
        // a press. Use that press's modifiers so the range anchor survives,
        // including programmatic input whose global keyboard state is empty.
        if (!event && handlingPress && pressModifiers.testFlag(Qt::ShiftModifier))
            return QItemSelectionModel::SelectCurrent | QItemSelectionModel::Rows;
        if (event && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease ||
                      event->type() == QEvent::MouseButtonDblClick || event->type() == QEvent::MouseMove)) {
            const auto *mouse = static_cast<const QMouseEvent *>(event);
            if (mouse->modifiers().testFlag(Qt::MetaModifier)) {
                // Qt's Linux extended selection uses Control. Translate only
                // this selection query; activation retains the real modifiers.
                QMouseEvent command(mouse->type(), mouse->position(), mouse->globalPosition(), mouse->button(), mouse->buttons(),
                                    (mouse->modifiers() & ~Qt::MetaModifier) | Qt::ControlModifier, mouse->pointingDevice());
                return View::selectionCommand(index, &command);
            }
        }
        return View::selectionCommand(index, event);
    }
    void dropEvent(QDropEvent *event) override {
        auto index = this->indexAt(event->position().toPoint());
        if (!index.isValid()) index = this->rootIndex();
        if (handler) handler(event, index);
        else View::dropEvent(event);
        endDragState();
    }
    void dragEnterEvent(QDragEnterEvent *event) override {
        View::dragEnterEvent(event);
        if (event->mimeData()->hasUrls()) event->acceptProposedAction();
    }
    void dragMoveEvent(QDragMoveEvent *event) override {
        // Retain Qt's edge auto-scroll and hover/drop indication. The browser
        // filter tracks spring dwell, while native Drop remains KIO-owned.
        View::dragMoveEvent(event);
        if (!event->mimeData()->hasUrls()) return;
        if (event->modifiers().testFlag(Qt::MetaModifier))
            event->setDropAction(event->modifiers().testFlag(Qt::AltModifier) ? Qt::LinkAction : Qt::MoveAction);
        else if (event->modifiers().testFlag(Qt::AltModifier)) event->setDropAction(Qt::CopyAction);
        event->accept();
    }
    void startDrag(Qt::DropActions supported) override {
        // Qt's default Move result may remove rows at the source, and QListView
        // can consume an icon move before KIO sees it. Use native URL MIME and
        // QDrag, with the actual transfer owned exclusively by KIO::drop.
        const auto indexes = this->selectionModel()->selectedRows(0);
        if (indexes.isEmpty()) return;
        auto *mime = this->model()->mimeData(indexes);
        if (!mime) return;
        if (prepareDrag) prepareDrag(mime, dragOrigin);
        auto *drag = new QDrag(this); // The platform drag manager owns its lifetime.
        drag->setMimeData(mime);
        const auto icon = indexes.first().data(Qt::DecorationRole).template value<QIcon>();
        if (!icon.isNull()) {
            drag->setPixmap(icon.pixmap(this->iconSize()));
            drag->setHotSpot(QPoint(this->iconSize().width() / 2, this->iconSize().height() / 2));
        }
        const QPointer<QWidget> alive(this);
        drag->exec(supported, this->defaultDropAction());
        if (!alive) return;
        endDragState();
        if (dragFinished) dragFinished();
    }
private:
    QPoint dragOrigin;
    bool handlingPress = false;
    Qt::KeyboardModifiers pressModifiers = Qt::NoModifier;
    QPersistentModelIndex rangeAnchor;
    QPersistentModelIndex rangeTarget;
    bool rangeAdditive = false;
};

class FinderIconView : public DropView<KCategorizedView> {
public:
    explicit FinderIconView(QWidget *parent = nullptr) : DropView(parent) {
        setViewportMargins(10, 5, 10, 0);
    }
    std::function<void()> arrangedLayout;
    void place(const QModelIndex &index, const QPoint &point) { setPositionForIndex(point, index); }
    QPoint position(const QModelIndex &index) const { return rectForIndex(index).topLeft(); }
    void fitPositions(const QSize &size) { resizeContents(size.width(), size.height()); }
    void doItemsLayout() override {
        KCategorizedView::doItemsLayout();
        if (arrangedLayout) arrangedLayout();
    }
};

// Native Qt supplies the visible row parity and selection to its style.
// Adapt only the row panel primitive; text, branches and focus stay native.
class FinderListStyle final : public QProxyStyle {
public:
    FinderListStyle() : QProxyStyle(QStringLiteral("Fusion")) {}
    bool paintingRow = false;
    bool selected = false;
    int heading = 0;
    mutable bool alternate = false;
    void drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter, const QWidget *widget = nullptr) const override {
        if (element != PE_PanelItemViewRow) { QProxyStyle::drawPrimitive(element, option, painter, widget); return; }
        const auto *item = qstyleoption_cast<const QStyleOptionViewItem *>(option);
        const auto *tree = qobject_cast<const QTreeView *>(widget);
        if (!paintingRow || !item || !tree) return;
        alternate = item->features & QStyleOptionViewItem::Alternate;
        if (!selected && !alternate) return;
        painter->save(); painter->setClipRect(item->rect, Qt::IntersectClip);
        painter->setRenderHint(QPainter::Antialiasing); painter->setPen(Qt::NoPen);
        painter->setBrush(selected ? FinderAppearance::color(FinderAppearance::Selection) : FinderAppearance::palette().color(QPalette::AlternateBase));
        painter->drawRoundedRect(QRectF(10, item->rect.top() + heading, tree->viewport()->width() - 20, item->rect.height() - heading), 8, 8);
        painter->restore();
    }
};

class FinderListView : public DropView<QTreeView> {
public:
    explicit FinderListView(QWidget *parent = nullptr) : DropView(parent), rowStyle(new FinderListStyle) {
        rowStyle->setParent(this); setStyle(rowStyle);
    }
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
    void paintEvent(QPaintEvent *event) override {
        DropView::paintEvent(event);
        // Decorative stripes have no indexes: blank space remains native
        // empty space for selection, context menus, drops and accessibility.
        // Walk only visible rows, including expanded children, so partial
        // repaint and pixel scrolling keep the trailing pattern aligned.
        if (!alternatingRowColors()) return;
        QModelIndex index = QTreeView::indexAt(QPoint(0, 0));
        if (!index.isValid()) index = model() ? model()->index(0, 0, rootIndex()) : QModelIndex();
        int bottom = 0;
        while (index.isValid()) {
            const QRect row = QTreeView::visualRect(index);
            if (row.bottom() >= viewport()->height() - 1) return;
            bottom = qMax(bottom, row.bottom() + 1);
            const auto next = indexBelow(index);
            if (!next.isValid()) break;
            index = next;
        }
        const int rowHeight = qMax(20, qMax(iconSize().height() + 4, fontMetrics().height() + 2));
        // For a scrolled tree use the cached native parity of a visible row,
        // then count only the remaining viewport rows. Do not walk ancestors
        // or all rows before the viewport on every repaint.
        bool alternate = false;
        if (index.isValid()) {
            auto last = index;
            int steps = 0;
            while (last.isValid() && last != parityIndex && QTreeView::visualRect(last).bottom() >= 0) {
                ++steps; last = indexAbove(last);
            }
            alternate = last.isValid() ? (parityAlternate ^ (steps % 2) ^ 1) : ((index.row() + 1) % 2);
        }
        QPainter painter(viewport()); painter.setClipRegion(event->region());
        for (int y = bottom; y < viewport()->height(); y += rowHeight, alternate = !alternate)
            if (alternate) paintStripe(&painter, QRect(0, y, viewport()->width(), rowHeight), palette().color(QPalette::AlternateBase));
    }
    void mousePressEvent(QMouseEvent *event) override {
        if (onHeading(event->position().toPoint())) { event->accept(); return; }
        DropView::mousePressEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (onHeading(event->position().toPoint())) {
            // A drag begun on a file may end over the heading. Finish the view
            // gesture while keeping the non-file heading out of activation.
            if (state() == DragSelectingState || state() == DraggingState) setState(NoState);
            event->accept(); return;
        }
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
        QStyleOptionViewItem content(option);
        const bool selected = selectionModel() && selectionModel()->isSelected(index);
        rowStyle->paintingRow = true;
        rowStyle->selected = selected;
        rowStyle->heading = index.data(SectionStartRole).toBool() ? SectionHeight : 0;
        content.features &= ~QStyleOptionViewItem::Alternate;
        content.state &= ~QStyle::State_Selected;
        content.backgroundBrush = Qt::NoBrush;
        content.palette.setColor(QPalette::AlternateBase, Qt::transparent);
        if (selected) content.palette.setColor(QPalette::Text, FinderAppearance::color(FinderAppearance::SelectedText));
        QTreeView::drawRow(painter, content, index);
        rowStyle->paintingRow = false;
        parityIndex = index;
        parityAlternate = rowStyle->alternate;
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
    static void paintStripe(QPainter *painter, const QRect &rect, QColor color) {
        painter->save(); painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen); painter->setBrush(color);
        painter->drawRoundedRect(QRectF(rect).adjusted(10, 0, -10, 0), 8, 8); painter->restore();
    }
    FinderListStyle *rowStyle;
    mutable QPersistentModelIndex parityIndex;
    mutable bool parityAlternate = false;
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
        column->keepRangeAnchor = true;
        initializeColumn(column);
        column->setModel(model());
        column->setRootIndex(index);
        column->setIconSize(QSize(18, 18));
        column->setUniformItemSizes(true);
        column->setSpacing(0);
        column->handler = handler;
        column->prepareDrag = prepareDrag;
        column->dragFinished = dragFinished;
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
    QStackedWidget *stack = nullptr;
    QHash<QWidget *, DirectoryState *> directoryStates;
    QSet<QUrl> loadedDirectories;
    QSet<QUrl> loadingDirectories;
    QHash<QUrl, QString> listingErrors;
    void updateDirectoryStates() {
        if (!stack) return;
        for (auto it = directoryStates.begin(); it != directoryStates.end(); ++it) {
            auto *surface = it.key(); auto *state = it.value();
            auto *view = qobject_cast<QAbstractItemView *>(surface->parentWidget());
            QModelIndex root;
            QUrl location = rootUrl;
            if (surface == columnHost) {
                const auto current = item(columnSelection->currentIndex());
                if (!current.isDir()) { state->showState({}, {}, {}); continue; }
                location = current.url(); root = proxy->mapFromSource(model->indexForUrl(location));
            } else if (view) {
                root = view->rootIndex();
                const auto entry = item(root);
                if (!entry.isNull() && entry.isDir()) location = entry.url();
            }
            state->setGeometry(surface->rect()); state->raise();
            if (proxy->rowCount(root) > 0 || location.isEmpty()) { state->showState({}, {}, {}); continue; }
            if (listingErrors.contains(location)) {
                state->showState(QStringLiteral("This location can’t be opened"), listingErrors.value(location).left(400), "error");
            } else if (loadingDirectories.contains(location) || !loadedDirectories.contains(location)) {
                state->showState(QStringLiteral("Loading…"), QStringLiteral("Getting the items in this location."), "loading");
            } else if (!proxy->filterRegularExpression().pattern().isEmpty()) {
                state->showState(QStringLiteral("No matching items"), QStringLiteral("Try a different filter or clear it to see all items."), "filter");
            } else if (!model->dirLister()->itemsForDir(location, KDirLister::AllItems).isEmpty()) {
                state->showState(QStringLiteral("No visible items"), QStringLiteral("This location contains hidden items. Use Show Hidden Files in the View menu to see them."), "folder");
            } else if (location.scheme() == "recentlyused") {
                state->showState(QStringLiteral("No recent items"), QStringLiteral("Files and folders you use will appear here when the recent-items provider records them."), "recents");
            } else if (location.scheme() == "trash") {
                state->showState(QStringLiteral("Trash is empty"), QStringLiteral("Items you move to Trash will appear here."), "folder");
            } else {
                state->showState(QStringLiteral("This folder is empty"), QStringLiteral("There are no items in this location."), "folder");
            }
        }
    }
    void installDirectoryState(QWidget *surface) {
        if (directoryStates.contains(surface)) return;
        directoryStates.insert(surface, new DirectoryState(surface));
        surface->installEventFilter(q);
        QObject::connect(surface, &QObject::destroyed, q, [this, surface] { directoryStates.remove(surface); });
    }
    FinderIconView *icons;
    FinderListView *list;
    ColumnsView *columns;
    DropView<QListView> *strip;
    QWidget *gallery;
    QWidget *galleryHost;
    QWidget *columnHost = nullptr;
    QWidget *sidePreviewHost;
    QSplitter *previewSplitter;
    int sidePreviewWidth = 280;
    QFileSystemWatcher directoryWatcher;
    QTimer directoryRefreshTimer;
    QSet<QString> changedDirectories;
    QTimer thumbnailTimer;
    QPointer<KIO::PreviewJob> thumbnailJob;
    QSet<QUrl> thumbnailRequests;
    quint64 thumbnailRevision = 0;
    QPointer<QThread> sizeWorker;
    QTimer sizeProgressTimer;
    std::shared_ptr<SizeScan> sizeScan;
    quint64 sizeRevision = 0;
    int sizeRoots = 0;
    void invalidateSizes() {
        ++sizeRevision;
        if (sizeScan) sizeScan->cancelRequested = true;
        if (sizeWorker) sizeWorker->requestInterruption();
        const auto urls = proxy->directorySizes.keys();
        proxy->directorySizes.clear();
        proxy->sizesChanged(urls);
    }
    ~Private() {
        if (sizeWorker) sizeWorker->requestInterruption();
        // QWidget deletes its children after Browser::d has been destroyed.
        // Detach callbacks that refer to this private state first.
        for (auto *surface : directoryStates.keys()) QObject::disconnect(surface, nullptr, q, nullptr);
    }
    QUrl rootUrl;
    QUrl currentUrl;
    QHash<QUrl, QHash<QUrl, QPoint>> arrangements;
    bool gridSnap = false;
    bool applyingArrangement = false;
    bool sortingArrangement = false;
    bool arrangementQueued = false;
    const QByteArray dragToken = QUuid::createUuid().toByteArray();
    QTimer springTimer;
    QTimer springLeaveTimer;
    bool springEnabled = true;
    bool springActive = false;
    bool springNavigating = false;
    QUrl springHover;
    QPointer<QAbstractItemView> springView;
    QPointer<const QMimeData> springMime;
    QMetaObject::Connection springMimeDestroyed;
    QUrl springOriginRoot;
    QUrl springOriginCurrent;
    QList<QUrl> springOriginSelection;
    QByteArray springOriginState;
    QUrl springRestoreColumns;
    Mode currentMode = Icons;
    QList<QUrl> pendingSelection;
    bool restoringGallerySelection = false;
    bool initialGallerySelectionPending = false;
    bool gallerySelectionQueued = false;
    QUrl pendingRename;
    quint64 folderCreationGeneration = 0;
    bool creatingFolder = false;
    bool changing = false;
    bool previews = true;
    bool sidePreview = false;
    IconOptions iconOptions;
    struct ViewOptions {
        int size = 64;
        int sort = KDirModel::Name;
        bool ascending = true;
        Group group = GroupNone;
        bool previews = true;
        bool sidePreview = false;
        IconOptions icons;
        int horizontal = 0;
        int vertical = 0;
    };
    std::array<ViewOptions, 4> viewOptions;
    bool pendingScrollRestore = false;
    void restoreScroll() {
        const auto options = viewOptions[currentMode];
        q->activeView()->horizontalScrollBar()->setValue(options.horizontal);
        q->activeView()->verticalScrollBar()->setValue(options.vertical);
        scheduleThumbnails();
    }
    void saveModeOptions() {
        auto *view = q->activeView();
        viewOptions[currentMode] = {q->iconSize(), q->sortColumn(), q->sortAscending(), q->group(), previews, sidePreview, iconOptions,
                                    view->horizontalScrollBar()->value(), view->verticalScrollBar()->value()};
    }
    void restoreModeOptions() {
        const auto options = viewOptions[currentMode];
        q->setIconSize(options.size);
        q->setSort(options.sort, options.ascending);
        q->setGroup(options.group);
        q->setGridSpacing(options.icons.spacing);
        q->setTextSize(options.icons.textSize);
        q->setLabelPosition(options.icons.label);
        q->setIconPreviewsShown(options.previews);
        q->setPreviewShown(options.sidePreview);
    }

    void syncDirectoryWatches(const QUrl &started = {}) {
        // Observe only loaded local directories. Qt's inotify directory signal
        // retains DELETE events which KDirWatch 6.24 can discard after its
        // whole-second mtime comparison. Native KIO still owns the listing diff.
        QSet<QString> wanted;
        auto urls = model->dirLister()->directories();
        if (!started.isEmpty()) urls.prepend(started);
        for (const auto &url : urls)
            if (url.isLocalFile() && wanted.size() < 256) wanted.insert(url.toLocalFile());
        const auto watched = directoryWatcher.directories();
        for (const auto &path : watched)
            if (!wanted.contains(path)) { directoryWatcher.removePath(path); changedDirectories.remove(path); }
        for (const auto &path : wanted)
            if (!watched.contains(path)) directoryWatcher.addPath(path);
    }

    explicit Private(Browser *owner) : q(owner) {
        directoryRefreshTimer.setSingleShot(true);
        directoryRefreshTimer.setInterval(60);
        QObject::connect(&directoryWatcher, &QFileSystemWatcher::directoryChanged, q, [this](const QString &path) {
            changedDirectories.insert(path);
            if (!directoryRefreshTimer.isActive()) directoryRefreshTimer.start();
        });
        QObject::connect(&directoryRefreshTimer, &QTimer::timeout, q, [this] {
            const auto changed = std::exchange(changedDirectories, {});
            const auto loaded = model->dirLister()->directories();
            for (const auto &path : changed) {
                const auto url = QUrl::fromLocalFile(path);
                if (loaded.contains(url)) model->dirLister()->updateDirectory(url);
            }
        });
        sizeProgressTimer.setInterval(100);
        QObject::connect(&sizeProgressTimer, &QTimer::timeout, q, [this] {
            if (sizeScan) emit q->sizeCalculationProgress(sizeScan->entries.load(), sizeScan->completed.load(), sizeRoots);
        });
        thumbnailTimer.setSingleShot(true);
        thumbnailTimer.setInterval(120);
        QObject::connect(&thumbnailTimer, &QTimer::timeout, q, [this] { updateVisibleThumbnails(); });
        springTimer.setSingleShot(true); springTimer.setInterval(650);
        springLeaveTimer.setSingleShot(true); springLeaveTimer.setInterval(0);
        QObject::connect(&springTimer, &QTimer::timeout, q, [this] { springOpen(); });
        QObject::connect(&springLeaveTimer, &QTimer::timeout, q, [this] { finishSpring(false); });
    }

    static QUrl normalized(QUrl url) {
        return url.adjusted(QUrl::NormalizePathSegments | QUrl::StripTrailingSlash);
    }
    bool manualIcons() const {
        return currentMode == Icons && viewOptions[Icons].sort == -1 && proxy->grouping == GroupNone;
    }
    QPoint boundedPosition(QPoint point) const {
        point.setX(qBound(0, point.x(), 1000000));
        point.setY(qBound(0, point.y(), 1000000));
        return point;
    }
    QPoint snapped(QPoint point) const {
        const auto grid = icons->gridSize().expandedTo(QSize(1, 1));
        point = boundedPosition(point);
        return QPoint(qRound(double(point.x()) / grid.width()) * grid.width(),
                      qRound(double(point.y()) / grid.height()) * grid.height());
    }
    static quint64 cellKey(int x, int y) { return (quint64(quint32(y)) << 32) | quint32(x); }
    void occupy(QSet<quint64> &cells, const QPoint &point) const {
        const auto grid = icons->gridSize().expandedTo(QSize(1, 1));
        const QRect rect(point, grid);
        for (int y = rect.top() / grid.height(); y <= rect.bottom() / grid.height(); ++y)
            for (int x = rect.left() / grid.width(); x <= rect.right() / grid.width(); ++x)
                cells.insert(cellKey(x, y));
    }
    QPoint vacant(QSet<quint64> &cells, QPoint start = {}) const {
        const auto grid = icons->gridSize().expandedTo(QSize(1, 1));
        const int columns = qMax(1, icons->viewport()->width() / grid.width());
        int x = start.x() / grid.width(), y = start.y() / grid.height();
        while (cells.contains(cellKey(x, y))) { if (++x >= columns) { x = 0; ++y; } }
        const QPoint result(x * grid.width(), y * grid.height());
        occupy(cells, result);
        return result;
    }
    void applyArrangement() {
        if (!manualIcons() || applyingArrangement || rootUrl.isEmpty()) return;
        QScopedValueRollback<bool> guard(applyingArrangement, true);
        auto &positions = arrangements[normalized(rootUrl)];
        QSet<quint64> occupied;
        // Reserve hidden/filtered positions too: new files must not silently
        // occupy the location of a file that is temporarily out of view.
        for (const auto &point : positions) occupy(occupied, point);
        QSize extent = icons->viewport()->size();
        QPoint nextVacancy;
        bool added = false;
        for (int row = 0; row < proxy->rowCount(); ++row) {
            const auto index = proxy->index(row, 0);
            const auto url = normalized(item(index).url());
            if (!positions.contains(url)) {
                nextVacancy = vacant(occupied, nextVacancy);
                positions.insert(url, nextVacancy); added = true;
            }
            const auto point = positions.value(url);
            icons->place(index, point);
            extent = extent.expandedTo(QSize(point.x() + icons->gridSize().width(), point.y() + icons->gridSize().height()));
        }
        icons->fitPositions(extent);
        if (added) emit q->iconArrangementChanged();
    }
    void scheduleArrangement() {
        if (arrangementQueued || !manualIcons() || applyingArrangement) return;
        arrangementQueued = true;
        QTimer::singleShot(0, q, [this] {
            arrangementQueued = false;
            if (manualIcons()) icons->doItemsLayout();
        });
    }
    void updateArrangementMode() {
        if (sortingArrangement) return;
        icons->setMovement(manualIcons() ? (gridSnap ? QListView::Snap : QListView::Free) : QListView::Static);
        // QListView movement/view-mode setters change drag/drop flags too.
        // Sorted Icons remain a native transfer destination/source.
        icons->setDragDropMode(QAbstractItemView::DragDrop);
        icons->viewport()->setAcceptDrops(true);
        icons->setResizeMode(manualIcons() ? QListView::Fixed : QListView::Adjust);
        icons->doItemsLayout();
        scheduleArrangement();
    }
    void writeDrag(QAbstractItemView *view, QMimeData *mime, const QPoint &origin) {
        QJsonObject points;
        for (const auto &index : view->selectionModel()->selectedRows(0)) {
            const QPoint point = view == icons ? icons->position(index) : view->visualRect(index).topLeft() +
                QPoint(view->horizontalScrollBar()->value(), view->verticalScrollBar()->value());
            points.insert(normalized(item(index).url()).toString(QUrl::FullyEncoded), QJsonArray{point.x(), point.y()});
        }
        const QPoint contentOrigin = origin + QPoint(view->horizontalScrollBar()->value(), view->verticalScrollBar()->value());
        mime->setData("application/x-finder-icon-arrangement", QJsonDocument(QJsonObject{
            {"token", QString::fromLatin1(dragToken)}, {"directory", normalized(rootUrl).toString(QUrl::FullyEncoded)},
            {"origin", QJsonArray{contentOrigin.x(), contentOrigin.y()}}, {"positions", points}}).toJson(QJsonDocument::Compact));
    }
    bool arrangeDrop(QDropEvent *event, const QModelIndex &target) {
        if (!manualIcons() || event->modifiers() != Qt::NoModifier || item(target).isDir()) return false;
        const auto payload = QJsonDocument::fromJson(event->mimeData()->data("application/x-finder-icon-arrangement")).object();
        if (payload.value("token").toString().toLatin1() != dragToken ||
            normalized(QUrl(payload.value("directory").toString())) != normalized(rootUrl)) return false;
        const auto points = payload.value("positions").toObject();
        const auto urls = KUrlMimeData::urlsFromMimeData(event->mimeData(), KUrlMimeData::PreferLocalUrls);
        if (urls.isEmpty() || points.size() != urls.size()) return false;
        for (const auto &url : urls) {
            if (normalized(parentUrl(url)) != normalized(rootUrl) ||
                !points.contains(normalized(url).toString(QUrl::FullyEncoded))) return false;
            const auto point = points.value(normalized(url).toString(QUrl::FullyEncoded)).toArray();
            if (point.size() != 2 || !point[0].isDouble() || !point[1].isDouble()) return false;
        }
        const auto origin = payload.value("origin").toArray();
        if (origin.size() != 2) return false;
        const QPoint dropPoint = event->position().toPoint() + QPoint(icons->horizontalScrollBar()->value(), icons->verticalScrollBar()->value());
        const QPoint delta = dropPoint - QPoint(origin[0].toInt(), origin[1].toInt());
        auto &positions = arrangements[normalized(rootUrl)];
        for (const auto &url : urls) {
            const auto point = points.value(normalized(url).toString(QUrl::FullyEncoded)).toArray();
            QPoint moved = boundedPosition(QPoint(point[0].toInt(), point[1].toInt()) + delta);
            positions.insert(normalized(url), gridSnap ? snapped(moved) : moved);
        }
        applyArrangement();
        event->setDropAction(Qt::MoveAction); event->accept();
        emit q->iconArrangementChanged();
        return true;
    }
    void beginSpring(QAbstractItemView *view, const QMimeData *mime) {
        springLeaveTimer.stop();
        if (springActive || !springEnabled || !mime || !mime->hasUrls()) return;
        springActive = true;
        springOriginRoot = rootUrl; springOriginCurrent = currentUrl;
        springOriginSelection = q->selectedUrls(); springOriginState = q->saveViewState();
        springView = view; springMime = mime;
        // Destruction also handles drags dropped into another widget/app. A
        // deferred leave handles cancellation of external native drags whose
        // platform MIME object may remain alive until the next drag.
        springMimeDestroyed = QObject::connect(mime, &QObject::destroyed, q, [this, mime] {
            if (springMime.isNull() || springMime.data() == mime) finishSpring(false);
        });
        emit q->springLoadingChanged(true);
    }
    void hoverSpring(QAbstractItemView *view, QDragMoveEvent *event) {
        beginSpring(view, event->mimeData());
        if (!springActive) return;
        const auto entry = item(view->indexAt(event->position().toPoint()));
        QUrl target = entry.isDir() ? normalized(FileTarget::url(entry)) : QUrl();
        const auto sources = KUrlMimeData::urlsFromMimeData(event->mimeData(), KUrlMimeData::PreferLocalUrls);
        for (const auto &source : sources) {
            const auto url = normalized(source);
            if (url == target || url.isParentOf(target)) { target = QUrl(); break; }
        }
        if (target == normalized(rootUrl)) target = QUrl();
        if (springHover == target && springView == view) return;
        springTimer.stop(); springView = view; springHover = target;
        if (!target.isEmpty()) springTimer.start();
    }
    void springOpen() {
        if (!springActive || springHover.isEmpty() || !springView || !springMime || !springView->isVisible()) return;
        const QUrl target = springHover; springHover = QUrl();
        QScopedValueRollback<bool> guard(springNavigating, true);
        // KFileItem has already identified the hovered directory. Listing is
        // asynchronous; there is no QFileInfo/network stat in the hover path.
        q->navigateValidated(target);
    }
    void finishSpring(bool commit) {
        springTimer.stop(); springLeaveTimer.stop(); springHover = QUrl();
        if (!springActive) return;
        const bool changed = normalized(rootUrl) != normalized(springOriginRoot);
        springActive = false;
        QObject::disconnect(springMimeDestroyed);
        springView = nullptr; springMime = nullptr;
        if (changed && !commit) {
            QScopedValueRollback<bool> guard(springNavigating, true);
            q->navigateValidated(springOriginRoot);
            q->restoreViewState(springOriginState);
            if (currentMode == Columns && springOriginCurrent != springOriginRoot) springRestoreColumns = springOriginCurrent;
            q->selectUrls(springOriginSelection);
            if (!springRestoreColumns.isEmpty()) {
                model->expandToUrl(springOriginCurrent);
                for (const auto &url : springOriginSelection) model->expandToUrl(url);
            }
        } else if (!commit) q->selectUrls(springOriginSelection);
        if (changed && commit) emit q->urlChanged(currentUrl);
        emit q->springLoadingChanged(false);
    }

    void layoutIcons() {
        const auto options = viewOptions[Icons].icons;
        QFont font = icons->font(); font.setPixelSize(options.textSize);
        const int lineHeight = QFontMetrics(font).height();
        const int size = icons->iconSize().width();
        const int width = options.label == LabelRight ? size + 144 + options.spacing : size + 40 + options.spacing;
        const int height = options.label == LabelRight ? qMax(size + 16, 2 * lineHeight + 20) : size + 24 + 2 * lineHeight;
        icons->setGridSize(QSize(width, height));
        icons->viewport()->update();
        // Gallery keeps its own thumbnail size independently of Icons zoom, with
        // modest extra space spread across the visible horizontal filmstrip.
        const int available = strip->viewport()->width();
        const int gallerySize = strip->iconSize().width();
        QFont galleryFont = strip->font();
        galleryFont.setPixelSize(viewOptions[Gallery].icons.textSize);
        const int galleryLineHeight = QFontMetrics(galleryFont).height();
        const int minimumWidth = gallerySize + 32;
        const int columns = qMax(1, available / minimumWidth);
        const int stripWidth = qBound(minimumWidth, available / columns, minimumWidth + 16);
        const QSize stripGrid(stripWidth, gallerySize + 20 + (q->galleryFilenamesShown() ? 2 * galleryLineHeight : 0));
        if (strip->gridSize() != stripGrid) strip->setGridSize(stripGrid);
        strip->setFixedHeight(stripGrid.height() + 16);
        strip->viewport()->update();
        scheduleArrangement();
    }

    void updatePreviewVisibility() {
        const bool shown = sidePreview && currentMode != Gallery && currentMode != Columns;
        sidePreviewHost->setVisible(shown);
        if (shown) previewSplitter->setSizes({qMax(240, previewSplitter->width() - sidePreviewWidth), sidePreviewWidth});
    }

    void resetThumbnailRequests() {
        ++thumbnailRevision;
        thumbnailRequests.clear();
        scheduleThumbnails();
    }

    void scheduleThumbnails() {
        if (previews && !thumbnailTimer.isActive())
            thumbnailTimer.start();
    }

    void updateVisibleThumbnails() {
        if (thumbnailJob || !previews || !q->isVisible()) return;
        QList<QAbstractItemView *> views;
        if (currentMode == Columns) {
            for (auto *column : columns->findChildren<QListView *>())
                if (column->isVisible()) views.append(column);
        } else views.append(q->activeView());
        KFileItemList visible;
        QSet<QUrl> sampled;
        // Sample viewport cells rather than walking every model row. Half-cell
        // steps also cover partially visible tiles and categorized headings.
        for (auto *view : views) {
            const QRect viewport = view->viewport()->rect();
            if (!view->isVisible() || viewport.isEmpty()) continue;
            const bool tiles = currentMode == Icons || currentMode == Gallery;
            const QSize grid = tiles ? static_cast<QListView *>(view)->gridSize() : QSize();
            const int dx = tiles ? qMax(16, grid.width() / 3) : viewport.width();
            const int dy = tiles ? qMax(16, grid.height() / 3) :
                qMax(8, qMax(view->fontMetrics().height() + 6, view->iconSize().height() + 6) / 2);
            for (int y = 1; y < viewport.height() && visible.size() < 16; y += dy) {
                for (int x = 1; x < viewport.width() && visible.size() < 16; x += dx) {
                    const auto index = view->indexAt(QPoint(x, y));
                    if (!index.isValid()) continue;
                    const auto entry = item(index);
                    if (entry.isNull() || thumbnailRequests.contains(entry.url()) || sampled.contains(entry.url())) continue;
                    sampled.insert(entry.url()); visible.append(entry);
                }
            }
            if (visible.size() >= 16) break;
        }
        if (visible.isEmpty()) return; // A stationary view has no timer loop.
        for (const auto &entry : visible) thumbnailRequests.insert(entry.url());
        const auto revision = thumbnailRevision;
        const auto extent = q->activeView()->iconSize();
        const auto plugins = KIO::PreviewJob::defaultPlugins();
        auto *operation = KIO::filePreview(visible, extent, &plugins);
        operation->setDevicePixelRatio(q->activeView()->devicePixelRatioF());
        thumbnailJob = operation;
        QObject::connect(operation, &KIO::PreviewJob::gotPreview, q,
            [this, revision](const KFileItem &entry, const QPixmap &pixmap) {
                if (revision != thumbnailRevision || !previews) return;
                const auto index = model->indexForUrl(entry.url());
                if (index.isValid()) model->setData(index, QIcon(pixmap), Qt::DecorationRole);
            });
        QObject::connect(operation, &KJob::result, q, [this] {
            thumbnailJob = nullptr;
            scheduleThumbnails();
        });
        // Native jobs own cache, worker limits and failure handling. Let this
        // small batch finish on view/navigation changes and ignore old results.
        // In KF 6.24 killing a PreviewJob can leave FilePreviewJob's timeout
        // using an already deleted transfer job; no private ABI workaround.
    }

    KFileItem item(const QModelIndex &index) const {
        return index.isValid() ? model->itemForIndex(proxy->mapToSource(index.siblingAtColumn(0))) : KFileItem();
    }

    QItemSelectionModel *activeSelection() const {
        return currentMode == Columns ? columnSelection : selection;
    }

    QModelIndexList selectedNames() const {
        QModelIndexList names;
        // selectedIndexes() expands every metadata column in a list selection.
        // Walk selection ranges and create only name indexes, including the
        // name-only ranges emitted by QListView/QColumnView.
        for (const auto &range : activeSelection()->selection()) {
            if (range.left() > KDirModel::Name || range.right() < KDirModel::Name) continue;
            // QColumnView retains ancestor highlights as the active column
            // changes. File actions apply only to this column's selection.
            if (currentMode == Columns && range.parent() != columnSelection->currentIndex().parent()) continue;
            for (int row = range.top(); row <= range.bottom(); ++row) names.append(proxy->index(row, KDirModel::Name, range.parent()));
        }
        return names;
    }

    void count() { updateDirectoryStates(); emit q->countChanged(q->itemCount()); }

    void selected() {
        if (changing) return;
        QTimer::singleShot(0, q, [this] { updateDirectoryStates(); });
        auto items = q->selectedItems();
        if (currentMode == Columns) {
            const auto current = item(columnSelection->currentIndex());
            QUrl active = rootUrl;
            if (!current.isNull() && FileTarget::url(current) == current.url()) active = current.isDir() ? current.url() : parentUrl(current.url());
            if (active != currentUrl) {
                currentUrl = active;
                if (springActive || springNavigating || !springRestoreColumns.isEmpty()) emit q->springUrlChanged(currentUrl);
                else emit q->urlChanged(currentUrl);
                count();
            }
        }
        emit q->selectionChanged(items);
        if (items.size() == 1 && items.first().isFile()) emit q->previewRequested(items.first().url());
        else emit q->previewRequested({});
    }

    void activate(const QModelIndex &index, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        const auto entry = FileTarget::item(item(index));
        if (entry.isNull()) return;
        if (entry.isDir()) {
            if (modifiers & (Qt::MetaModifier | Qt::ControlModifier | Qt::AltModifier)) {
                emit q->folderOpenRequested(entry.url(), modifiers);
                return;
            }
            if (currentMode == Columns && item(index).url() != entry.url()) {
                if (q->property("finderHostFolderPolicy").toBool()) emit q->folderOpenRequested(entry.url(), modifiers);
                else q->navigate(entry.url());
            } else if (currentMode == Columns) {
                columnSelection->setCurrentIndex(index.siblingAtColumn(0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                if (proxy->canFetchMore(index)) proxy->fetchMore(index);
                columns->scrollTo(index);
                selected();
            } else if (q->property("finderHostFolderPolicy").toBool()) emit q->folderOpenRequested(entry.url(), modifiers);
            else q->navigate(entry.url());
        } else emit q->openRequested(entry.url());
    }

    QModelIndex indexForTarget(const QUrl &url) const {
        const auto direct = model->indexForUrl(url);
        if (direct.isValid()) return direct;
        for (const auto &folder : {currentUrl, rootUrl}) {
            for (const auto &entry : model->dirLister()->itemsForDir(folder, KDirLister::AllItems))
                if (FileTarget::url(entry) == url) return model->indexForUrl(entry.url());
        }
        return {};
    }

    void scheduleGallerySelection() {
        if (gallerySelectionQueued) return;
        gallerySelectionQueued = true;
        QTimer::singleShot(0, q, [this] {
            gallerySelectionQueued = false;
            if (!initialGallerySelectionPending || changing || springActive || springNavigating
                || currentMode != Gallery || loadingDirectories.contains(rootUrl)) return;
            if (!pendingSelection.isEmpty()) { applySelection(); return; }
            if (!selection->selectedRows(0).isEmpty()) { initialGallerySelectionPending = false; return; }
            const auto first = proxy->index(0, 0);
            if (!first.isValid()) return;
            initialGallerySelectionPending = false;
            selection->setCurrentIndex(first, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            strip->scrollTo(first);
        });
    }

    void applySelection() {
        if (pendingSelection.isEmpty()) return;
        QItemSelection selected;
        QModelIndex current;
        bool complete = true;
        for (const auto &url : pendingSelection) {
            const auto index = proxy->mapFromSource(indexForTarget(url));
            if (!index.isValid()) { complete = false; continue; }
            selected.select(index, index);
            if (!current.isValid()) current = index;
        }
        if (!current.isValid()) return;
        // KDirLister can announce copied siblings in separate batches. Keep
        // the complete request until every URL arrives; clearing it after the
        // first index silently drops the other files from a paste selection.
        if (complete) pendingSelection.clear();
        activeSelection()->select(selected, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        activeSelection()->setCurrentIndex(current, QItemSelectionModel::NoUpdate);
        q->activeView()->scrollTo(current);
        if (complete) springRestoreColumns = QUrl();
        if (complete && !pendingRename.isEmpty() && FileTarget::url(item(current)) == pendingRename) {
            pendingRename = QUrl();
            QTimer::singleShot(0, q, [this, index = QPersistentModelIndex(current)] {
                if (!index.isValid() || !q->isVisible()) return;
                auto *view = q->activeView();
                if (currentMode == Columns) {
                    for (auto *column : columns->findChildren<QListView *>())
                        if (column->isVisible() && column->rootIndex() == index.parent()) { view = column; break; }
                }
                view->setFocus(); view->edit(index);
            });
        }
    }

    void job(KIO::Job *job, const QString &message) {
        job->setUiDelegate(new FinderJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, q->window()));
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
        const auto entry = FileTarget::item(item(index));
        if (entry.isNull() || name == entry.name()) return;
        if (!validName(name)) { emit q->errorOccurred("Enter a valid filename without a slash."); return; }
        const auto confirmed = Browser::confirmedRenameName(entry, name, q->window());
        if (confirmed.isEmpty() || confirmed == entry.name()) return;
        const auto destination = childUrl(parentUrl(entry.url()), confirmed);
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
        const auto contextItems = q->selectedItems();
        const bool hasSelection = !contextItems.isEmpty();
        QMenu menu(q);
        auto *open = menu.addAction(QStringLiteral("Open"), [this, contextItems] {
            // Capture items, not model indexes: opening a folder can replace
            // the model before the remaining selected files are activated.
            for (const auto &entry : contextItems) {
                if (entry.isDir()) emit q->folderOpenRequested(entry.url(), Qt::NoModifier);
                else emit q->openRequested(entry.url());
            }
        });
        open->setEnabled(hasSelection);
        KFileItemActions nativeActions;
        nativeActions.setParentWidget(q->window());
        if (hasSelection) {
            nativeActions.setItemListProperties(KFileItemListProperties(contextItems));
            nativeActions.insertOpenWithActionsTo(nullptr, &menu, {});
            nativeActions.addActionsTo(&menu);
        }
        auto *look = menu.addAction(QStringLiteral("Quick Look"), [this] {
            const auto urls = q->selectedUrls();
            if (!urls.isEmpty()) emit q->quickLookRequested(urls);
        });
        look->setEnabled(hasSelection);
        menu.addSeparator();
        menu.addAction(QStringLiteral("New Folder"), q, &Browser::newFolder);
        menu.addAction(QStringLiteral("New Folder with Selection"), q, &Browser::newFolderWithSelection)
            ->setEnabled(q->canNewFolderWithSelection());
        auto *rename = menu.addAction(QStringLiteral("Rename"), q, &Browser::renameSelected);
        rename->setEnabled(hasSelection);
        menu.addSeparator();
        auto *copy = menu.addAction(QStringLiteral("Copy"), [this] { q->copySelected(false); });
        auto *cut = menu.addAction(QStringLiteral("Cut"), [this] { q->copySelected(true); });
        copy->setEnabled(hasSelection); cut->setEnabled(hasSelection);
        const auto *clipboard = QApplication::clipboard()->mimeData();
        menu.addAction(QStringLiteral("Paste"), [this] { q->paste(); })->setEnabled(clipboard && clipboard->hasUrls());
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
        emit q->contextMenuRequested(&menu, contextItems);
        menu.exec(view->viewport()->mapToGlobal(point));
    }

    void prepare(QAbstractItemView *view, bool childColumn = false) {
        if (childColumn) view->setFont(columns->font());
        view->setSelectionMode(QAbstractItemView::ExtendedSelection);
        view->setSelectionBehavior(QAbstractItemView::SelectRows);
        view->setDragEnabled(true);
        view->setAcceptDrops(true);
        view->setDropIndicatorShown(true);
        view->setDragDropMode(QAbstractItemView::DragDrop);
        view->viewport()->setAcceptDrops(true);
        view->setDefaultDropAction(Qt::CopyAction);
        const auto dragPrepare = [this, view](QMimeData *mime, const QPoint &point) { writeDrag(view, mime, point); };
        const auto dragDone = [this] { finishSpring(false); };
        if (view == icons) { icons->prepareDrag = dragPrepare; icons->dragFinished = dragDone; }
        else if (view == list) { list->prepareDrag = dragPrepare; list->dragFinished = dragDone; }
        else if (view == columns) { columns->prepareDrag = dragPrepare; columns->dragFinished = dragDone; }
        else { auto *native = static_cast<DropView<QListView> *>(view); native->prepareDrag = dragPrepare; native->dragFinished = dragDone; }
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
        installDirectoryState(view->viewport());
        QObject::connect(view->verticalScrollBar(), &QScrollBar::valueChanged, q, [this] { scheduleThumbnails(); });
        QObject::connect(view->horizontalScrollBar(), &QScrollBar::valueChanged, q, [this] { scheduleThumbnails(); });
        // QColumnView already forwards its child columns' activation signals.
        if (!childColumn) QObject::connect(view, &QAbstractItemView::doubleClicked, q, [this](const auto &index) { activate(index); });
        QObject::connect(view, &QWidget::customContextMenuRequested, q, [this, view](const auto &point) { context(view, point); });
    }
};

Browser::Browser(QWidget *parent) : QWidget(parent), d(std::make_unique<Private>(this)) {
    setObjectName(QStringLiteral("finderBrowser"));
    d->model = new MetadataDirModel(this);
    d->model->setDropsAllowed(KDirModel::DropOnDirectory);
    d->model->dirLister()->setMainWindow(window());
    d->model->dirLister()->setAutoErrorHandlingEnabled(false);
    d->model->dirLister()->setDelayedMimeTypes(true);
    connect(d->model, &QAbstractItemModel::rowsAboutToBeRemoved, this,
            [this](const QModelIndex &parent, int first, int last) {
        // KDirLister Reload clears the root rows before started(). Preserve
        // the existing Gallery targets while their native indexes are valid.
        if (parent.isValid() || first != 0 || last != d->model->rowCount() - 1
            || d->currentMode != Gallery || d->changing || d->springActive
            || d->springNavigating || !d->pendingSelection.isEmpty()) return;
        auto urls = selectedUrls();
        if (urls.isEmpty()) return;
        const auto primary = primaryUrl();
        if (urls.removeOne(primary)) urls.prepend(primary);
        d->pendingSelection = urls;
        d->restoringGallerySelection = true;
    });
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
    d->previewSplitter = new QSplitter(Qt::Horizontal, this);
    d->previewSplitter->setObjectName(QStringLiteral("finderPreviewSplitter"));
    d->previewSplitter->setChildrenCollapsible(false);
    d->previewSplitter->setHandleWidth(5);
    d->stack = new QStackedWidget(d->previewSplitter); d->stack->setMinimumWidth(240);
    auto *layout = new QHBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0); layout->addWidget(d->previewSplitter, 1);
    d->sidePreviewHost = new QWidget(d->previewSplitter); d->sidePreviewHost->setObjectName("finderSidePreview");
    d->sidePreviewHost->setMinimumWidth(180); d->previewSplitter->setCollapsible(1, true);
    d->previewSplitter->setStretchFactor(0, 1); d->previewSplitter->setStretchFactor(1, 0);
    d->sidePreviewWidth = qBound(180, QSettings().value("View/previewWidth", 280).toInt(), 900);
    d->sidePreviewHost->hide();
    connect(d->previewSplitter, &QSplitter::splitterMoved, this, [this] {
        const int width = d->previewSplitter->sizes().value(1);
        if (!width) { d->sidePreview = false; d->sidePreviewHost->hide(); d->selected(); return; }
        d->sidePreviewWidth = width;
        QSettings().setValue("View/previewWidth", width);
    });
    d->icons = new FinderIconView(d->stack); d->icons->setObjectName("finderIconView");
    d->icons->setViewMode(QListView::IconMode); d->icons->setResizeMode(QListView::Adjust);
    d->icons->setMovement(QListView::Static); d->icons->setIconSize({64, 64});
    d->icons->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    d->icons->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    d->icons->setWordWrap(true); d->icons->setUniformItemSizes(true); d->icons->setCategoryDrawer(new FinderCategoryDrawer(d->icons));
    d->icons->setCategorySpacing(8); d->icons->setCollapsibleBlocks(false);
    d->icons->arrangedLayout = [this] { d->applyArrangement(); };
    d->list = new FinderListView(d->stack); d->list->setObjectName("finderListView");
    d->list->setRootIsDecorated(true); d->list->setAlternatingRowColors(true); d->list->setUniformRowHeights(true);
    d->list->setIconSize({16, 16}); d->list->setSortingEnabled(true);
    d->columns = new ColumnsView(d->stack); d->columns->setObjectName("finderColumnView");
    d->columns->setIconSize({18, 18});
    d->viewOptions[List].size = 16; d->viewOptions[Columns].size = 18;
    d->columns->setColumnWidths({220, 220, 220, 220, 220}); d->columns->setResizeGripsVisible(true);
    d->columnHost = new QWidget; d->columnHost->setObjectName("finderColumnPreview");
    d->columnHost->setMinimumWidth(250); d->columns->setPreviewWidget(d->columnHost);
    d->installDirectoryState(d->columnHost);
    d->columns->prepare = [this](auto *view) { d->prepare(view, true); };
    d->gallery = new QWidget(d->stack); d->gallery->setObjectName("finderGalleryView");
    auto *galleryLayout = new QVBoxLayout(d->gallery); galleryLayout->setContentsMargins(0, 0, 0, 0); galleryLayout->setSpacing(0);
    d->galleryHost = new QWidget(d->gallery); d->galleryHost->setObjectName("finderGalleryPreview");
    galleryLayout->addWidget(d->galleryHost, 1);
    d->installDirectoryState(d->galleryHost);
    d->strip = new DropView<QListView>(d->gallery); d->strip->setObjectName("finderGalleryStrip");
    d->strip->setViewMode(QListView::IconMode); d->strip->setFlow(QListView::LeftToRight); d->strip->setWrapping(false);
    d->strip->setMovement(QListView::Static); d->strip->setIconSize({64, 64});
    d->strip->setResizeMode(QListView::Adjust); d->strip->setWordWrap(true); d->strip->setUniformItemSizes(true);
    d->strip->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    galleryLayout->addWidget(d->strip);
    d->layoutIcons();
    const auto dropped = [this](QDropEvent *event, const QModelIndex &index) {
        if (d->arrangeDrop(event, index)) { d->finishSpring(true); return; }
        const auto entry = d->item(index);
        drop(event, entry.isDir() ? FileTarget::url(entry) : (index.isValid() ? parentUrl(FileTarget::url(entry)) :
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
    d->list->setColumnWidth(KDirModel::Name, 340); d->list->setColumnWidth(KDirModel::Size, 96);
    d->list->setColumnWidth(KDirModel::ModifiedTime, 180); d->list->setColumnWidth(KDirModel::Type, 122);
    for (int column : {KDirModel::Permissions, KDirModel::Owner, KDirModel::Group}) d->list->hideColumn(column);
    for (int column = DateCreatedColumn; column < ListColumnCount; ++column) {
        d->list->setColumnWidth(column, column == TagsColumn ? 180 : 185);
        d->list->hideColumn(column);
    }
    d->list->header()->moveSection(d->list->header()->visualIndex(KDirModel::Type), 3);
    d->list->header()->moveSection(d->list->header()->visualIndex(KDirModel::ModifiedTime), 1);
    d->list->header()->setFixedHeight(28);
    d->list->header()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    d->list->header()->setStretchLastSection(false);
    d->list->header()->setSectionsMovable(true);
    d->list->header()->setSectionResizeMode(QHeaderView::Interactive);
    d->list->header()->setMinimumSectionSize(48);
    // Explicit fitting samples a bounded number of loaded rows. Qt's default
    // precision is 1000; -1 would visit the entire folder on each fit.
    d->list->header()->setResizeContentsPrecision(200);
    connect(d->list->header(), &QHeaderView::sectionHandleDoubleClicked, this, &Browser::autoSizeListColumns);
    connect(d->list, &QTreeView::expanded, this, [this] { d->scheduleThumbnails(); });
    connect(d->list, &QTreeView::collapsed, this, [this] { d->scheduleThumbnails(); });
    d->list->header()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(d->list->header(), &QWidget::customContextMenuRequested, this, [this](const QPoint &point) {
        QMenu menu(d->list->header());
        const int section = d->list->header()->logicalIndexAt(point);
        menu.addAction(QStringLiteral("Resize Column to Fit"), this, [this, section] { autoSizeListColumns(section); })
            ->setEnabled(section >= 0);
        menu.addAction(QStringLiteral("Resize All Columns to Fit"), this, [this] { autoSizeListColumns(); });
        menu.addSeparator();
        for (int column = 0; column < listColumnCount(); ++column) {
            auto *action = menu.addAction(d->proxy->headerData(column, Qt::Horizontal).toString());
            action->setCheckable(true); action->setChecked(!d->list->isColumnHidden(column));
            action->setEnabled(column != KDirModel::Name);
            connect(action, &QAction::toggled, this, [this, column](bool visible) { setListColumnVisible(column, visible); });
        }
        menu.exec(d->list->header()->mapToGlobal(point));
    });
    d->stack->addWidget(d->icons); d->stack->addWidget(d->list); d->stack->addWidget(d->columns); d->stack->addWidget(d->gallery);
    setFocusProxy(d->icons);
    d->gallery->setFocusProxy(d->strip);
    // A private KDE profile need not have PreviewSettings populated. Native
    // default thumbnailers include installed image/PDF workers and their cache.
    d->resetThumbnailRequests();
    connect(d->selection, &QItemSelectionModel::selectionChanged, this, [this] { if (d->currentMode != Columns) d->selected(); });
    connect(d->selection, &QItemSelectionModel::currentChanged, this, [this] { if (d->currentMode != Columns) d->selected(); });
    connect(d->columnSelection, &QItemSelectionModel::selectionChanged, this, [this] { if (d->currentMode == Columns) d->selected(); });
    connect(d->columnSelection, &QItemSelectionModel::currentChanged, this, [this] { if (d->currentMode == Columns) d->selected(); });
    connect(d->columns, &QColumnView::updatePreviewWidget, this, [this](const auto &index) {
        if (d->currentMode == Columns) {
            const auto item = d->item(index); emit previewRequested(item.isFile() ? FileTarget::url(item) : QUrl());
        }
    });
    connect(d->model->dirLister(), &KDirLister::started, this, [this](const QUrl &url) { d->syncDirectoryWatches(url); });
    connect(d->model->dirLister(), qOverload<>(&KDirLister::completed), this, [this] { d->syncDirectoryWatches(); });
    connect(d->model->dirLister(), &KDirLister::jobError, this, [this](KIO::Job *job) {
        const auto *listing = qobject_cast<KIO::SimpleJob *>(job);
        const auto failed = listing ? listing->url() : d->currentUrl;
        d->listingErrors.insert(failed, FinderAlert::plain(job->errorString()));
        d->loadingDirectories.remove(failed); d->updateDirectoryStates();
        if (job->error() != KIO::ERR_USER_CANCELED && job->error() != KJob::KilledJobError)
            FinderAlert::showError(window(), tr("This location can’t be opened"), job->errorString());
        emit errorOccurred(job->errorString());
    });
    connect(d->model->dirLister(), &KDirLister::listingDirCompleted, this, [this](const QUrl &url) {
        d->loadingDirectories.remove(url); d->loadedDirectories.insert(url); d->updateDirectoryStates();
    });
    connect(d->model->dirLister(), &KDirLister::itemsDeleted, this, [this] { d->invalidateSizes(); });
    connect(d->model, &QAbstractItemModel::modelReset, this, [this] { d->invalidateSizes(); });
    connect(d->model->dirLister(), &KDirLister::newItems, this, [this](const KFileItemList &items) {
        if (!items.isEmpty()) d->invalidateSizes();
        for (const auto &item : items) d->thumbnailRequests.remove(item.url());
        d->scheduleThumbnails();
    });
    connect(d->model->dirLister(), &KDirLister::refreshItems, this, [this](const QList<QPair<KFileItem, KFileItem>> &items) {
        if (!items.isEmpty()) { d->invalidateSizes(); d->resetThumbnailRequests(); }
        bool changed = false;
        for (const auto &pair : items) {
            const auto oldUrl = Private::normalized(pair.first.url()), newUrl = Private::normalized(pair.second.url());
            if (oldUrl == newUrl) continue;
            auto folder = d->arrangements.find(Private::normalized(parentUrl(oldUrl)));
            if (folder == d->arrangements.end() || !folder->contains(oldUrl)) continue;
            const QPoint point = folder->take(oldUrl);
            d->arrangements[Private::normalized(parentUrl(newUrl))].insert(newUrl, point); changed = true;
        }
        if (changed) { d->scheduleArrangement(); emit iconArrangementChanged(); }
    });
    connect(d->model, &KDirModel::expand, this, [this](const QModelIndex &source) {
        if (d->springRestoreColumns.isEmpty() || mode() != Columns) return;
        const auto index = d->proxy->mapFromSource(source);
        if (index.isValid()) { d->columns->scrollTo(index); d->applySelection(); }
    });
    connect(d->model->dirLister(), &KDirLister::jobError, this, [this](KIO::Job *) {
        if (d->springActive) {
            QTimer::singleShot(0, this, &Browser::cancelSpringLoading);
        }
    });
    connect(d->model, &QAbstractItemModel::rowsInserted, this, [this] { d->count(); d->applySelection(); });
    connect(d->proxy, &QAbstractItemModel::rowsInserted, this, [this] { d->scheduleArrangement(); });
    connect(d->proxy, &QAbstractItemModel::rowsInserted, this, [this] { d->scheduleGallerySelection(); });
    connect(d->proxy, &QAbstractItemModel::rowsRemoved, this, [this] { d->scheduleArrangement(); });
    connect(d->proxy, &QAbstractItemModel::layoutChanged, this, [this] { d->scheduleArrangement(); });
    connect(d->proxy, &QAbstractItemModel::modelReset, this, [this] { d->scheduleArrangement(); });
    connect(d->model, &QAbstractItemModel::rowsRemoved, this, [this] { d->count(); });
    connect(d->model, &QAbstractItemModel::modelReset, this, [this] { d->count(); });
    connect(d->model->dirLister(), &KDirLister::completed, this, [this] {
        d->loadingDirectories.clear(); d->loadedDirectories.insert(d->rootUrl);
        d->count(); d->applySelection();
        if (d->restoringGallerySelection) {
            d->pendingSelection.clear(); d->restoringGallerySelection = false;
        }
        d->scheduleGallerySelection();
        d->scheduleArrangement();
        if (d->pendingScrollRestore) {
            d->pendingScrollRestore = false;
            QTimer::singleShot(0, this, [this] { d->restoreScroll(); });
        }
        emit statusMessage({});
    });
    connect(d->model->dirLister(), &KDirLister::started, this, [this](const QUrl &url) {
        d->listingErrors.remove(url); d->loadedDirectories.remove(url); d->loadingDirectories.insert(url);
        d->updateDirectoryStates(); emit statusMessage("Loading…");
    });
    connect(d->model->dirLister(), qOverload<>(&KDirLister::canceled), this, [this] {
        for (const auto &url : std::as_const(d->loadingDirectories))
            if (!d->listingErrors.contains(url)) d->listingErrors.insert(url, QStringLiteral("Loading was cancelled. Open this location again to retry."));
        d->loadingDirectories.clear(); d->updateDirectoryStates();
    });
    connect(QApplication::clipboard(), &QClipboard::dataChanged, this, [this] {
        d->proxy->cutUrls.clear();
        const auto *mime = QApplication::clipboard()->mimeData();
        if (mime && mime->data("application/x-kde-cutselection") == "1") {
            for (const auto &url : KUrlMimeData::urlsFromMimeData(mime, KUrlMimeData::PreferLocalUrls)) d->proxy->cutUrls.insert(url);
        }
        // Cut state is a cheap presentation flag, never a new metadata or
        // thumbnail request for every row in the directory.
        for (auto *view : QList<QAbstractItemView *>{d->icons, d->list, d->columns, d->strip}) view->viewport()->update();
    });
    const auto updateAppearance = [this] {
        d->list->setAlternatingRowColors(FinderAppearance::alternatingRowColors());
        setStyleSheet(QStringLiteral(
            "#finderBrowser QAbstractItemView{background:palette(base);color:palette(text);border:0;}"
            "#finderColumnView::item:selected{background:%1;color:%2;}"
            "#finderBrowser QColumnView::item{height:24px;}"
            "#finderGalleryPreview,#finderColumnPreview,#finderSidePreview{background:palette(base);}"
            "#finderBrowser QHeaderView::section{background:palette(base);color:palette(placeholder-text);border:0;border-bottom:1px solid palette(alternate-base);padding:3px 8px;text-align:left;}"
            "#finderBrowser QHeaderView::section:first{color:palette(text);}")
            .arg(FinderAppearance::color(FinderAppearance::Selection).name(),
                 FinderAppearance::color(FinderAppearance::SelectedText).name()));
        for (auto *view : QList<QAbstractItemView *>{d->icons, d->list, d->columns, d->strip}) view->viewport()->update();
    };
    if (auto *appearance = FinderAppearance::instance())
        connect(appearance, &FinderAppearance::changed, this, updateAppearance);
    updateAppearance();
    d->icons->setAccessibleName(tr("Files in icon view"));
    d->list->setAccessibleName(tr("Files in list view"));
    d->columns->setAccessibleName(tr("Files in column view"));
    d->strip->setAccessibleName(tr("Files in gallery view"));
}

Browser::~Browser() {
    // QWidget destroys children after d has been released. KCoreDirLister
    // emits canceled while it is destroyed, and models/views may emit reset
    // or selection signals. Detach their callbacks while private state lives.
    for (auto *child : findChildren<QObject *>())
        QObject::disconnect(child, nullptr, this, nullptr);
}
QUrl Browser::url() const { return d->currentUrl; }
Browser::Mode Browser::mode() const { return d->currentMode; }
int Browser::iconSize() const { return activeView()->iconSize().width(); }
Browser::Group Browser::group() const { return d->proxy->grouping; }
int Browser::gridSpacing() const { return d->iconOptions.spacing; }
int Browser::textSize() const { return d->iconOptions.textSize; }
Browser::LabelPosition Browser::labelPosition() const { return d->iconOptions.label; }
bool Browser::iconPreviewsShown() const { return d->previews; }
bool Browser::previewShown() const { return d->sidePreview; }
int Browser::previewWidth() const { return d->sidePreviewWidth; }
QByteArray Browser::saveViewState() const {
    d->saveModeOptions();
    QJsonArray modes;
    for (const auto &options : d->viewOptions) {
        modes.append(QJsonObject{{"iconSize", options.size}, {"sort", options.sort}, {"ascending", options.ascending},
            {"group", int(options.group)}, {"previews", options.previews}, {"preview", options.sidePreview},
            {"spacing", options.icons.spacing}, {"textSize", options.icons.textSize}, {"labels", int(options.icons.label)},
            {"horizontal", options.horizontal}, {"vertical", options.vertical}});
    }
    QJsonArray columns;
    for (int width : d->columns->columnWidths()) columns.append(width);
    QJsonArray visible;
    for (int column : visibleListColumns()) visible.append(column);
    QJsonObject positions;
    const auto savedPositions = d->arrangements.value(Private::normalized(d->rootUrl));
    for (auto it = savedPositions.cbegin(); it != savedPositions.cend(); ++it)
        positions.insert(it.key().toString(QUrl::FullyEncoded), QJsonArray{it->x(), it->y()});
    return QJsonDocument(QJsonObject{{"version", 1}, {"mode", int(mode())}, {"modes", modes},
        {"snapToGrid", snapToGrid()}, {"positionsUrl", Private::normalized(d->rootUrl).toString(QUrl::FullyEncoded)}, {"iconPositions", positions},
        {"galleryFilenames", galleryFilenamesShown()}, {"previewWidth", previewWidth()}, {"listHeader", QString::fromLatin1(d->list->header()->saveState().toBase64())},
        {"columnWidths", columns}, {"listVisibleColumns", visible}}).toJson(QJsonDocument::Compact);
}
void Browser::restoreViewState(const QByteArray &state) {
    const auto object = QJsonDocument::fromJson(state).object();
    const auto modes = object.value("modes").toArray();
    if (object.value("version").toInt() != 1 || modes.size() != 4) return;
    for (int number = 0; number < modes.size(); ++number) {
        const auto saved = modes[number].toObject();
        auto &options = d->viewOptions[number];
        options.size = number == Columns ? 18 : number == List ? qBound(16, saved.value("iconSize").toInt(18), 32) :
            qBound(32, saved.value("iconSize").toInt(64), 256);
        options.sort = qBound(number == Icons ? -1 : 0, saved.value("sort").toInt(), int(ListColumnCount) - 1);
        options.ascending = saved.value("ascending").toBool(true);
        options.group = Group(qBound(0, saved.value("group").toInt(), int(GroupTags)));
        options.previews = saved.value("previews").toBool(true);
        options.sidePreview = saved.value("preview").toBool();
        options.icons.spacing = qBound(0, saved.value("spacing").toInt(16), 128);
        options.icons.textSize = qBound(9, saved.value("textSize").toInt(13), 20);
        options.icons.label = LabelPosition(qBound(0, saved.value("labels").toInt(), int(LabelRight)));
        options.horizontal = qMax(0, saved.value("horizontal").toInt());
        options.vertical = qMax(0, saved.value("vertical").toInt());
    }
    const bool previousChanging = d->changing;
    d->changing = true;
    d->gridSnap = object.value("snapToGrid").toBool(false);
    const QUrl positionsUrl = Private::normalized(QUrl(object.value("positionsUrl").toString()));
    if (!positionsUrl.isEmpty() && (d->rootUrl.isEmpty() || positionsUrl == Private::normalized(d->rootUrl))) {
        QHash<QUrl, QPoint> positions;
        const auto savedPositions = object.value("iconPositions").toObject();
        for (auto it = savedPositions.begin(); it != savedPositions.end() && positions.size() < 100000; ++it) {
            const QUrl url = Private::normalized(QUrl(it.key()));
            const auto point = it.value().toArray();
            if (url.isValid() && Private::normalized(parentUrl(url)) == positionsUrl && point.size() == 2 &&
                point[0].isDouble() && point[1].isDouble())
                positions.insert(url, d->boundedPosition(QPoint(point[0].toInt(), point[1].toInt())));
        }
        d->arrangements.insert(positionsUrl, positions);
    }
    d->currentMode = Mode(qBound(0, object.value("mode").toInt(), int(Gallery)));
    d->stack->setCurrentIndex(int(mode())); setFocusProxy(activeView());
    d->restoreModeOptions();
    d->layoutIcons();
    d->proxy->decorationSize = activeView()->iconSize(); d->proxy->decorationScale = activeView()->devicePixelRatioF();
    setGalleryFilenamesShown(object.value("galleryFilenames").toBool(true));
    setPreviewWidth(object.value("previewWidth").toInt(280));
    const auto header = QByteArray::fromBase64(object.value("listHeader").toString().toLatin1());
    if (!header.isEmpty()) d->list->header()->restoreState(header);
    if (object.contains("listVisibleColumns")) {
        QSet<int> visible{NameColumn};
        for (const auto &value : object.value("listVisibleColumns").toArray()) visible.insert(value.toInt());
        for (int column = 1; column < listColumnCount(); ++column) d->list->setColumnHidden(column, !visible.contains(column));
    } else {
        // Legacy states know only native columns. New metadata starts optional.
        for (int column = DateCreatedColumn; column < ListColumnCount; ++column) d->list->hideColumn(column);
    }
    d->list->showColumn(NameColumn);
    // Header state includes a sort indicator and may sort the proxy as a side
    // effect. Per-mode saved sort remains authoritative after geometry restore.
    setSort(d->viewOptions[mode()].sort, d->viewOptions[mode()].ascending);
    d->updateArrangementMode();
    QList<int> widths;
    for (const auto &width : object.value("columnWidths").toArray()) widths.append(qBound(80, width.toInt(220), 900));
    if (!widths.isEmpty()) d->columns->setColumnWidths(widths);
    d->resetThumbnailRequests();
    d->pendingScrollRestore = true;
    QTimer::singleShot(0, this, [this] { d->restoreScroll(); });
    d->changing = previousChanging;
    d->selected();
    emit listColumnsChanged();
}
bool Browser::canCompressSelection() const {
    const auto urls = selectedUrls();
    return !urls.isEmpty() && !QStandardPaths::findExecutable(QStringLiteral("ark")).isEmpty() &&
        std::all_of(urls.cbegin(), urls.cend(), [](const QUrl &url) { return url.isLocalFile(); });
}
bool Browser::canNewFolderWithSelection() const {
    const auto items = selectedItems();
    if (items.isEmpty()) return false;
    const auto parent = parentUrl(items.first().url());
    // Capability queries stay in memory. Permission/conflict decisions belong
    // to the actual KIO jobs rather than synchronous parent-directory stats.
    return parent.isValid() && std::all_of(items.cbegin(), items.cend(), [&parent](const KFileItem &item) {
        return parentUrl(item.url()) == parent && KProtocolManager::supportsMoving(item.url()) &&
            KProtocolManager::supportsDeleting(item.url());
    });
}
int Browser::sortColumn() const { return mode() == Icons && d->viewOptions[Icons].sort == -1 ? -1 : d->proxy->sortColumn(); }
bool Browser::snapToGrid() const { return d->gridSnap; }
bool Browser::canArrangeIcons() const { return d->manualIcons(); }
QPoint Browser::iconPosition(const QUrl &url) const {
    return d->arrangements.value(Private::normalized(d->rootUrl)).value(Private::normalized(url), QPoint(-1, -1));
}
QMimeData *Browser::createDragMimeData(const QPoint &origin) const {
    auto *view = activeView();
    auto *mime = view->model()->mimeData(view->selectionModel()->selectedRows(0));
    if (mime) d->writeDrag(view, mime, origin);
    return mime;
}
bool Browser::springLoadingActive() const { return d->springActive; }
bool Browser::sortAscending() const { return d->proxy->sortOrder() == Qt::AscendingOrder; }
int Browser::listColumnCount() const { return d->proxy->columnCount(); }
QString Browser::listColumnTitle(int column) const {
    return column >= 0 && column < listColumnCount() ? d->proxy->headerData(column, Qt::Horizontal).toString() : QString();
}
bool Browser::listColumnVisible(int column) const {
    return column >= 0 && column < listColumnCount() && !d->list->isColumnHidden(column);
}
QList<int> Browser::visibleListColumns() const {
    QList<int> columns;
    for (int visual = 0; visual < listColumnCount(); ++visual) {
        const int logical = d->list->header()->logicalIndex(visual);
        if (listColumnVisible(logical)) columns.append(logical);
    }
    return columns;
}
void Browser::setListColumnVisible(int column, bool visible) {
    if (column < 0 || column >= listColumnCount() || column == NameColumn || listColumnVisible(column) == visible) return;
    d->list->setColumnHidden(column, !visible);
    emit listColumnsChanged();
}
QVariant Browser::metadata(const QUrl &url, int role) const {
    const auto source = d->model->indexForUrl(url);
    return source.isValid() ? d->proxy->mapFromSource(source).data(role) : QVariant();
}
void Browser::refreshMetadata() { d->invalidateSizes(); static_cast<MetadataDirModel *>(d->model)->refreshMetadata(); d->proxy->refreshTags(); }
bool Browser::sizeCalculationRunning() const { return !d->sizeWorker.isNull(); }
void Browser::cancelSizeCalculation() {
    if (d->sizeScan) d->sizeScan->cancelRequested = true;
    if (d->sizeWorker) d->sizeWorker->requestInterruption();
}
void Browser::calculateAllSizes() { calculateAllSizes(SizeCalculationLimits{}); }
void Browser::calculateAllSizes(SizeCalculationLimits limits) {
    if (sizeCalculationRunning()) return;
    limits.maxEntries = qBound<quint64>(1, limits.maxEntries, 250000);
    limits.maxDepth = qBound(0, limits.maxDepth, 64);
    limits.maxMilliseconds = qBound(1, limits.maxMilliseconds, 15000);
    QList<QUrl> urls;
    QList<QUrl> changed;
    const auto parent = d->currentUrl == d->rootUrl ? QModelIndex() : d->model->indexForUrl(d->currentUrl);
    const int rows = d->model->rowCount(parent);
    bool capped = false;
    for (int row = 0; row < rows; ++row) {
        const auto item = d->model->itemForIndex(d->model->index(row, 0, parent));
        if (!item.isDir()) continue;
        if (changed.size() >= 4096) { capped = true; break; }
        changed.append(item.url());
        if (!item.url().isLocalFile() || item.isLink()) {
            d->proxy->directorySizes.insert(item.url(), {SizeUnavailable, 0, QStringLiteral("Requires a real local directory; symlinks and remote providers are not scanned.")});
        } else if (urls.size() < 4096) {
            urls.append(item.url());
            d->proxy->directorySizes.insert(item.url(), {SizeCalculating, 0, {}});
        } else { capped = true; break; }
    }
    d->proxy->sizesChanged(changed);
    if (urls.isEmpty()) {
        emit sizeCalculationFinished(false, false, QStringLiteral("No supported loaded folders to calculate."));
        return;
    }
    const auto revision = ++d->sizeRevision;
    auto scan = std::make_shared<SizeScan>();
    d->sizeScan = scan; d->sizeRoots = urls.size();
    auto *worker = QThread::create([urls, limits, scan] { scanDirectorySizes(urls, limits, scan); });
    d->sizeWorker = worker;
    connect(worker, &QThread::finished, this, [this, revision, urls, scan, capped] {
        d->sizeProgressTimer.stop();
        d->sizeWorker = nullptr; d->sizeScan.reset();
        const bool stale = revision != d->sizeRevision;
        if (!stale) {
            for (auto it = scan->sizes.cbegin(); it != scan->sizes.cend(); ++it)
                d->proxy->directorySizes.insert(it.key(), it.value());
            d->proxy->sizesChanged(urls);
        }
        emit sizeCalculationProgress(scan->entries.load(), scan->completed.load(), urls.size());
        emit sizeCalculationRunningChanged(false);
        const bool canceled = stale || scan->canceled || scan->cancelRequested.load();
        const bool limited = capped || scan->limited;
        const bool partial = std::any_of(scan->sizes.cbegin(), scan->sizes.cend(), [](const DirectorySize &size) { return size.state != SizeComplete; });
        const auto message = canceled ? QStringLiteral("Size calculation canceled.") : limited ? QStringLiteral("Size calculation reached its limit; totals may be partial.") :
            partial ? QStringLiteral("Size calculation finished; some folders could not be fully read.") : QStringLiteral("Folder sizes calculated.");
        emit sizeCalculationFinished(canceled, limited, message);
        emit statusMessage(message);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    d->sizeProgressTimer.start();
    worker->start();
    emit sizeCalculationRunningChanged(true);
    emit statusMessage(QStringLiteral("Calculating folder sizes…"));
}

QWidget *Browser::galleryPreviewHost() const { return d->galleryHost; }
QWidget *Browser::columnPreviewHost() const { return d->columnHost; }
QWidget *Browser::previewHost() const { return d->sidePreviewHost; }
KDirModel *Browser::directoryModel() const { return d->model; }
QAbstractItemView *Browser::activeView() const {
    switch (d->currentMode) { case List: return d->list; case Columns: return d->columns; case Gallery: return d->strip; default: return d->icons; }
}
QString Browser::confirmedRenameName(const KFileItem &item, const QString &name, QWidget *parent) {
    if (!validName(name)) return {};
    if (item.isDir() || !QSettings().value("Advanced/warnExtensionChange", true).toBool()) return name;
    const QMimeDatabase database;
    const auto extension = [&database](const QString &filename) {
        const auto known = database.suffixForFileName(filename);
        return known.isEmpty() ? QFileInfo(filename).suffix() : known;
    };
    const auto oldExtension = extension(item.name());
    const auto newExtension = extension(name);
    if (oldExtension.compare(newExtension, Qt::CaseInsensitive) == 0) return name;
    QMessageBox warning(QMessageBox::Warning, QStringLiteral("Change Filename Extension?"),
        QStringLiteral("Changing the extension of “%1” may change which application opens it.").arg(item.name()),
        QMessageBox::NoButton, parent);
    warning.setObjectName(QStringLiteral("finderExtensionWarning"));
    auto *keep = warning.addButton(QStringLiteral("Keep Original Extension"), QMessageBox::RejectRole);
    auto *change = warning.addButton(QStringLiteral("Use New Extension"), QMessageBox::AcceptRole);
    warning.addButton(QMessageBox::Cancel);
    warning.setDefaultButton(keep);
    warning.exec();
    if (warning.clickedButton() == change) return name;
    if (warning.clickedButton() != keep) return {};
    const auto stem = newExtension.isEmpty() ? name : name.left(name.size() - newExtension.size() - 1);
    return oldExtension.isEmpty() ? stem : stem + '.' + oldExtension;
}
KFileItemList Browser::selectedItems() const {
    KFileItemList result;
    for (const auto &index : d->selectedNames()) {
        const auto item = d->item(index);
        if (!item.isNull()) result.append(FileTarget::item(item));
    }
    return result;
}
QUrl Browser::primaryUrl() const { return FileTarget::url(d->item(d->activeSelection()->currentIndex())); }
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
    navigateValidated(target);
}
void Browser::navigateValidated(const QUrl &requested) {
    if (!requested.isValid() || requested.isEmpty()) return;
    if (!d->springNavigating) d->finishSpring(false);
    const QUrl target = Private::normalized(requested);
    d->springRestoreColumns = QUrl();
    d->changing = true;
    d->selection->clear(); d->columnSelection->clear(); d->pendingSelection.clear(); d->pendingRename = QUrl();
    d->restoringGallerySelection = false;
    ++d->folderCreationGeneration; d->creatingFolder = false;
    d->rootUrl = target; d->currentUrl = target;
    d->initialGallerySelectionPending = true;
    d->loadingDirectories.insert(target); d->listingErrors.remove(target); d->loadedDirectories.remove(target);
    d->resetThumbnailRequests();
    refreshMetadata();
    d->model->openUrl(target);
    // KDirLister can clear rows without a modelReset. Rebuild the column path
    // so invalidated child roots cannot become additional top-level columns.
    d->columns->setRootIndex(QModelIndex());
    d->changing = false;
    d->scheduleArrangement();
    if (d->springNavigating) emit springUrlChanged(target);
    else emit urlChanged(target);
    emit selectionChanged({}); emit previewRequested({});
    d->scheduleGallerySelection();
}
void Browser::setMode(Mode mode) {
    if (mode < Icons || mode > Gallery || d->currentMode == mode) return;
    if (!d->springNavigating) d->finishSpring(false);
    const auto urls = selectedUrls(); const QUrl current = d->currentUrl;
    const bool collapseColumns = d->currentMode == Columns && current != d->rootUrl;
    d->saveModeOptions();
    const bool previousChanging = d->changing;
    d->changing = true;
    d->currentMode = mode; d->stack->setCurrentIndex(int(mode));
    if (mode == Gallery && urls.isEmpty()) d->initialGallerySelectionPending = true;
    d->updateDirectoryStates();
    setFocusProxy(activeView());
    d->restoreModeOptions();
    d->proxy->decorationSize = activeView()->iconSize(); d->proxy->decorationScale = activeView()->devicePixelRatioF();
    d->resetThumbnailRequests();
    d->updatePreviewVisibility(); d->layoutIcons();
    d->updateArrangementMode();
    if (collapseColumns) navigate(current);
    selectUrls(urls);
    d->changing = previousChanging;
    activeView()->setFocus(); d->selected();
    d->scheduleGallerySelection();
    QTimer::singleShot(0, this, [this] { d->restoreScroll(); });
}
void Browser::setHiddenFiles(bool show) { d->model->dirLister()->setShowHiddenFiles(show); d->model->dirLister()->emitChanges(); }
void Browser::setFilter(const QString &filter) { d->proxy->setFilterFixedString(filter); d->count(); d->scheduleThumbnails(); }
void Browser::setSort(int column, bool ascending) {
    column = qBound(mode() == Icons ? -1 : 0, column, int(ListColumnCount) - 1);
    const auto urls = selectedUrls();
    d->viewOptions[mode()].sort = column;
    d->viewOptions[mode()].ascending = ascending;
    const auto order = ascending ? Qt::AscendingOrder : Qt::DescendingOrder;
    const QSignalBlocker blocker(d->list->header());
    d->list->header()->setSortIndicator(column, order);
    // KCategorizedView requires a sortable column for its groups. None is a
    // separate Icons setting; grouping may still sort within its buckets.
    d->proxy->sort(column < 0 && group() != GroupNone ? NameColumn : column, order);
    d->updateArrangementMode();
    selectUrls(urls);
    d->scheduleThumbnails();
}
void Browser::setSnapToGrid(bool enabled) {
    if (d->gridSnap == enabled) return;
    d->gridSnap = enabled;
    d->updateArrangementMode();
    emit iconArrangementChanged();
}
void Browser::setIconPosition(const QUrl &url, const QPoint &position) {
    if (!canArrangeIcons() || Private::normalized(parentUrl(url)) != Private::normalized(d->rootUrl)) return;
    const auto index = d->proxy->mapFromSource(d->model->indexForUrl(url));
    if (!index.isValid()) return;
    const QPoint point = d->gridSnap ? d->snapped(position) : d->boundedPosition(position);
    auto &positions = d->arrangements[Private::normalized(d->rootUrl)];
    if (positions.value(Private::normalized(url), QPoint(-1, -1)) == point) return;
    positions.insert(Private::normalized(url), point);
    d->applyArrangement();
    emit iconArrangementChanged();
}
void Browser::cleanUp() {
    if (!canArrangeIcons()) return;
    const auto selected = selectedUrls();
    auto &positions = d->arrangements[Private::normalized(d->rootUrl)];
    QList<QUrl> urls = selected;
    if (urls.isEmpty()) {
        for (int row = 0; row < d->proxy->rowCount(); ++row) urls.append(d->item(d->proxy->index(row, 0)).url());
    }
    QSet<QUrl> moving;
    for (auto &url : urls) { url = Private::normalized(url); moving.insert(url); }
    std::sort(urls.begin(), urls.end(), [&positions](const auto &a, const auto &b) {
        const QPoint pa = positions.value(a), pb = positions.value(b);
        return pa.y() != pb.y() ? pa.y() < pb.y() : pa.x() != pb.x() ? pa.x() < pb.x() : a.toString() < b.toString();
    });
    QSet<quint64> occupied;
    for (auto it = positions.cbegin(); it != positions.cend(); ++it)
        if (!moving.contains(it.key())) d->occupy(occupied, it.value());
    for (const auto &url : urls) positions[url] = d->vacant(occupied, d->snapped(positions.value(url)));
    d->applyArrangement();
    emit iconArrangementChanged();
}
void Browser::cleanUpBy(int column, bool ascending) {
    if (!canArrangeIcons() || column < 0 || column >= ListColumnCount) return;
    const auto selected = selectedUrls();
    QSet<QUrl> selectedSet;
    for (const auto &url : selected) selectedSet.insert(Private::normalized(url));
    {
        const QScopedValueRollback<bool> guard(d->applyingArrangement, true);
        const QScopedValueRollback<bool> sorting(d->sortingArrangement, true);
        const QSignalBlocker header(d->list->header());
        d->proxy->sort(column, ascending ? Qt::AscendingOrder : Qt::DescendingOrder);
        QList<QUrl> urls;
        for (int row = 0; row < d->proxy->rowCount(); ++row) {
            const auto url = Private::normalized(d->item(d->proxy->index(row, 0)).url());
            if (selectedSet.isEmpty() || selectedSet.contains(url)) urls.append(url);
        }
        auto &positions = d->arrangements[Private::normalized(d->rootUrl)];
        QSet<QUrl> moving;
        for (const auto &url : urls) moving.insert(url);
        QSet<quint64> occupied;
        for (auto it = positions.cbegin(); it != positions.cend(); ++it)
            if (!moving.contains(it.key())) d->occupy(occupied, it.value());
        QPoint nextVacancy;
        for (const auto &url : urls) { nextVacancy = d->vacant(occupied, nextVacancy); positions[url] = nextVacancy; }
        d->proxy->sort(-1, d->viewOptions[Icons].ascending ? Qt::AscendingOrder : Qt::DescendingOrder);
        selectUrls(selected);
    }
    d->scheduleArrangement();
    emit iconArrangementChanged();
}
void Browser::setSpringLoadingEnabled(bool enabled) {
    d->springEnabled = enabled;
    if (!enabled) cancelSpringLoading();
}
void Browser::setSpringLoadingDelay(int milliseconds) { d->springTimer.setInterval(qBound(100, milliseconds, 3000)); }
void Browser::cancelSpringLoading() { d->finishSpring(false); }
void Browser::setIconSize(int size) {
    if (mode() == Columns) return;
    size = mode() == List ? qBound(16, size, 32) : qBound(32, size, 256);
    if (iconSize() == size) return;
    auto *view = activeView();
    view->setIconSize(QSize(size, size));
    d->viewOptions[mode()].size = size;
    d->proxy->decorationSize = activeView()->iconSize(); d->proxy->decorationScale = activeView()->devicePixelRatioF();
    if (mode() == List) {
        auto *delegate = static_cast<FileDelegate *>(d->list->itemDelegate());
        delegate->setMaximumSize(QSize(10000, qMax(24, size + 6) + SectionHeight));
        d->list->doItemsLayout();
    }
    d->layoutIcons();
    d->resetThumbnailRequests();
}
void Browser::setGroup(Group value) {
    if (value < GroupNone || value > GroupTags || group() == value) return;
    const auto urls = selectedUrls();
    const bool previousChanging = d->changing;
    d->changing = true;
    d->proxy->setGroup(value);
    if (sortColumn() < 0) d->proxy->sort(value == GroupNone ? -1 : NameColumn, sortAscending() ? Qt::AscendingOrder : Qt::DescendingOrder);
    d->updateArrangementMode();
    d->list->setUniformRowHeights(value == GroupNone);
    if (mode() == List) d->list->doItemsLayout();
    else if (mode() == Icons) d->icons->doItemsLayout();
    selectUrls(urls);
    d->changing = previousChanging;
    d->selected();
    d->scheduleThumbnails();
}
void Browser::setGridSpacing(int spacing) {
    spacing = qBound(0, spacing, 128);
    if (gridSpacing() == spacing) return;
    d->iconOptions.spacing = spacing;
    d->viewOptions[mode()].icons.spacing = spacing;
    d->layoutIcons();
    d->scheduleThumbnails();
}
void Browser::setTextSize(int pixels) {
    pixels = qBound(9, pixels, 20);
    if (textSize() == pixels && activeView()->font().pixelSize() == pixels) return;
    d->iconOptions.textSize = pixels;
    d->viewOptions[mode()].icons.textSize = pixels;
    QFont font = d->icons->font(); font.setPixelSize(d->iconOptions.textSize);
    activeView()->setFont(font);
    if (mode() == Columns)
        for (auto *column : d->columns->findChildren<QListView *>()) column->setFont(font);
    d->layoutIcons();
    if (mode() == List) d->list->doItemsLayout();
    d->scheduleThumbnails();
}
void Browser::setLabelPosition(LabelPosition position) {
    if (position < LabelBelow || position > LabelRight || labelPosition() == position) return;
    d->iconOptions.label = position;
    d->viewOptions[mode()].icons.label = position;
    d->layoutIcons();
    d->scheduleThumbnails();
}
void Browser::setIconPreviewsShown(bool shown) {
    if (d->previews == shown) return;
    d->previews = shown;
    d->resetThumbnailRequests();
    if (!shown) d->model->clearAllPreviews();
}
bool Browser::galleryFilenamesShown() const {
    return !d->strip->property("finderShowFilenames").isValid() || d->strip->property("finderShowFilenames").toBool();
}
void Browser::setGalleryFilenamesShown(bool shown) {
    if (galleryFilenamesShown() == shown) return;
    d->strip->setProperty("finderShowFilenames", shown); d->layoutIcons();
}
void Browser::setPreviewShown(bool shown) {
    d->sidePreview = shown;
    d->updatePreviewVisibility();
    d->selected();
}
void Browser::setPreviewWidth(int width) {
    d->sidePreviewWidth = qBound(180, width, 900);
    d->updatePreviewVisibility();
}
void Browser::refreshTags() { d->proxy->refreshTags(); }
void Browser::refreshFileDetails() { d->proxy->refreshFileDetails(); }
void Browser::refreshPreferences() {
    d->proxy->refreshPreferences();
    // QColumnView's dynamically created child views paint their own viewports.
    for (auto *view : findChildren<QAbstractItemView *>()) view->viewport()->update();
}
void Browser::setFoldersFirst(bool enabled) { d->proxy->setFoldersFirstPreference(enabled); }
void Browser::selectUrls(const QList<QUrl> &urls) { d->pendingSelection = urls; if (urls.isEmpty()) d->activeSelection()->clearSelection(); else d->applySelection(); }
void Browser::renameSelected() {
    const auto rows = d->selectedNames(); if (rows.isEmpty()) return;
    if (rows.size() > 1) {
        const auto origin = url();
        auto *dialog = new BatchRenameDialog(selectedItems(), window());
        dialog->setObjectName(QStringLiteral("finderBatchRename"));
        connect(dialog, &BatchRenameDialog::renamingFinished, this, [this, origin](const QList<QUrl> &urls) { if (url() == origin) selectUrls(urls); });
        connect(dialog, &BatchRenameDialog::error, this, [this](KJob *job) { emit errorOccurred(job->errorString()); });
        dialog->show();
        return;
    }
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
    if (!url().isValid() || url().scheme() == "trash" || d->creatingFolder) return;
    const auto parent = url();
    QSet<QString> occupied;
    for (const auto &entry : d->model->dirLister()->itemsForDir(parent, KDirLister::AllItems)) occupied.insert(entry.name());
    auto candidate = std::make_shared<int>(1);
    while (occupied.contains(*candidate == 1 ? QStringLiteral("Untitled Folder") : QString("Untitled Folder %1").arg(*candidate))) ++*candidate;
    d->creatingFolder = true;
    const auto generation = ++d->folderCreationGeneration;
    auto attempt = std::make_shared<std::function<void()>>();
    *attempt = [this, parent, candidate, generation, weak = std::weak_ptr<std::function<void()>>(attempt)] {
        if (generation != d->folderCreationGeneration || url() != parent) return;
        const auto name = *candidate == 1 ? QStringLiteral("Untitled Folder") : QString("Untitled Folder %1").arg(*candidate);
        const auto destination = childUrl(parent, name);
        auto *job = KIO::mkdir(destination, -1);
        KIO::FileUndoManager::self()->recordJob(KIO::FileUndoManager::Mkdir, {}, destination, job);
        emit statusMessage(QStringLiteral("Creating folder…"));
        auto keep = weak.lock();
        connect(job, &KJob::result, this, [this, parent, destination, candidate, generation, keep](KJob *done) {
            if (generation != d->folderCreationGeneration) return;
            if ((done->error() == KIO::ERR_DIR_ALREADY_EXIST || done->error() == KIO::ERR_FILE_ALREADY_EXIST) && ++*candidate <= 10000) {
                if (keep) QTimer::singleShot(0, this, [keep] { (*keep)(); });
                return;
            }
            d->creatingFolder = false;
            if (done->error()) { emit errorOccurred(done->errorString()); return; }
            emit statusMessage(QStringLiteral("Folder created"));
            if (url() == parent) { d->pendingRename = destination; selectUrls({destination}); }
        });
    };
    (*attempt)();
}
void Browser::newFolderWithSelection() {
    if (!canNewFolderWithSelection()) return;
    const auto sources = selectedUrls();
    // In Columns mode url() can be the selected directory itself. Always
    // create alongside the captured sources, never inside a selected folder.
    const auto parent = parentUrl(sources.first());
    bool ok = false;
    const auto name = QInputDialog::getText(window(), "New Folder with Selection", "Name:", QLineEdit::Normal, "New Folder With Items", &ok);
    if (!ok) return;
    if (!validName(name)) { emit errorOccurred("Enter a valid folder name without a slash."); return; }
    const auto destination = childUrl(parent, name);
    auto *create = KIO::mkdir(destination);
    KIO::FileUndoManager::self()->recordJob(KIO::FileUndoManager::Mkdir, {}, destination, create);
    d->job(create, "Creating folder…");
    connect(create, &KJob::result, this, [this, sources, destination, parent](KJob *finished) {
        if (finished->error()) return; // Never merge into an existing folder.
        auto *move = KIO::move(sources, destination);
        d->copyJob(move, "Moving selection into folder…");
        connect(move, &KJob::result, this, [this, destination, parent](KJob *finished) {
            // KIO records actual completed moves even on partial failure.
            // Keep the new folder and let native Undo reverse the move, then
            // the mkdir; deleting it as rollback could delete moved files.
            if (!finished->error() && url() == parent) selectUrls({destination});
        });
    });
}
void Browser::autoSizeListColumns(int column) {
    if (column < -1 || column >= listColumnCount()) return;
    for (int current = 0; current < listColumnCount(); ++current) {
        if ((column == -1 || column == current) && !d->list->isColumnHidden(current))
            d->list->resizeColumnToContents(current);
    }
}
void Browser::copySelected(bool cut) {
    const auto rows = d->selectedNames(); if (rows.isEmpty()) return;
    auto *mime = d->proxy->mimeData(rows);
    if (!mime) return;
    mime->setData("application/x-kde-cutselection", cut ? "1" : "0");
    QApplication::clipboard()->setMimeData(mime);
    emit statusMessage(QString::number(rows.size()) + (cut ? " item(s) ready to move" : " item(s) copied"));
}
void Browser::paste(bool move) {
    if (!url().isValid()) return;
    const auto destination = url();
    const auto *mime = QApplication::clipboard()->mimeData();
    if (!mime) return;
    if (move && mime->hasUrls()) {
        d->copyJob(KIO::move(mime->urls(), destination), "Moving…");
    } else {
        auto *job = KIO::paste(mime, destination);
        d->job(job, "Pasting…");
        auto created = std::make_shared<QList<QUrl>>();
        connect(job, &KIO::PasteJob::itemCreated, this, [created, destination](const QUrl &item) {
            // Folder copies also emit descendants. Select the new top-level
            // entries together after the job, rather than the last copied file.
            if (parentUrl(item) == destination) created->append(item);
        });
        connect(job, &KJob::result, this, [this, created, destination] {
            if (url() == destination && !created->isEmpty()) selectUrls(*created);
        });
    }
}
void Browser::trashSelected() { const auto urls = selectedUrls(); if (!urls.isEmpty()) d->copyJob(KIO::trash(urls), "Moving to Trash…"); }
void Browser::duplicateSelected() {
    const auto sources = selectedUrls();
    QSet<QUrl> occupied;
    QSet<QUrl> parents;
    for (const auto &source : sources) {
        const auto parent = parentUrl(source);
        if (parents.contains(parent)) continue;
        parents.insert(parent);
        for (const auto &entry : d->model->dirLister()->itemsForDir(parent, KDirLister::AllItems)) occupied.insert(entry.url());
    }
    for (const auto &source : sources) {
        const QFileInfo info(source.fileName());
        const auto entry = d->model->dirLister()->findByUrl(source);
        QString suffix = entry.isDir() || info.suffix().isEmpty() ? QString() : "." + info.suffix();
        QString stem = suffix.isEmpty() ? source.fileName() : info.completeBaseName();
        const auto parent = parentUrl(source);
        QUrl destination = childUrl(parent, stem + " copy" + suffix);
        int index = 2;
        while (occupied.contains(destination)) destination = childUrl(parent, stem + " copy " + QString::number(index++) + suffix);
        occupied.insert(destination);
        auto *copy = KIO::copyAs(source, destination);
        // The lister snapshot avoids GUI-thread stats, but another client may
        // create this name meanwhile. Native auto-rename preserves both files.
        copy->setAutoRename(true);
        d->copyJob(copy, "Duplicating…");
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
    const bool option = event->modifiers().testFlag(Qt::AltModifier);
    const bool command = event->modifiers().testFlag(Qt::MetaModifier);
    Qt::DropAction action = event->dropAction();
    Qt::KeyboardModifiers modifiers = event->modifiers();
    if (option || command) {
        action = option && command ? Qt::LinkAction : option ? Qt::CopyAction : Qt::MoveAction;
        // DropJob accepts Qt's action when a native Linux modifier is present.
        // Translate Finder intent to that public event API, retaining KIO's
        // conflict handling, protocols and built-in FileUndoManager recording.
        modifiers = action == Qt::LinkAction ? Qt::ControlModifier | Qt::ShiftModifier :
                    action == Qt::CopyAction ? Qt::ControlModifier : Qt::ShiftModifier;
    }
    QDropEvent native(event->position(), event->possibleActions() | action, event->mimeData(), event->buttons(), modifiers);
    native.setDropAction(action);
    auto *job = KIO::drop(&native, destination);
    d->job(job, "Transferring…");
    auto created = std::make_shared<QList<QUrl>>();
    connect(job, &KIO::DropJob::itemCreated, this, [created, destination](const QUrl &url) {
        if (Private::normalized(parentUrl(url)) == Private::normalized(destination)) created->append(url);
    });
    connect(job, &KJob::result, this, [this, created, destination] {
        if (Private::normalized(url()) == Private::normalized(destination) && !created->isEmpty()) selectUrls(*created);
    });
    d->finishSpring(true);
    event->setDropAction(action); event->accept();
}
bool Browser::eventFilter(QObject *object, QEvent *event) {
    if ((event->type() == QEvent::Resize || event->type() == QEvent::Show) &&
        d->directoryStates.contains(qobject_cast<QWidget *>(object))) d->updateDirectoryStates();
    auto *dragView = qobject_cast<QAbstractItemView *>(object->parent());
    if (dragView && object == dragView->viewport()) {
        if (event->type() == QEvent::DragEnter) {
            auto *drag = static_cast<QDragEnterEvent *>(event);
            if (drag->mimeData()->hasUrls()) {
                d->beginSpring(dragView, drag->mimeData()); drag->acceptProposedAction(); return true;
            }
        } else if (event->type() == QEvent::DragMove) {
            auto *drag = static_cast<QDragMoveEvent *>(event);
            if (drag->mimeData()->hasUrls()) {
                d->hoverSpring(dragView, drag);
            }
        } else if (event->type() == QEvent::DragLeave) {
            d->springTimer.stop(); d->springHover = QUrl();
            if (d->springActive) d->springLeaveTimer.start();
        } else if (event->type() == QEvent::Drop) {
            auto *dropEvent = static_cast<QDropEvent *>(event);
            if (dropEvent->mimeData()->hasUrls()) {
                auto index = dragView->indexAt(dropEvent->position().toPoint());
                if (dragView == d->icons && d->arrangeDrop(dropEvent, index)) d->finishSpring(true);
                else {
                    if (!index.isValid()) index = dragView->rootIndex();
                    const auto entry = d->item(index);
                    drop(dropEvent, entry.isDir() ? FileTarget::url(entry) : index.isValid() ? parentUrl(FileTarget::url(entry)) :
                         d->currentMode == Columns ? d->rootUrl : url());
                }
                if (dragView == d->icons) d->icons->endDragState();
                else if (dragView == d->list) d->list->endDragState();
                else if (dragView == d->columns) d->columns->endDragState();
                else static_cast<DropView<QListView> *>(dragView)->endDragState();
                return true;
            }
        } else if (event->type() == QEvent::Hide && d->springActive && !d->springNavigating) cancelSpringLoading();
    }
    if (event->type() == QEvent::MouseButtonDblClick) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        auto *view = qobject_cast<QAbstractItemView *>(object->parent());
        if (mouse->button() == Qt::LeftButton && view && object == view->viewport() &&
            (mouse->modifiers() & (Qt::MetaModifier | Qt::ControlModifier | Qt::AltModifier))) {
            const auto index = view->indexAt(mouse->position().toPoint());
            if (d->item(index).isDir()) {
                // Capture the input event before Qt emits doubleClicked or a
                // queued action can lose the user's actual modifier state.
                d->activate(index, mouse->modifiers());
                return true;
            }
        }
    }
    if (event->type() == QEvent::Resize || event->type() == QEvent::Show) {
        auto *view = qobject_cast<QAbstractItemView *>(object->parent());
        if (view && object == view->viewport()) {
            if (event->type() == QEvent::Show) view->viewport()->setAcceptDrops(true);
            d->scheduleThumbnails();
        }
    }
    if (event->type() == QEvent::DevicePixelRatioChange) {
        d->proxy->decorationScale = activeView()->devicePixelRatioF();
        d->resetThumbnailRequests();
    }
    if (event->type() == QEvent::Resize && object == d->strip->viewport()) {
        // Defer until Qt has established the viewport geometry; no animation
        // loop or background polling is needed for a stationary filmstrip.
        QTimer::singleShot(0, this, [this] { d->layoutIcons(); });
    }
    if (event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape && d->springActive) { cancelSpringLoading(); return true; }
        if ((object == d->strip || object == d->strip->viewport()) &&
            (key->key() == Qt::Key_PageUp || key->key() == Qt::Key_PageDown) && key->modifiers() == Qt::NoModifier) {
            const auto current = d->selection->currentIndex();
            const auto parent = d->strip->rootIndex();
            const int count = d->proxy->rowCount(parent);
            if (count) {
                const int page = qMax(1, d->strip->viewport()->width() / qMax(1, d->strip->gridSize().width()));
                const int step = key->key() == Qt::Key_PageDown ? page : -page;
                const int row = current.isValid() ? qBound(0, current.row() + step, count - 1) : 0;
                const auto target = d->proxy->index(row, KDirModel::Name, parent);
                d->selection->setCurrentIndex(target, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                d->strip->scrollTo(target);
            }
            return true;
        }
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
