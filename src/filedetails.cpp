// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#include "filedetails.h"

#include <QAbstractListModel>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QHash>
#include <QHideEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStringDecoder>
#include <QTextEdit>
#include <QThreadPool>
#include <QVBoxLayout>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace {
using namespace FileDetails;
bool stopped(const Cancellation &cancelled) { return cancelled && cancelled->load(std::memory_order_relaxed); }
Error failure(ErrorCode code, const QString &message, int nativeCode = 0) { return {code, message, nativeCode}; }
Error cancelledError() { return failure(ErrorCode::Cancelled, QObject::tr("Metadata read cancelled.")); }
Error systemError(int code)
{
    if (code == ENODATA) return {};
    if (code == ENOTSUP || code == ENOSYS)
        return failure(ErrorCode::Unsupported, QObject::tr("This storage does not support user extended attributes."), code);
    if (code == ENOENT || code == ESTALE)
        return failure(ErrorCode::Missing, QObject::tr("The selected file no longer exists or is unavailable."), code);
    if (code == ELOOP || code == ENOTDIR)
        return failure(ErrorCode::UnsafeFile, QObject::tr("Metadata is unavailable on symbolic links or paths through symbolic links."), code);
    if (code == EPERM || code == EACCES || code == EROFS)
        return failure(ErrorCode::PermissionDenied, QObject::tr("Permission is required, or this storage is read-only."), code);
    if (code == E2BIG || code == ERANGE)
        return failure(ErrorCode::TooLarge, QObject::tr("The metadata exceeds the storage size limit."), code);
    return failure(ErrorCode::StorageError, QObject::tr("Could not access file metadata: %1")
        .arg(QString::fromLocal8Bit(std::strerror(code))), code);
}
struct Descriptor {
    int fd = -1;
    ~Descriptor() { if (fd >= 0) ::close(fd); }
    void reset(int next) { if (fd >= 0) ::close(fd); fd = next; }
};
Identity identityFor(const struct stat &info) { return {quint64(info.st_dev), quint64(info.st_ino), true}; }
Error openMetadata(const QUrl &url, Descriptor &handle, Identity &identity, bool &directory, const Cancellation &cancelled = {})
{
    if (!url.isValid() || url.isRelative() || url.isEmpty() || url.path().contains(QChar::Null)
        || !url.userInfo().isEmpty() || url.port() != -1 || url.hasQuery() || url.hasFragment())
        return failure(ErrorCode::InvalidInput, QObject::tr("Choose an absolute file URL without credentials, query or fragment."));
    if (!url.isLocalFile() || (!url.host().isEmpty() && url.host() != QStringLiteral("localhost")))
        return failure(ErrorCode::NonLocal, QObject::tr("Metadata editing is available for local file URLs only."));
    auto local = url;
    local.setHost({});
    const auto path = QFile::encodeName(local.toLocalFile());
    if (!path.startsWith('/') || path.contains('\0') || path.size() > 4096)
        return failure(ErrorCode::InvalidInput, QObject::tr("The local path is invalid or exceeds 4096 bytes."));
    const auto parts = path.split('/');
    if (parts.size() > 258 || parts.contains(".."))
        return failure(ErrorCode::InvalidInput, QObject::tr("Paths containing '..' or more than 256 components are unsupported."));
    if (stopped(cancelled)) return cancelledError();
    Descriptor pinned;
    pinned.fd = ::open("/", O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (pinned.fd < 0) return systemError(errno);
    for (qsizetype index = 0; index < parts.size(); ++index) {
        if (parts[index].isEmpty() || parts[index] == ".") continue;
        if (stopped(cancelled)) return cancelledError();
        bool last = true;
        for (qsizetype next = index + 1; next < parts.size(); ++next)
            if (!parts[next].isEmpty()) { last = false; break; }
        const int next = ::openat(pinned.fd, parts[index].constData(),
            O_PATH | O_NOFOLLOW | O_CLOEXEC | (last ? 0 : O_DIRECTORY));
        if (next < 0) return systemError(errno);
        pinned.reset(next);
    }
    struct stat info {};
    if (::fstat(pinned.fd, &info) != 0) return systemError(errno);
    if (S_ISLNK(info.st_mode)) return systemError(ELOOP);
    if (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode))
        return failure(ErrorCode::UnsafeFile, QObject::tr("Only regular files and folders support this editor; devices, FIFOs and sockets are not opened."));
    if (stopped(cancelled)) return cancelledError();
    // f*xattr rejects O_PATH descriptors. Reopen only the already verified
    // kernel-owned /proc/self/fd link, never the original mutable pathname.
    // This cannot open a replacement symlink/device after the safety check.
    const auto pinnedPath = QByteArray("/proc/self/fd/") + QByteArray::number(pinned.fd);
    handle.fd = ::open(pinnedPath.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | (S_ISDIR(info.st_mode) ? O_DIRECTORY : 0));
    if (handle.fd < 0) return systemError(errno);
    struct stat reopened {};
    if (::fstat(handle.fd, &reopened) != 0) return systemError(errno);
    identity = identityFor(info);
    if (identityFor(reopened) != identity)
        return failure(ErrorCode::Changed, QObject::tr("The selected inode changed while opening metadata. Refresh and try again."));
    directory = S_ISDIR(info.st_mode);
    return {};
}
Error readAttribute(int fd, const char *name, qsizetype maximum, QByteArray &value, const Cancellation &cancelled, bool *present = nullptr)
{
    for (int retry = 0; retry < 3; ++retry) {
        if (stopped(cancelled)) return cancelledError();
        const auto size = ::fgetxattr(fd, name, nullptr, 0);
        if (size < 0) {
            if (errno == ENODATA && present) *present = false;
            return systemError(errno);
        }
        if (present) *present = true;
        if (size > maximum) return failure(ErrorCode::TooLarge, QObject::tr("An existing metadata value exceeds the editor's size limit."));
        QByteArray bytes(size, Qt::Uninitialized);
        if (stopped(cancelled)) return cancelledError();
        // A one-byte buffer even for size zero detects a value growing between
        // probes, instead of mistaking a second size query for returned bytes.
        char empty = 0;
        const auto actual = ::fgetxattr(fd, name, size ? bytes.data() : &empty, size ? size_t(size) : 1);
        if (actual < 0) {
            if (errno == ERANGE) continue;
            if (errno == ENODATA && present) *present = false;
            return systemError(errno);
        }
        if (actual > size) continue;
        bytes.resize(actual);
        value = std::move(bytes);
        return {};
    }
    return failure(ErrorCode::Changed, QObject::tr("The metadata changed repeatedly while being read. Refresh and try again."));
}
Error validateComment(const QString &comment)
{
    // Check character count before encoding, to bound the temporary allocation.
    if (comment.size() > maximumCommentBytes || comment.toUtf8().size() > maximumCommentBytes)
        return failure(ErrorCode::TooLarge, QObject::tr("Comments must contain at most 16384 UTF-8 bytes."));
    if (comment.contains(QChar::Null) || QString::fromUtf8(comment.toUtf8()) != comment)
        return failure(ErrorCode::InvalidInput, QObject::tr("Comments must be valid Unicode without NUL characters."));
    return {};
}
Error writeAttribute(const QUrl &url, const char *name, const QByteArray &value, Identity expected)
{
    Descriptor handle;
    Identity current;
    bool directory = false;
    auto error = openMetadata(url, handle, current, directory);
    if (!error.ok()) return error;
    if (expected.valid && current != expected)
        return failure(ErrorCode::Changed, QObject::tr("The selected file was replaced. Refresh before editing its metadata."));
    const int result = value.isEmpty() ? ::fremovexattr(handle.fd, name)
        : ::fsetxattr(handle.fd, name, value.constData(), size_t(value.size()), 0);
    return result == 0 ? Error{} : systemError(errno);
}
QString errorText(const Details &details)
{
    QStringList messages;
    for (const auto *error : {&details.error, &details.extensionError, &details.commentError})
        if (!error->ok() && !messages.contains(error->message)) messages.append(error->message);
    return messages.join(' ');
}
QThreadPool &metadataPool()
{
    static QThreadPool pool;
    static const bool configured = [] { pool.setMaxThreadCount(2); return true; }();
    Q_UNUSED(configured)
    return pool;
}
} // namespace

