// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#include "inspector.h"
#include "tags.h"

#include <QAbstractTableModel>
#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFormLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QLabel>
#include <QLocale>
#include <QMimeDatabase>
#include <QPushButton>
#include <QPointer>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QTabWidget>
#include <QTableView>
#include <QThread>
#include <QThreadPool>
#include <QTimeZone>
#include <QVBoxLayout>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <optional>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr qsizetype batchSize = 32;
constexpr qint64 timeLimitMs = 5000; // Cooperative, between filesystem calls.
constexpr qsizetype displayLimit = 8192;
using Cancellation = std::shared_ptr<std::atomic_bool>;

QString bounded(const QString &value)
{
    return value.size() <= displayLimit ? value : value.left(displayLimit) + QStringLiteral("… (truncated)");
}

bool actionable(const QUrl &url)
{
    return url.isValid() && !url.isEmpty() && !url.isRelative() && !url.path().contains(QChar::Null);
}

bool localMetadataUrl(const QUrl &url)
{
    return actionable(url) && url.isLocalFile()
        && (url.host().isEmpty() || url.host() == QStringLiteral("localhost"))
        && url.userInfo().isEmpty() && url.port() == -1 && !url.hasQuery() && !url.hasFragment()
        && QDir::isAbsolutePath(url.toLocalFile());
}

QString nameFor(const QUrl &url)
{
    const auto name = url.adjusted(QUrl::StripTrailingSlash).fileName();
    if (!name.isEmpty()) return bounded(name);
    return bounded(url.isLocalFile() ? url.toLocalFile()
        : url.toDisplayString(QUrl::RemoveUserInfo | QUrl::RemoveQuery | QUrl::RemoveFragment));
}

QString locationFor(const QUrl &url)
{
    const auto parent = url.adjusted(QUrl::StripTrailingSlash | QUrl::RemoveFilename
        | QUrl::RemoveUserInfo | QUrl::RemoveQuery | QUrl::RemoveFragment);
    return bounded(parent.isLocalFile() && (parent.host().isEmpty() || parent.host() == QStringLiteral("localhost"))
        ? QDir::cleanPath(parent.toLocalFile()) : parent.toDisplayString());
}

QString byteText(quint64 bytes)
{
    const auto exact = Inspector::tr("%1 bytes").arg(QLocale().toString(bytes));
    if (bytes < 1024 || bytes > quint64(std::numeric_limits<qint64>::max())) return exact;
    return Inspector::tr("%1 (%2)").arg(QLocale().formattedDataSize(qint64(bytes), 1, QLocale::DataSizeIecFormat), exact);
}

QString dateText(const QDateTime &value)
{
    return value.isValid() ? QLocale().toString(value.toLocalTime(), QLocale::ShortFormat)
                           : Inspector::tr("Unavailable");
}

struct Item {
    bool available = false;
    bool regular = false;
    QString kind;
    QString size;
    QDateTime modified, created;
    QString tags;
    QString status;
};

struct Result {
    Inspector::Summary summary;
    QList<Item> items;
};

struct Request {
    QList<QUrl> urls;
    quint64 generation;
    Cancellation cancelled;
};

struct State {
    std::mutex mutex;
    std::optional<Request> pending;
    Cancellation active;
    Inspector *receiver = nullptr; // Protected by mutex, including destruction.
    quint64 generation = 0;
    bool scheduled = false;
    bool stopping = false;
};

QThreadPool &metadataPool()
{
    // Match the existing SmartFolderReader lifecycle: process-owned so neither
    // controller destruction nor application shutdown joins a stuck mount.
    // Idle threads expire; the OS reclaims the pool at process exit.
    static auto *pool = [] {
        auto *value = new QThreadPool;
        value->setMaxThreadCount(2);
        value->setExpiryTimeout(30000);
        return value;
    }();
    return *pool;
}