FileDetails::Details FileDetails::read(const QUrl &url, const Cancellation &cancelled)
{
    Details details;
    details.url = url;
    Descriptor handle;
    details.error = openMetadata(url, handle, details.identity, details.directory, cancelled);
    if (!details.error.ok()) { details.identity = {}; return details; }
    QByteArray extension;
    bool hasExtension = false;
    details.extensionError = readAttribute(handle.fd, hideExtensionAttribute, 1, extension, cancelled, &hasExtension);
    if (details.extensionError.ok() && hasExtension) {
        if (extension == "1") details.extensionOverride = ExtensionOverride::Hide;
        else if (extension == "0") details.extensionOverride = ExtensionOverride::Show;
        else details.extensionError = failure(ErrorCode::InvalidMetadata, QObject::tr("The stored Hide extension value is invalid; the default presentation is used."));
    }
    QByteArray comment;
    details.commentError = readAttribute(handle.fd, commentAttribute, maximumCommentBytes, comment, cancelled);
    if (details.commentError.ok()) {
        QStringDecoder decoder(QStringDecoder::Utf8);
        const QString text = decoder(comment);
        if (decoder.hasError() || comment.contains('\0'))
            details.commentError = failure(ErrorCode::InvalidMetadata, QObject::tr("The existing comment is not valid UTF-8 text without NUL characters."));
        else details.comment = text;
    }
    return details;
}
FileDetails::Batch FileDetails::readBatch(const QList<QUrl> &urls, Limits limits, const Cancellation &cancelled)
{
    Batch batch;
    batch.requestedCount = urls.size();
    const auto maximum = std::clamp<qsizetype>(limits.maxItems, 0, maximumMetadataItems);
    const auto count = std::min(urls.size(), maximum);
    const auto time = std::clamp(limits.maxTimeMs, 0, maximumTimeMs);
    QElapsedTimer timer;
    timer.start();
    batch.items.reserve(count);
    for (qsizetype index = 0; index < count; ++index) {
        if (stopped(cancelled)) { batch.cancelled = true; break; }
        if (timer.elapsed() >= time) { batch.limited = true; break; }
        batch.items.append(read(urls[index], cancelled));
    }
    batch.cancelled = batch.cancelled || stopped(cancelled);
    batch.limited = batch.limited || (!batch.cancelled && batch.items.size() < urls.size());
    return batch;
}
FileDetails::Error FileDetails::setExtensionOverride(const QUrl &url, ExtensionOverride value, Identity expected)
{
    if (value != ExtensionOverride::Inherit && value != ExtensionOverride::Show && value != ExtensionOverride::Hide)
        return failure(ErrorCode::InvalidInput, QObject::tr("The extension preference is invalid."));
    return writeAttribute(url, hideExtensionAttribute, value == ExtensionOverride::Inherit ? QByteArray{}
        : value == ExtensionOverride::Hide ? QByteArray("1") : QByteArray("0"), expected);
}
FileDetails::Error FileDetails::setComment(const QUrl &url, const QString &comment, Identity expected)
{
    const auto error = validateComment(comment);
    return error.ok() ? writeAttribute(url, commentAttribute, comment.toUtf8(), expected) : error;
}
QString FileDetails::displayName(const QString &name, bool directory, const QString &knownSuffix,
                               ExtensionOverride value, bool showAllExtensions)
{
    if (directory || showAllExtensions || value == ExtensionOverride::Show) return name;
    if (!knownSuffix.isEmpty() && !knownSuffix.contains('/') && !knownSuffix.contains(QChar::Null)
        && name.size() > knownSuffix.size() + 1 && name.endsWith('.' + knownSuffix, Qt::CaseInsensitive))
        return name.left(name.size() - knownSuffix.size() - 1);
    if (value == ExtensionOverride::Hide) {
        const auto dot = name.lastIndexOf('.');
        if (dot > 0 && dot < name.size() - 1) return name.left(dot);
    }
    return name;
}