class Descriptor {
public:
    explicit Descriptor(int descriptor) : descriptor(descriptor) {}
    ~Descriptor() { if (descriptor >= 0) ::close(descriptor); }
    int get() const { return descriptor; }
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
private:
    int descriptor;
};

void addDate(const QDateTime &date, qsizetype &count, QDateTime &first, QDateTime &last)
{
    if (!date.isValid()) return;
    if (!count || date < first) first = date;
    if (!count || date > last) last = date;
    ++count;
}

Item inspect(const QUrl &url, Inspector::Summary &summary, QMimeDatabase &mimes,
             const Cancellation &cancelled)
{
    Item item;
    if (!actionable(url)) {
        ++summary.unavailableCount;
        item.status = Inspector::tr("Invalid or relative URL; metadata unavailable.");
        return item;
    }
    if (!localMetadataUrl(url)) {
        ++summary.remoteCount;
        item.status = Inspector::tr("Metadata unavailable here for this remote or unsupported URL. Use Native Get Info.");
        return item;
    }
    auto localUrl = url;
    localUrl.setHost(QString()); // file://localhost has the same local inode semantics.
    const auto path = QFile::encodeName(localUrl.toLocalFile());
    // Pin the selected inode without opening contents, FIFOs or device drivers.
    // A symlink describes the link itself; never recurse into a selected folder.
    const Descriptor pinned(::open(path.constData(), O_PATH | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    struct stat metadata {};
    if (pinned.get() < 0 || ::fstat(pinned.get(), &metadata) != 0) {
        const int code = errno;
        ++summary.unavailableCount;
        item.status = Inspector::tr("Metadata unavailable: %1").arg(QString::fromLocal8Bit(std::strerror(code)));
        return item;
    }
    if (cancelled->load(std::memory_order_relaxed)) return item;
    item.available = true;
    ++summary.availableCount;
    if (S_ISREG(metadata.st_mode)) {
        item.regular = true;
        ++summary.fileCount;
        const auto mime = mimes.mimeTypeForFile(url.fileName(), QMimeDatabase::MatchExtension);
        item.kind = mime.isValid() ? mime.comment() : Inspector::tr("File");
        const auto bytes = quint64(std::max<off_t>(0, metadata.st_size));
        if (std::numeric_limits<quint64>::max() - summary.fileBytes < bytes) summary.sizeOverflow = true;
        else summary.fileBytes += bytes;
        item.size = byteText(bytes);
    } else if (S_ISDIR(metadata.st_mode)) {
        ++summary.folderCount;
        item.kind = Inspector::tr("Folder");
        item.size = Inspector::tr("Not calculated; folder contents excluded");
    } else if (S_ISLNK(metadata.st_mode)) {
        ++summary.linkCount;
        item.kind = Inspector::tr("Symbolic link");
        item.size = Inspector::tr("Excluded; target not followed");
    } else {
        ++summary.otherCount;
        item.kind = S_ISFIFO(metadata.st_mode) ? Inspector::tr("FIFO")
            : S_ISSOCK(metadata.st_mode) ? Inspector::tr("Socket")
            : (S_ISCHR(metadata.st_mode) || S_ISBLK(metadata.st_mode)) ? Inspector::tr("Device")
            : Inspector::tr("Special file");
        item.size = Inspector::tr("Excluded; special file contents not opened");
    }
    ++summary.kinds[item.kind];
    item.modified = QDateTime::fromMSecsSinceEpoch(qint64(metadata.st_mtim.tv_sec) * 1000
        + metadata.st_mtim.tv_nsec / 1000000, QTimeZone::UTC);
    addDate(item.modified, summary.modifiedCount, summary.earliestModified, summary.latestModified);
    if (cancelled->load(std::memory_order_relaxed)) return item;
    struct statx birth {};
    if (::statx(pinned.get(), "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, STATX_BTIME, &birth) == 0
        && (birth.stx_mask & STATX_BTIME)) {
        item.created = QDateTime::fromMSecsSinceEpoch(qint64(birth.stx_btime.tv_sec) * 1000
            + birth.stx_btime.tv_nsec / 1000000, QTimeZone::UTC);
        addDate(item.created, summary.createdCount, summary.earliestCreated, summary.latestCreated);
    }
    if ((S_ISREG(metadata.st_mode) || S_ISDIR(metadata.st_mode)) && !cancelled->load(std::memory_order_relaxed)) {
        // Check the pinned inode, so a renamed path cannot redirect this access
        // check. This reports contents access separately from metadata access.
        const auto inodePath = QByteArray("/proc/self/fd/") + QByteArray::number(pinned.get());
        if (::faccessat(AT_FDCWD, inodePath.constData(), R_OK | (S_ISDIR(metadata.st_mode) ? X_OK : 0), AT_EACCESS) != 0) {
            ++summary.unreadableCount;
            item.status = Inspector::tr("Contents are not readable; filesystem metadata is available.");
        }
    }
    if (item.status.isEmpty()) item.status = Inspector::tr("Filesystem metadata; file kinds inferred from names.");
    return item;
}

Result readMetadata(const Request &request)
{
    Result result;
    auto &summary = result.summary;
    summary.selectedCount = request.urls.size();
    const auto limit = std::min(request.urls.size(), Inspector::maximumMetadataItems);
    result.items.reserve(limit);
    QElapsedTimer elapsed;
    elapsed.start();
    QMimeDatabase mimes;
    bool firstLocation = true;
    QStringList firstTags;
    auto proceed = [&] {
        if (request.cancelled->load(std::memory_order_relaxed)) return false;
        if (elapsed.elapsed() >= timeLimitMs) { summary.timeLimited = true; return false; }
        return true;
    };
    while (result.items.size() < limit && proceed()) {
        const qsizetype start = result.items.size();
        const qsizetype end = std::min(limit, start + batchSize);
        QList<QUrl> tagUrls;
        for (qsizetype index = start; index < end && proceed(); ++index) {
            const auto &url = request.urls[index];
            auto item = inspect(url, summary, mimes, request.cancelled);
            if (request.cancelled->load(std::memory_order_relaxed)) break;
            const auto location = locationFor(url);
            if (firstLocation) { summary.location = location; firstLocation = false; }
            else if (summary.location != location) summary.multipleLocations = true;
            if (item.available && (item.regular || item.kind == Inspector::tr("Folder"))) {
                auto localUrl = url;
                localUrl.setHost(QString());
                tagUrls.append(localUrl);
            }
            result.items.append(std::move(item));
            ++summary.inspectedCount;
        }
        if (!proceed()) break;
        // Reuse the native reader: one mount snapshot per bounded batch, no tag
        // database, writes, registry discovery or whole-disk scanning.
        QHash<QUrl, QString> errors;
        const auto tags = tagUrls.isEmpty() ? QHash<QUrl, QStringList>() : FileTags::tagsForUrls(tagUrls, &errors);
        if (!proceed()) break;
        for (qsizetype index = start; index < result.items.size(); ++index) {
            auto &item = result.items[index];
            auto url = request.urls[index];
            if (localMetadataUrl(url)) url.setHost(QString());
            if (!tags.contains(url) || errors.contains(url)) {
                ++summary.tagsUnavailableCount;
                item.tags = errors.contains(url) ? errors.value(url) : Inspector::tr("Unavailable for this item");
                continue;
            }
            auto values = tags.value(url);
            values.removeDuplicates();
            values.sort();
            item.tags = values.isEmpty() ? Inspector::tr("None") : bounded(values.join(QStringLiteral(", ")));
            if (!summary.tagsReadCount) { firstTags = values; summary.commonTags = values; }
            else {
                if (values != firstTags) summary.tagSetsDiffer = true;
                for (auto it = summary.commonTags.begin(); it != summary.commonTags.end();) {
                    if (!values.contains(*it)) it = summary.commonTags.erase(it);
                    else ++it;
                }
            }
            ++summary.tagsReadCount;
        }
    }
    summary.tagsUnavailableCount = summary.inspectedCount - summary.tagsReadCount;
    return result;
}

// URL-derived names and locations are cheap model lookups. No stat, MIME
// probes, xattrs or directory traversal ever run from a data/paint callback.
class ItemsModel final : public QAbstractTableModel {
public:
    explicit ItemsModel(QObject *parent) : QAbstractTableModel(parent) {}
    QList<QUrl> urls;
    QList<Item> items;
    void reset(QList<QUrl> next, QList<Item> metadata = {}) {
        beginResetModel(); urls = std::move(next); items = std::move(metadata); endResetModel();
    }
    int rowCount(const QModelIndex &parent = {}) const override {
        return parent.isValid() ? 0 : int(std::min<qsizetype>(urls.size(), std::numeric_limits<int>::max()));
    }
    int columnCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : 3; }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
        if (section == 0) return Inspector::tr("Name");
        if (section == 1) return Inspector::tr("Size");
        return Inspector::tr("Individual Properties");
    }
    QVariant data(const QModelIndex &index, int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= urls.size()) return {};
        const auto &url = urls[index.row()];
        if (role == Qt::UserRole) return url;
        if (role == Qt::AccessibleDescriptionRole || role == Qt::ToolTipRole) {
            if (index.column() == 2) return Inspector::tr("Open native properties for %1").arg(nameFor(url));
            return index.row() < items.size() ? items[index.row()].status : Inspector::tr("Metadata not inspected");
        }
        if (role == Qt::DisplayRole || role == Qt::AccessibleTextRole) {
            if (index.column() == 0) return nameFor(url);
            if (index.column() == 2) return actionable(url) ? Inspector::tr("Native Get Info…") : Inspector::tr("Unavailable");
            if (index.row() >= items.size()) return Inspector::tr("Not inspected");
            const auto &item = items[index.row()];
            return item.size.isEmpty() ? Inspector::tr("Unavailable") : item.size;
        }
        return {};
    }
};

class LinkDelegate final : public QStyledItemDelegate {
public:
    explicit LinkDelegate(QObject *parent) : QStyledItemDelegate(parent) {}
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override {
        QStyledItemDelegate::initStyleOption(option, index);
        if (!actionable(index.data(Qt::UserRole).toUrl())) return;
        option->font.setUnderline(true);
        option->palette.setColor(QPalette::Text, option->palette.color(QPalette::Link));
    }
};
} // namespace

class Inspector::Private {
public:
    explicit Private(Inspector *owner) : q(owner), state(std::make_shared<State>()) { state->receiver = owner; }
    Inspector *q;
    std::shared_ptr<State> state;
    QList<QUrl> urls, latestSelection;
    Summary summary;
    bool follow = true;
    bool loading = false;
    bool completed = false;
    QCheckBox *followBox = nullptr;
    QLabel *heading = nullptr;
    QLabel *status = nullptr;
    QMap<QString, QLabel *> fields, detailFields;
    ItemsModel *model = nullptr;
    QTableView *table = nullptr;
    QPushButton *properties = nullptr;
    QPushButton *reveal = nullptr;
    QPushButton *stop = nullptr;

    static void work(const std::shared_ptr<State> &state) {
        for (;;) {
            std::optional<Request> request;
            {
                std::lock_guard lock(state->mutex);
                if (state->stopping || !state->pending) {
                    state->scheduled = false; state->active.reset(); return;
                }
                request = std::move(state->pending); state->pending.reset(); state->active = request->cancelled;
            }
            if (request->cancelled->load(std::memory_order_relaxed)) continue;
            auto result = readMetadata(*request);
            std::lock_guard lock(state->mutex);
            if (state->stopping || request->cancelled->load(std::memory_order_relaxed)
                || request->generation != state->generation) continue;
            auto *receiver = state->receiver;
            // Posting is serialized with destruction. Qt discards queued
            // deliveries for a destroyed context; recheck generation on delivery
            // for replacement/cancellation after the worker has already posted.
            QMetaObject::invokeMethod(receiver, [receiver, state, generation = request->generation,
                                               result = std::move(result)]() mutable {
                {
                    std::lock_guard lock(state->mutex);
                    if (state->stopping || state->generation != generation) return;
                }
                auto *d = receiver->d.get();
                const int row = d->table->currentIndex().row();
                d->summary = std::move(result.summary);
                d->model->reset(d->urls, std::move(result.items));
                d->selectRow(row);
                d->completed = true;
                d->loading = false;
                d->updateSummary();
                QPointer<Inspector> guard(receiver);
                emit receiver->loadingChanged(false);
                if (!guard) return;
                {
                    std::lock_guard lock(state->mutex);
                    if (state->stopping || state->generation != generation) return;
                }
                emit receiver->metadataChanged();
            }, Qt::QueuedConnection);
        }
    }