namespace {
struct ReadRequest {
    QList<QUrl> urls;
    Limits limits;
    qsizetype requestedCount = 0;
    quint64 generation = 0;
    Cancellation cancelled;
};
struct WriteRequest {
    QUrl url;
    Identity expected;
    ExtensionOverride extension = ExtensionOverride::Inherit;
    QString comment;
    bool writesComment = false;
    quint64 id = 0, selectionGeneration = 0;
};
struct WorkerState {
    std::mutex mutex;
    std::optional<ReadRequest> pendingRead;
    std::optional<WriteRequest> pendingWrite;
    Cancellation activeRead;
    FileDetailsCache *receiver = nullptr;
    // Read cancellation must not invalidate a write's readback. Only selecting
    // a new URL set advances selectionGeneration, including A -> B -> A.
    quint64 generation = 0, selectionGeneration = 0;
    bool scheduled = false, stopping = false;
};
class DetailsModel final : public QAbstractListModel {
public:
    explicit DetailsModel(QObject *parent) : QAbstractListModel(parent) {}
    QList<QUrl> urls;
    QHash<QUrl, Details> entries;
    bool pending = false;
    void resetUrls(QList<QUrl> next) {
        beginResetModel(); urls = std::move(next); entries.clear(); endResetModel();
    }
    void changed() { if (!urls.isEmpty()) emit dataChanged(index(0), index(int(urls.size()) - 1)); }
    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : int(urls.size()); }
    QVariant data(const QModelIndex &index, int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= urls.size() || index.column() != 0) return {};
        const auto &url = urls[index.row()];
        if (role == FileDetailsCache::UrlRole) return url;
        if (role == Qt::DisplayRole || role == Qt::AccessibleTextRole) return url.adjusted(QUrl::StripTrailingSlash).fileName();
        if (role == FileDetailsCache::PendingRole) return pending;
        const auto found = entries.constFind(url);
        if (found == entries.cend()) {
            if (role == FileDetailsCache::ErrorRole || role == Qt::ToolTipRole)
                return pending ? QObject::tr("Reading metadata…") : QObject::tr("Metadata was not inspected. Refresh to retry.");
            return {};
        }
        const auto &details = *found;
        if (role == FileDetailsCache::ErrorRole || role == Qt::ToolTipRole) return errorText(details);
        if (role == FileDetailsCache::ExtensionOverrideRole) return int(details.extensionOverride);
        if (role == FileDetailsCache::CommentRole) return details.commentError.ok() && details.available() ? QVariant(details.comment) : QVariant{};
        if (role == FileDetailsCache::DeviceRole) return details.identity.valid ? QVariant::fromValue(details.identity.device) : QVariant{};
        if (role == FileDetailsCache::InodeRole) return details.identity.valid ? QVariant::fromValue(details.identity.inode) : QVariant{};
        return {};
    }
};
}