    QLabel *field(QFormLayout *layout, const QString &title, const QString &name) {
        auto *value = new QLabel(q);
        value->setObjectName(name);
        value->setTextFormat(Qt::PlainText);
        value->setWordWrap(true);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        layout->addRow(title, value);
        return value;
    }
    void selectRow(int row) {
        if (model->rowCount()) table->setCurrentIndex(model->index(std::clamp(row, 0, model->rowCount() - 1), 0));
        updateDetails();
    }
    void updateDetails() {
        const int row = table->currentIndex().row();
        const bool valid = row >= 0 && row < urls.size();
        properties->setEnabled(valid && actionable(urls[row]));
        properties->setToolTip(valid ? Inspector::tr("Native properties for %1").arg(nameFor(urls[row])) : QString());
        for (auto *field : detailFields) field->setText(Inspector::tr("Unavailable"));
        if (!valid) return;
        detailFields["name"]->setText(nameFor(urls[row]));
        detailFields["location"]->setText(locationFor(urls[row]));
        if (row >= model->items.size()) {
            detailFields["status"]->setText(loading ? Inspector::tr("Reading metadata…") : Inspector::tr("Not inspected"));
            return;
        }
        const auto &item = model->items[row];
        detailFields["kind"]->setText(item.kind.isEmpty() ? Inspector::tr("Unavailable") : item.kind);
        detailFields["size"]->setText(item.size.isEmpty() ? Inspector::tr("Unavailable") : item.size);
        detailFields["modified"]->setText(dateText(item.modified));
        detailFields["created"]->setText(item.created.isValid() ? dateText(item.created) : Inspector::tr("Not provided or unavailable"));
        detailFields["tags"]->setText(item.tags.isEmpty() ? Inspector::tr("Unavailable") : item.tags);
        detailFields["status"]->setText(item.status);
    }
    QString dates(qsizetype count, const QDateTime &first, const QDateTime &last) const {
        if (!count) return Inspector::tr("Not provided or unavailable");
        const auto range = first == last ? dateText(first) : Inspector::tr("%1 – %2").arg(dateText(first), dateText(last));
        return count == summary.selectedCount ? range : Inspector::tr("%1 (%2 of %3 entries)").arg(range)
            .arg(count).arg(summary.selectedCount);
    }
    void updateSummary() {
        heading->setText(urls.isEmpty() ? Inspector::tr("No Selection")
            : urls.size() == 1 ? nameFor(urls.first()) : Inspector::tr("Selection Summary — %1 Items").arg(urls.size()));
        reveal->setEnabled(!urls.isEmpty());
        stop->setEnabled(loading);
        fields["selection"]->setText(Inspector::tr("%1 selected; %2 inspected").arg(summary.selectedCount).arg(summary.inspectedCount));
        fields["counts"]->setText(Inspector::tr("%1 files, %2 folders, %3 symbolic links, %4 special files; %5 remote/unsupported, %6 unavailable")
            .arg(summary.fileCount).arg(summary.folderCount).arg(summary.linkCount).arg(summary.otherCount)
            .arg(summary.remoteCount).arg(summary.unavailableCount));
        fields["size"]->setText(summary.sizeOverflow ? Inspector::tr("Known file sizes exceed the 64-bit total limit")
            : summary.fileCount ? Inspector::tr("%1 in %2 inspected regular files; folder contents excluded")
                .arg(byteText(summary.fileBytes)).arg(summary.fileCount)
            : Inspector::tr("No regular-file bytes measured; folder contents excluded"));
        QStringList kinds;
        for (auto it = summary.kinds.cbegin(); it != summary.kinds.cend() && kinds.size() < 6; ++it)
            kinds.append(Inspector::tr("%1 (%2)").arg(it.key()).arg(it.value()));
        if (summary.kinds.size() > 6) kinds.append(Inspector::tr("%1 other kinds").arg(summary.kinds.size() - 6));
        fields["kind"]->setText(kinds.isEmpty() ? Inspector::tr("Unavailable") : kinds.join(QStringLiteral(", ")));
        fields["modified"]->setText(dates(summary.modifiedCount, summary.earliestModified, summary.latestModified));
        fields["created"]->setText(dates(summary.createdCount, summary.earliestCreated, summary.latestCreated));
        fields["location"]->setText(summary.multipleLocations ? Inspector::tr("Multiple enclosing locations (inspected entries)")
            : summary.inspectedCount ? summary.location : urls.size() == 1 ? locationFor(urls.first()) : Inspector::tr("Not inspected"));
        QString tags = summary.tagsReadCount ? summary.commonTags.isEmpty()
            ? summary.tagSetsDiffer ? Inspector::tr("None shared") : Inspector::tr("None")
            : bounded(summary.commonTags.join(QStringLiteral(", "))) : Inspector::tr("Unavailable");
        if (summary.tagsReadCount != summary.selectedCount)
            tags += Inspector::tr(" (%1 of %2 tag sets read)").arg(summary.tagsReadCount).arg(summary.selectedCount);
        fields["tags"]->setText(tags);
        QString message;
        if (urls.isEmpty()) message = Inspector::tr("Select files or folders in this window.");
        else if (loading) message = Inspector::tr("Reading metadata…");
        else if (summary.cancelled) message = Inspector::tr("Metadata cancelled. Refresh to inspect this captured selection.");
        else if (summary.inspectedCount < summary.selectedCount)
            message = summary.timeLimited ? Inspector::tr("Partial summary: processing time limit reached; %1 entries not inspected.")
                    .arg(summary.selectedCount - summary.inspectedCount)
                : Inspector::tr("Partial summary: metadata is limited to %1 entries; %2 entries not inspected.")
                    .arg(Inspector::maximumMetadataItems).arg(summary.selectedCount - summary.inspectedCount);
        else message = Inspector::tr("Summary of selected entries. File kinds are inferred from names; native properties manage permissions and Open With.");
        if (summary.unreadableCount) message += Inspector::tr(" %1 entries have unreadable contents.").arg(summary.unreadableCount);
        status->setText(message);
        updateDetails();
    }
};