class FileDetailsCache::Private {
public:
    explicit Private(FileDetailsCache *owner) : q(owner), state(std::make_shared<WorkerState>()), model(new DetailsModel(owner)) {
        state->receiver = owner;
    }
    FileDetailsCache *q;
    std::shared_ptr<WorkerState> state;
    DetailsModel *model;
    Limits limits;
    Batch batch;
    qsizetype requestedCount = 0;
    quint64 nextId = 0;
    bool writing = false;

    static void work(const std::shared_ptr<WorkerState> &state) {
        for (;;) {
            std::optional<ReadRequest> read;
            std::optional<WriteRequest> write;
            {
                std::lock_guard lock(state->mutex);
                if (state->stopping || (!state->pendingWrite && !state->pendingRead)) {
                    state->scheduled = false; state->activeRead.reset(); return;
                }
                if (state->pendingWrite) { write = std::move(state->pendingWrite); state->pendingWrite.reset(); }
                else { read = std::move(state->pendingRead); state->pendingRead.reset(); state->activeRead = read->cancelled; }
            }
            if (write) {
                const auto error = write->writesComment ? FileDetails::setComment(write->url, write->comment, write->expected)
                    : FileDetails::setExtensionOverride(write->url, write->extension, write->expected);
                const auto details = error.ok() ? FileDetails::read(write->url) : Details{};
                std::lock_guard lock(state->mutex);
                if (state->stopping) continue;
                auto *receiver = state->receiver;
                QMetaObject::invokeMethod(receiver, [receiver, state, write = std::move(*write), error, details] {
                    auto *d = receiver->d.get();
                    bool current;
                    { std::lock_guard lock(state->mutex); current = write.selectionGeneration == state->selectionGeneration; }
                    if (current && error.ok() && details.identity == write.expected && d->model->urls.contains(write.url))
                        d->model->entries.insert(write.url, details);
                    d->writing = false;
                    QPointer<FileDetailsCache> guard(receiver);
                    d->model->changed();
                    if (!guard) return;
                    emit receiver->writingChanged(false);
                    if (!guard) return;
                    emit receiver->writeFinished(write.id, write.url, error);
                    if (!guard) return;
                    if (error.ok()) emit receiver->detailsChanged({write.url});
                }, Qt::QueuedConnection);
            } else {
                auto batch = FileDetails::readBatch(read->urls, read->limits, read->cancelled);
                batch.requestedCount = read->requestedCount;
                batch.limited = batch.limited || (!batch.cancelled && batch.items.size() < batch.requestedCount);
                std::lock_guard lock(state->mutex);
                if (state->stopping || stopped(read->cancelled) || read->generation != state->generation) continue;
                auto *receiver = state->receiver;
                QMetaObject::invokeMethod(receiver, [receiver, state, generation = read->generation, batch = std::move(batch)]() mutable {
                    {
                        std::lock_guard lock(state->mutex);
                        if (state->stopping || generation != state->generation) return;
                    }
                    auto *d = receiver->d.get();
                    d->model->entries.clear();
                    for (const auto &details : batch.items) d->model->entries.insert(details.url, details);
                    d->batch = std::move(batch);
                    d->model->pending = false;
                    const auto completed = d->batch;
                    const auto urls = d->model->urls;
                    QPointer<FileDetailsCache> guard(receiver);
                    d->model->changed();
                    if (!guard) return;
                    {
                        std::lock_guard lock(state->mutex);
                        if (generation != state->generation) return;
                    }
                    emit receiver->loadingChanged(false);
                    if (!guard) return;
                    {
                        std::lock_guard lock(state->mutex);
                        if (generation != state->generation) return;
                    }
                    emit receiver->detailsChanged(urls);
                    if (!guard) return;
                    {
                        std::lock_guard lock(state->mutex);
                        if (generation != state->generation) return;
                    }
                    emit receiver->batchFinished(completed);
                }, Qt::QueuedConnection);
            }
        }
    }
    void scheduleLocked() {
        if (!state->scheduled) {
            state->scheduled = true;
            metadataPool().start([state = state] { work(state); });
        }
    }
    void cancelLocked() {
        ++state->generation;
        if (state->activeRead) state->activeRead->store(true, std::memory_order_relaxed);
        if (state->pendingRead) state->pendingRead->cancelled->store(true, std::memory_order_relaxed);
        state->pendingRead.reset();
    }
    quint64 write(WriteRequest request) {
        Error rejection;
        if (writing) rejection = failure(ErrorCode::Busy, FileDetailsCache::tr("A metadata write is already in progress."));
        const auto details = model->entries.constFind(request.url);
        if (rejection.ok() && (details == model->entries.cend() || !details->available()))
            rejection = failure(ErrorCode::Changed, FileDetailsCache::tr("Read this file's metadata before editing it."));
        if (rejection.ok() && request.writesComment) rejection = validateComment(request.comment);
        if (!rejection.ok()) {
            QMetaObject::invokeMethod(q, [q = q, url = request.url, rejection] { emit q->writeFinished(0, url, rejection); }, Qt::QueuedConnection);
            return 0;
        }
        request.expected = details->identity;
        request.id = ++nextId;
        const auto id = request.id;
        const bool loading = model->pending;
        {
            std::lock_guard lock(state->mutex);
            cancelLocked();
            request.selectionGeneration = state->selectionGeneration;
            state->pendingWrite = std::move(request);
            scheduleLocked();
        }
        writing = true;
        model->pending = false;
        QPointer<FileDetailsCache> guard(q);
        auto *owner = q;
        model->changed();
        if (!guard) return id;
        if (loading) emit owner->loadingChanged(false);
        if (guard) emit owner->writingChanged(true);
        return id;
    }
};