Inspector::Inspector(QWidget *parent) : QDialog(parent), d(std::make_unique<Private>(this))
{
    setObjectName(QStringLiteral("finderInspector"));
    setWindowTitle(tr("Inspector"));
    setModal(false);
    resize(620, 650);
    auto *layout = new QVBoxLayout(this);
    d->heading = new QLabel(this);
    d->heading->setObjectName(QStringLiteral("inspectorHeading"));
    d->heading->setTextFormat(Qt::PlainText);
    d->heading->setWordWrap(true);
    auto font = d->heading->font(); font.setBold(true); d->heading->setFont(font);
    layout->addWidget(d->heading);
    d->followBox = new QCheckBox(tr("Follow selection in this window"), this);
    d->followBox->setObjectName(QStringLiteral("inspectorFollowSelection"));
    d->followBox->setChecked(true);
    layout->addWidget(d->followBox);
    connect(d->followBox, &QCheckBox::toggled, this, &Inspector::setFollowsSelection);
    auto *tabs = new QTabWidget(this);
    tabs->setObjectName(QStringLiteral("inspectorTabs"));
    layout->addWidget(tabs, 1);
    auto *summaryPage = new QWidget(tabs);
    auto *summaryLayout = new QVBoxLayout(summaryPage);
    auto *summaryForm = new QFormLayout;
    summaryForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    summaryForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    summaryLayout->addLayout(summaryForm);
    const QList<QPair<QString, QString>> rows{{"selection", tr("Selection:")}, {"counts", tr("Known Counts:")},
        {"size", tr("Known Size:")}, {"kind", tr("Kinds (by name):")}, {"modified", tr("Modified:")},
        {"created", tr("Created:")}, {"location", tr("Where:")}, {"tags", tr("Common Tags (read-only):")}};
    for (const auto &row : rows) d->fields[row.first] = d->field(summaryForm, row.second, QStringLiteral("inspectorSummary_") + row.first);
    summaryLayout->addStretch();
    tabs->addTab(summaryPage, tr("Summary"));
    auto *itemsPage = new QWidget(tabs);
    auto *itemsLayout = new QVBoxLayout(itemsPage);
    d->table = new QTableView(itemsPage);
    d->table->setObjectName(QStringLiteral("inspectorItems"));
    d->model = new ItemsModel(d->table);
    d->table->setModel(d->model);
    d->table->setItemDelegateForColumn(2, new LinkDelegate(d->table));
    d->table->setSelectionBehavior(QAbstractItemView::SelectRows);
    d->table->setSelectionMode(QAbstractItemView::SingleSelection);
    d->table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    d->table->setWordWrap(false);
    d->table->verticalHeader()->hide();
    d->table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    d->table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    d->table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    d->table->setColumnWidth(1, 130);
    d->table->setColumnWidth(2, 160);
    itemsLayout->addWidget(d->table, 1);
    auto *detailForm = new QFormLayout;
    detailForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    detailForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    const QList<QPair<QString, QString>> detailRows{{"name", tr("Name:")}, {"kind", tr("Kind (by name):")},
        {"size", tr("Size:")}, {"modified", tr("Modified:")}, {"created", tr("Created:")},
        {"location", tr("Where:")}, {"tags", tr("Tags (read-only):")}, {"status", tr("Metadata:")}};
    for (const auto &row : detailRows) d->detailFields[row.first] = d->field(detailForm, row.second, QStringLiteral("inspectorItem_") + row.first);
    itemsLayout->addLayout(detailForm);
    tabs->addTab(itemsPage, tr("Items / Native Get Info"));
    d->status = new QLabel(this);
    d->status->setObjectName(QStringLiteral("inspectorStatus"));
    d->status->setTextFormat(Qt::PlainText);
    d->status->setWordWrap(true);
    layout->addWidget(d->status);
    d->properties = new QPushButton(tr("Native Get Info for Selected Item…"), this);
    d->properties->setObjectName(QStringLiteral("inspectorProperties"));
    d->properties->setAutoDefault(false);
    layout->addWidget(d->properties);
    d->reveal = new QPushButton(tr("Open Enclosing Folders"), this);
    d->reveal->setObjectName(QStringLiteral("inspectorReveal"));
    d->reveal->setAutoDefault(false);
    layout->addWidget(d->reveal);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto *refreshButton = buttons->addButton(tr("Refresh"), QDialogButtonBox::ActionRole);
    refreshButton->setObjectName(QStringLiteral("inspectorRefresh"));
    d->stop = buttons->addButton(tr("Stop"), QDialogButtonBox::ActionRole);
    d->stop->setObjectName(QStringLiteral("inspectorStop"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(refreshButton, &QPushButton::clicked, this, &Inspector::refresh);
    connect(d->stop, &QPushButton::clicked, this, &Inspector::cancelMetadata);
    connect(d->properties, &QPushButton::clicked, this, [this] {
        const int row = d->table->currentIndex().row();
        if (row >= 0 && row < d->urls.size() && actionable(d->urls[row])) emit propertiesRequested(d->urls[row]);
    });
    connect(d->table->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] { d->updateDetails(); });
    connect(d->table, &QTableView::clicked, this, [this](const QModelIndex &index) {
        if (index.column() == 2 && actionable(index.data(Qt::UserRole).toUrl())) emit propertiesRequested(index.data(Qt::UserRole).toUrl());
    });
    connect(d->table, &QTableView::activated, this, [this](const QModelIndex &index) {
        if (actionable(index.data(Qt::UserRole).toUrl())) emit propertiesRequested(index.data(Qt::UserRole).toUrl());
    });
    connect(d->reveal, &QPushButton::clicked, this, [this] { if (!d->urls.isEmpty()) emit revealRequested(d->urls); });
    d->updateSummary();
}

Inspector::~Inspector()
{
    std::lock_guard lock(d->state->mutex);
    d->state->stopping = true;
    d->state->receiver = nullptr;
    if (d->state->active) d->state->active->store(true, std::memory_order_relaxed);
    d->state->pending.reset();
}

void Inspector::setUrls(QList<QUrl> urls)
{
    Q_ASSERT(QThread::currentThread() == thread());
    d->latestSelection = urls;
    if (!d->follow || d->urls == urls) return;
    d->urls = std::move(urls);
    refresh();
}

QList<QUrl> Inspector::urls() const { return d->urls; }
bool Inspector::followsSelection() const { return d->follow; }
bool Inspector::isLoading() const { return d->loading; }
Inspector::Summary Inspector::summary() const { return d->summary; }

void Inspector::setFollowsSelection(bool follow)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (d->follow == follow) return;
    d->follow = follow;
    { const QSignalBlocker blocker(d->followBox); d->followBox->setChecked(follow); }
    QPointer<Inspector> guard(this);
    if (follow) setUrls(d->latestSelection);
    if (!guard) return;
    emit followsSelectionChanged(follow);
}