FileDetailsCache::FileDetailsCache(QObject *parent) : QObject(parent), d(std::make_unique<Private>(this)) {}
FileDetailsCache::~FileDetailsCache()
{
    std::lock_guard lock(d->state->mutex);
    d->cancelLocked();
    d->state->stopping = true;
    d->state->receiver = nullptr;
    d->state->pendingWrite.reset();
}
void FileDetailsCache::setUrls(QList<QUrl> urls, Limits limits)
{
    d->requestedCount = urls.size();
    urls = urls.mid(0, maximumMetadataItems);
    d->limits = limits;
    {
        std::lock_guard lock(d->state->mutex);
        ++d->state->selectionGeneration;
        d->cancelLocked();
    }
    QPointer<FileDetailsCache> guard(this);
    d->model->resetUrls(std::move(urls));
    if (guard) refresh();
}
QList<QUrl> FileDetailsCache::urls() const { return d->model->urls; }
std::optional<FileDetails::Details> FileDetailsCache::cached(const QUrl &url) const
{
    const auto found = d->model->entries.constFind(url);
    return found == d->model->entries.cend() ? std::optional<Details>{} : *found;
}
QAbstractItemModel *FileDetailsCache::model() const { return d->model; }
bool FileDetailsCache::isLoading() const { return d->model->pending; }
bool FileDetailsCache::isWriting() const { return d->writing; }
FileDetails::Batch FileDetailsCache::lastBatch() const { return d->batch; }
quint64 FileDetailsCache::setExtensionOverride(const QUrl &url, ExtensionOverride value)
{
    WriteRequest request;
    request.url = url; request.extension = value;
    return d->write(std::move(request));
}
quint64 FileDetailsCache::setComment(const QUrl &url, const QString &comment)
{
    WriteRequest request;
    request.url = url; request.comment = comment; request.writesComment = true;
    return d->write(std::move(request));
}
void FileDetailsCache::refresh()
{
    const bool loading = d->model->pending;
    quint64 generation;
    {
        std::lock_guard lock(d->state->mutex);
        d->cancelLocked();
        generation = d->state->generation;
        if (!d->model->urls.isEmpty()) {
            d->state->pendingRead = ReadRequest{d->model->urls, d->limits, d->requestedCount,
                d->state->generation, std::make_shared<std::atomic_bool>(false)};
            d->scheduleLocked();
        }
    }
    d->model->pending = !d->model->urls.isEmpty();
    d->batch = {};
    d->batch.requestedCount = d->requestedCount;
    const auto state = d->state;
    const bool pending = d->model->pending;
    QPointer<FileDetailsCache> guard(this);
    d->model->changed();
    if (!guard) return;
    { std::lock_guard lock(state->mutex); if (generation != state->generation) return; }
    if (loading != pending) emit loadingChanged(pending);
}
void FileDetailsCache::cancel()
{
    quint64 generation;
    {
        std::lock_guard lock(d->state->mutex);
        d->cancelLocked();
        generation = d->state->generation;
    }
    if (!d->model->pending) return;
    d->model->pending = false;
    d->batch.cancelled = true;
    const auto batch = d->batch;
    const auto state = d->state;
    QPointer<FileDetailsCache> guard(this);
    d->model->changed();
    if (!guard) return;
    emit loadingChanged(false);
    if (!guard) return;
    { std::lock_guard lock(state->mutex); if (generation != state->generation) return; }
    emit batchFinished(batch);
}