void Inspector::refresh()
{
    Q_ASSERT(QThread::currentThread() == thread());
    bool schedule = false;
    {
        std::lock_guard lock(d->state->mutex);
        const quint64 generation = ++d->state->generation;
        if (d->state->active) d->state->active->store(true, std::memory_order_relaxed);
        d->state->pending.reset();
        if (!d->urls.isEmpty()) {
            d->state->pending = Request{d->urls, generation, std::make_shared<std::atomic_bool>(false)};
            if (!d->state->scheduled) { d->state->scheduled = true; schedule = true; }
        }
    }
    d->loading = !d->urls.isEmpty();
    d->completed = !d->loading;
    d->summary = {};
    d->summary.selectedCount = d->urls.size();
    d->model->reset(d->urls);
    d->selectRow(0);
    d->updateSummary();
    if (schedule) metadataPool().start([state = d->state] { Private::work(state); });
    emit loadingChanged(d->loading);
}

void Inspector::cancelMetadata()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!d->loading) return;
    {
        std::lock_guard lock(d->state->mutex);
        ++d->state->generation;
        if (d->state->active) d->state->active->store(true, std::memory_order_relaxed);
        d->state->pending.reset();
    }
    d->loading = false;
    d->summary.cancelled = true;
    d->updateSummary();
    emit loadingChanged(false);
}

void Inspector::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    if (!d->loading && !d->completed && !d->urls.isEmpty()) refresh();
}

void Inspector::hideEvent(QHideEvent *event)
{
    QDialog::hideEvent(event);
    cancelMetadata();
}