class FileDetailsDialog::Private {
public:
    explicit Private(FileDetailsDialog *owner) : q(owner), cache(new FileDetailsCache(owner)) {}
    FileDetailsDialog *q;
    FileDetailsCache *cache;
    QUrl url;
    QString suffix;
    bool showAll = false, dirty = false;
    quint64 urlRevision = 0, writeRevision = 0;
    QString operationMessage;
    Error lastWriteError;
    QLabel *heading = nullptr, *status = nullptr, *preference = nullptr;
    QLineEdit *name = nullptr;
    QCheckBox *hide = nullptr;
    QTextEdit *comment = nullptr;
    QPushButton *rename = nullptr, *defaults = nullptr, *save = nullptr, *properties = nullptr;
    void update() {
        const auto details = cache->cached(url);
        const bool available = details && details->available();
        const bool busy = cache->isWriting();
        const bool editable = available && !busy && !cache->isLoading();
        name->setEnabled(available && !busy);
        rename->setEnabled(available && !busy && name->text() != url.adjusted(QUrl::StripTrailingSlash).fileName());
        hide->setEnabled(editable && !details->directory);
        defaults->setEnabled(editable && !details->directory);
        comment->setEnabled(editable);
        save->setEnabled(editable && dirty);
        properties->setEnabled(url.isValid() && !url.isRelative() && !url.isEmpty());
        if (!available) {
            status->setText(!operationMessage.isEmpty() ? operationMessage : cache->isLoading() ? FileDetailsDialog::tr("Reading file metadata…")
                : details ? errorText(*details) : FileDetailsDialog::tr("Metadata was not inspected. Refresh to retry."));
            return;
        }
        const QSignalBlocker block(hide);
        hide->setChecked(details->extensionOverride == ExtensionOverride::Hide
            || (details->extensionOverride == ExtensionOverride::Inherit &&
                FileDetails::displayName(name->text(), details->directory, suffix, ExtensionOverride::Inherit, false) != name->text()));
        if (!dirty) {
            const QSignalBlocker blockComment(comment);
            comment->setPlainText(details->comment);
        }
        preference->setText(showAll ? FileDetailsDialog::tr("Show all filename extensions is enabled; the full name is displayed regardless of this file's preference.")
            : details->extensionOverride == ExtensionOverride::Inherit ? FileDetailsDialog::tr("Using the default filename extension preference.")
            : FileDetailsDialog::tr("This file has an explicit filename extension preference."));
        const auto errors = errorText(*details);
        status->setText(!operationMessage.isEmpty() ? operationMessage : busy ? FileDetailsDialog::tr("Saving file metadata…") : !lastWriteError.ok() ? lastWriteError.message : !errors.isEmpty() ? errors
            : FileDetailsDialog::tr("Metadata is stored on this Linux inode. Native properties manage permissions and Open With."));
    }
};
FileDetailsDialog::FileDetailsDialog(QWidget *parent) : QDialog(parent), d(std::make_unique<Private>(this))
{
    setObjectName(QStringLiteral("finderFileDetails"));
    setWindowTitle(tr("Get Info"));
    setModal(false);
    resize(440, 640);
    auto *layout = new QVBoxLayout(this);
    d->heading = new QLabel(this);
    d->heading->setTextFormat(Qt::PlainText);
    d->heading->setWordWrap(true);
    auto font = d->heading->font(); font.setBold(true); d->heading->setFont(font);
    layout->addWidget(d->heading);
    auto *names = new QGroupBox(tr("Name && Extension"), this);
    auto *nameLayout = new QFormLayout(names);
    d->name = new QLineEdit(names);
    d->name->setObjectName(QStringLiteral("fileDetailsName"));
    d->name->setAccessibleName(tr("Full filename including extension"));
    d->name->setMaxLength(255);
    nameLayout->addRow(tr("Name:"), d->name);
    d->rename = new QPushButton(tr("Rename…"), names);
    d->rename->setObjectName(QStringLiteral("fileDetailsRename"));
    nameLayout->addRow(d->rename);
    d->hide = new QCheckBox(tr("Hide extension"), names);
    d->hide->setObjectName(QStringLiteral("fileDetailsHideExtension"));
    nameLayout->addRow(d->hide);
    d->defaults = new QPushButton(tr("Use default"), names);
    d->defaults->setObjectName(QStringLiteral("fileDetailsUseDefault"));
    nameLayout->addRow(d->defaults);
    d->preference = new QLabel(names);
    d->preference->setTextFormat(Qt::PlainText);
    d->preference->setWordWrap(true);
    d->preference->setMinimumHeight(d->preference->fontMetrics().lineSpacing() * 3);
    nameLayout->addRow(d->preference);
    layout->addWidget(names);
    auto *comments = new QGroupBox(tr("Comments"), this);
    auto *commentLayout = new QVBoxLayout(comments);
    d->comment = new QTextEdit(comments);
    d->comment->setObjectName(QStringLiteral("fileDetailsComment"));
    d->comment->setAccessibleName(tr("File comment"));
    d->comment->setAcceptRichText(false);
    d->comment->setPlaceholderText(tr("Add a comment to this file"));
    commentLayout->addWidget(d->comment);
    d->save = new QPushButton(tr("Save Comment"), comments);
    d->save->setObjectName(QStringLiteral("fileDetailsSaveComment"));
    commentLayout->addWidget(d->save);
    layout->addWidget(comments, 1);
    d->status = new QLabel(this);
    d->status->setObjectName(QStringLiteral("fileDetailsStatus"));
    d->status->setTextFormat(Qt::PlainText);
    d->status->setWordWrap(true);
    d->status->setMinimumHeight(d->status->fontMetrics().lineSpacing() * 3);
    layout->addWidget(d->status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    d->properties = buttons->addButton(tr("Native Properties…"), QDialogButtonBox::ActionRole);
    d->properties->setObjectName(QStringLiteral("fileDetailsNativeProperties"));
    auto *refreshButton = buttons->addButton(tr("Refresh"), QDialogButtonBox::ActionRole);
    refreshButton->setObjectName(QStringLiteral("fileDetailsRefresh"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &FileDetailsDialog::reject);
    connect(refreshButton, &QPushButton::clicked, this, &FileDetailsDialog::refresh);
    connect(d->properties, &QPushButton::clicked, this, [this] { emit propertiesRequested(d->url); });
    connect(d->name, &QLineEdit::textChanged, this, [this] { d->update(); });
    connect(d->rename, &QPushButton::clicked, this, [this] {
        const auto name = d->name->text();
        if (name.isEmpty() || name == "." || name == ".." || name.contains('/') || name.contains(QChar::Null)
            || name.toUtf8().size() > 255) {
            d->status->setText(tr("Enter one filename of at most 255 UTF-8 bytes, without '/' or NUL."));
            return;
        }
        d->status->setText(tr("Rename requested. The native file operation will report its result."));
        emit renameRequested(d->url, name);
    });
    connect(d->hide, &QCheckBox::toggled, this, [this](bool checked) {
        d->cache->setExtensionOverride(d->url, checked ? ExtensionOverride::Hide : ExtensionOverride::Show);
    });
    connect(d->defaults, &QPushButton::clicked, this, [this] { d->cache->setExtensionOverride(d->url, ExtensionOverride::Inherit); });
    connect(d->comment, &QTextEdit::textChanged, this, [this] { d->dirty = true; d->update(); });
    connect(d->save, &QPushButton::clicked, this, [this] { d->cache->setComment(d->url, d->comment->toPlainText()); });
    connect(d->cache, &FileDetailsCache::loadingChanged, this, [this] { d->update(); });
    connect(d->cache, &FileDetailsCache::writingChanged, this, [this](bool writing) {
        d->operationMessage.clear();
        if (writing) { d->lastWriteError = {}; d->writeRevision = d->urlRevision; }
        d->update();
    });
    connect(d->cache, &FileDetailsCache::detailsChanged, this, [this] { d->update(); });
    connect(d->cache, &FileDetailsCache::writeFinished, this, [this](quint64 id, const QUrl &url, const Error &error) {
        if (url != d->url || (id && d->writeRevision != d->urlRevision)) return;
        d->lastWriteError = error;
        if (!error.ok()) { d->update(); return; }
        // Only clear the draft after a comment write, not when toggling Hide
        // extension. Equality also preserves a draft edited during completion.
        const auto details = d->cache->cached(url);
        if (details && details->comment == d->comment->toPlainText()) d->dirty = false;
        d->update();
        emit metadataSaved(url);
    });
    d->update();
}
FileDetailsDialog::~FileDetailsDialog() = default;
void FileDetailsDialog::setUrl(const QUrl &url)
{
    ++d->urlRevision;
    d->url = url;
    d->dirty = false;
    d->lastWriteError = {};
    d->operationMessage.clear();
    const auto name = url.adjusted(QUrl::StripTrailingSlash).fileName();
    d->heading->setText(name.isEmpty() ? tr("Get Info") : name);
    {
        const QSignalBlocker block(d->name);
        d->name->setText(name);
    }
    {
        const QSignalBlocker block(d->comment);
        d->comment->clear();
    }
    d->cache->setUrls(url.isEmpty() ? QList<QUrl>{} : QList<QUrl>{url});
    d->update();
}
QUrl FileDetailsDialog::url() const { return d->url; }
FileDetailsCache *FileDetailsDialog::metadataCache() const { return d->cache; }
void FileDetailsDialog::setExtensionPresentation(QString knownSuffix, bool showAllExtensions)
{
    d->suffix = knownSuffix.left(255);
    d->showAll = showAllExtensions;
    d->update();
}
void FileDetailsDialog::showOperationMessage(const QString &message) { d->operationMessage = message; d->update(); }
void FileDetailsDialog::refresh() { d->lastWriteError = {}; d->operationMessage.clear(); d->cache->refresh(); d->update(); }
void FileDetailsDialog::reject()
{
    if (!isWindow()) { emit closeRequested(); return; }
    if (d->cache->isWriting()) {
        showOperationMessage(tr("A metadata save is in progress. Wait for it to finish before closing."));
        return;
    }
    QDialog::reject();
}
void FileDetailsDialog::closeEvent(QCloseEvent *event)
{
    if (!isWindow()) {
        event->ignore();
        emit closeRequested();
        return;
    }
    if (d->cache->isWriting()) {
        event->ignore();
        showOperationMessage(tr("A metadata save is in progress. Wait for it to finish before closing."));
        return;
    }
    QDialog::closeEvent(event);
}
void FileDetailsDialog::hideEvent(QHideEvent *event) { d->cache->cancel(); QDialog::hideEvent(event); }
void FileDetailsDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    if (!d->url.isEmpty() && !d->cache->isLoading() && !d->cache->cached(d->url)) refresh();
}
