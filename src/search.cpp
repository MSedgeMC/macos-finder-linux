// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#include "search.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QThread>
#include <QMimeDatabase>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <optional>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <vector>

namespace {
struct Request {
    quint64 generation;
    QUrl root;
    QString query;
    LocalFileSearch::Options options;
    std::shared_ptr<std::atomic_bool> cancel;
};

struct Shared {
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<Request> pending;
    std::shared_ptr<std::atomic_bool> active;
    bool stopping = false;
};

struct Mount {
    QByteArray path;
    bool local;
};

QByteArray mountPath(QByteArray path) {
    path.replace("\\040", " ");
    path.replace("\\011", "\t");
    path.replace("\\012", "\n");
    path.replace("\\134", "\\");
    return path;
}

// Inspect mount metadata before opening a mount. QStorageInfo::refresh/statfs
// can itself block on an unavailable network filesystem, so it is not used.
bool loadMounts(QList<Mount> &mounts) {
    QFile file(QStringLiteral("/proc/self/mountinfo"));
    if (!file.open(QIODevice::ReadOnly)) return false;
    const auto data = file.read(4 * 1024 * 1024);
    const QList<QByteArray> localTypes{"ext2", "ext3", "ext4", "btrfs", "xfs", "f2fs", "zfs",
        "tmpfs", "ramfs", "overlay", "ubifs", "ufs", "jfs", "reiserfs", "ntfs", "ntfs3",
        "vfat", "exfat", "iso9660", "udf", "squashfs", "fuseblk", "bcachefs"};
    for (const auto &line : data.split('\n')) {
        const auto separator = line.indexOf(" - ");
        if (separator < 0) continue;
        const auto fields = line.left(separator).split(' ');
        const auto trailing = line.mid(separator + 3).split(' ');
        if (fields.size() < 6 || trailing.isEmpty()) continue;
        mounts.append({mountPath(fields[4]), localTypes.contains(trailing[0])});
    }
    std::stable_sort(mounts.begin(), mounts.end(), [](const Mount &a, const Mount &b) {
        return a.path.size() > b.path.size();
    });
    return std::any_of(mounts.cbegin(), mounts.cend(), [](const Mount &mount) { return mount.path == "/"; });
}

bool localPath(const QByteArray &path, const QList<Mount> &mounts) {
    for (const auto &mount : mounts) {
        if (path == mount.path || (mount.path == "/") || path.startsWith(mount.path + '/'))
            return mount.local;
    }
    return false;
}

struct DirectoryCloser {
    void operator()(DIR *directory) const { if (directory) closedir(directory); }
};
using Directory = std::unique_ptr<DIR, DirectoryCloser>;
struct Frame {
    QByteArray path;
    Directory directory;
};

// Walk components relative to already-open directory descriptors. O_NOFOLLOW
// prevents a directory (including an ancestor of the root) being replaced with
// a symlink that leads outside the selected tree or onto a network mount.
int openRoot(const QByteArray &path, const QList<Mount> &mounts,
             const std::shared_ptr<std::atomic_bool> &cancel) {
    if (!localPath("/", mounts)) { errno = EXDEV; return -1; }
    int descriptor = open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) return -1;
    QByteArray current;
    for (const auto &part : path.split('/')) {
        if (part.isEmpty()) continue;
        if (cancel->load(std::memory_order_relaxed)) { close(descriptor); errno = ECANCELED; return -1; }
        current += '/' + part;
        if (!localPath(current, mounts)) { close(descriptor); errno = EXDEV; return -1; }
        const int next = openat(descriptor, part.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        const int failure = errno;
        close(descriptor);
        if (next < 0) { errno = failure; return -1; }
        descriptor = next;
    }
    return descriptor;
}

QString readableError(int code) {
    if (code == EXDEV) return QStringLiteral("Recursive search is available on local folders. Network and virtual mounts are skipped.");
    if (code == ELOOP || code == ENOTDIR) return QStringLiteral("Choose a folder without symbolic links in its path for recursive search.");
    if (code == EACCES || code == EPERM) return QStringLiteral("The search folder is not readable.");
    if (code == ENOENT) return QStringLiteral("The search folder no longer exists.");
    return QStringLiteral("Could not read the search folder: %1").arg(QString::fromLocal8Bit(std::strerror(code)));
}
}

class LocalFileSearchWorker : public QThread {
    Q_OBJECT
public:
    explicit LocalFileSearchWorker(std::shared_ptr<Shared> state) : state(std::move(state)) {}
signals:
    void batchReady(quint64 generation, const QList<QUrl> &files);
    void complete(quint64 generation, int count, bool limited, int skipped,
                  const QString &error, bool canceled);
protected:
    void run() override {
        while (true) {
            std::optional<Request> request;
            {
                std::unique_lock lock(state->mutex);
                state->wake.wait(lock, [this] { return state->stopping || state->pending.has_value(); });
                if (state->stopping) return;
                request = std::move(state->pending);
                state->pending.reset();
                state->active = request->cancel;
            }
            scan(*request);
            std::lock_guard lock(state->mutex);
            if (state->active == request->cancel) state->active.reset();
        }
    }
private:
    std::shared_ptr<Shared> state;

    void scan(const Request &request) {
        int count = 0, examined = 0, skipped = 0;
        bool limited = false;
        QString failure;
        QList<QUrl> batch;
        QElapsedTimer elapsed, lastBatch;
        elapsed.start();
        lastBatch.start();
        const auto canceled = [&] { return request.cancel->load(std::memory_order_relaxed); };
        const auto finish = [&] {
            if (!canceled() && !batch.isEmpty()) emit batchReady(request.generation, batch);
            emit complete(request.generation, count, limited, skipped, failure, canceled());
        };
        if (canceled()) { finish(); return; }
        if (request.query.isEmpty() && !request.options.hasCriteria()) { finish(); return; }
        if (!request.root.isLocalFile() || (!request.root.host().isEmpty() && request.root.host() != "localhost")) {
            failure = QStringLiteral("Recursive filename search is available for local folders.");
            finish();
            return;
        }
        const auto root = QDir::cleanPath(request.root.toLocalFile());
        if (!QDir::isAbsolutePath(root)) {
            failure = QStringLiteral("Choose an absolute local folder for search.");
            finish();
            return;
        }
        QList<Mount> mounts;
        if (!loadMounts(mounts)) {
            failure = QStringLiteral("Cannot verify local filesystem mounts for recursive search.");
            finish();
            return;
        }
        const auto rootPath = QFile::encodeName(root);
        const int descriptor = openRoot(rootPath, mounts, request.cancel);
        if (descriptor < 0) {
            if (!canceled()) failure = readableError(errno);
            finish();
            return;
        }
        Directory directory(fdopendir(descriptor));
        if (!directory) {
            const int code = errno;
            close(descriptor);
            failure = readableError(code);
            finish();
            return;
        }
        std::vector<Frame> stack;
        stack.push_back({rootPath, std::move(directory)});
        QByteArray preferredPath;
        if (!request.options.preferredPath.isEmpty()) {
            const auto preferred = QFile::encodeName(QDir::cleanPath(request.options.preferredPath));
            if (preferred != rootPath && preferred.startsWith(rootPath == "/" ? rootPath : rootPath + '/')) {
                const int preferredDescriptor = openRoot(preferred, mounts, request.cancel);
                if (preferredDescriptor >= 0) {
                    Directory first(fdopendir(preferredDescriptor));
                    if (first) {
                        preferredPath = preferred;
                        stack.push_back({preferred, std::move(first)});
                    } else close(preferredDescriptor);
                }
            }
        }
        while (!stack.empty() && !canceled()) {
            if (examined >= request.options.maxEntries || elapsed.elapsed() >= request.options.maxElapsedMs) {
                limited = true;
                break;
            }
            auto &frame = stack.back();
            errno = 0;
            const auto *entry = readdir(frame.directory.get());
            if (!entry) {
                if (errno) ++skipped;
                stack.pop_back();
                continue;
            }
            const QByteArray name(entry->d_name);
            if (name == "." || name == "..") continue;
            ++examined;
            if (!request.options.showHidden && name.startsWith('.')) continue;
            const QByteArray path = frame.path == "/" ? '/' + name : frame.path + '/' + name;
            // Do not even stat a remote or pseudo mount entry.
            if (!localPath(path, mounts)) { ++skipped; continue; }
            unsigned char type = entry->d_type;
            if (type == DT_UNKNOWN) {
                struct stat status;
                if (fstatat(dirfd(frame.directory.get()), name.constData(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
                    ++skipped;
                    continue;
                }
                type = S_ISDIR(status.st_mode) ? DT_DIR : S_ISLNK(status.st_mode) ? DT_LNK : S_ISREG(status.st_mode) ? DT_REG : DT_UNKNOWN;
            }
            bool tagMatches = request.options.requiredTag.isEmpty();
            if (!tagMatches && (type == DT_DIR || type == DT_REG)) {
                // Read metadata from the inode reached by this safe directory
                // walk. Never follow a changed symlink or read file contents.
                const int tagged = openat(dirfd(frame.directory.get()), name.constData(),
                                          O_PATH | O_NOFOLLOW | O_CLOEXEC);
                if (tagged >= 0) {
                    struct stat metadata;
                    if (fstat(tagged, &metadata) == 0 &&
                        (S_ISREG(metadata.st_mode) || S_ISDIR(metadata.st_mode))) {
                        const QByteArray pinned = "/proc/self/fd/" + QByteArray::number(tagged);
                        char value[65536];
                        const auto length = getxattr(pinned.constData(), "user.xdg.tags", value, sizeof(value));
                        if (length >= 0)
                            tagMatches = QString::fromUtf8(value, length).split(',')
                                .contains(request.options.requiredTag, Qt::CaseInsensitive);
                    }
                    close(tagged);
                }
            }
            bool metadataMatches = true;
            if (request.options.minimumSize >= 0 || request.options.maximumSize >= 0 ||
                request.options.modifiedAfter >= 0 || request.options.modifiedBefore >= 0) {
                struct stat metadata;
                metadataMatches = fstatat(dirfd(frame.directory.get()), name.constData(), &metadata, AT_SYMLINK_NOFOLLOW) == 0;
                if (metadataMatches) {
                    metadataMatches = (request.options.minimumSize < 0 || metadata.st_size >= request.options.minimumSize) &&
                        (request.options.maximumSize < 0 || metadata.st_size <= request.options.maximumSize) &&
                        (request.options.modifiedAfter < 0 || metadata.st_mtime >= request.options.modifiedAfter) &&
                        (request.options.modifiedBefore < 0 || metadata.st_mtime <= request.options.modifiedBefore);
                }
            }
            const auto kind = request.options.kind;
            if (metadataMatches && !kind.isEmpty()) {
                if (kind == "folder") metadataMatches = type == DT_DIR;
                else if (kind == "file") metadataMatches = type == DT_REG;
                else {
                    const auto mime = QMimeDatabase().mimeTypeForFile(QFile::decodeName(name), QMimeDatabase::MatchExtension);
                    metadataMatches = type == DT_REG && (
                        (kind == "image" && mime.name().startsWith("image/")) ||
                        (kind == "pdf" && mime.inherits("application/pdf")) ||
                        (kind == "text" && mime.inherits("text/plain")) ||
                        (kind == "audio" && mime.name().startsWith("audio/")) ||
                        (kind == "video" && mime.name().startsWith("video/")) ||
                        (kind == "archive" && (mime.inherits("application/zip") || mime.inherits("application/x-tar") || mime.inherits("application/x-compressed-tar"))));
                }
            }
            const bool nameMatches = QFile::decodeName(name).contains(request.query, Qt::CaseInsensitive);
            if (tagMatches && metadataMatches && nameMatches && !request.options.contentText.isEmpty()) {
                metadataMatches = false;
                const auto mime = QMimeDatabase().mimeTypeForFile(QFile::decodeName(name), QMimeDatabase::MatchExtension);
                if (type == DT_REG && mime.inherits("text/plain")) {
                    const int descriptor = openat(dirfd(frame.directory.get()), name.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
                    if (descriptor >= 0) {
                        struct stat metadata;
                        if (fstat(descriptor, &metadata) == 0 && S_ISREG(metadata.st_mode) && metadata.st_size <= request.options.maxContentBytes) {
                            QFile content;
                            if (content.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle))
                                metadataMatches = QString::fromUtf8(content.read(request.options.maxContentBytes)).contains(request.options.contentText, Qt::CaseInsensitive);
                            else close(descriptor);
                        } else close(descriptor);
                    }
                }
            }
            if (tagMatches && metadataMatches && (type == DT_DIR || type == DT_REG || type == DT_LNK) &&
                    QFile::decodeName(name).contains(request.query, Qt::CaseInsensitive)) {
                batch.append(QUrl::fromLocalFile(QFile::decodeName(path)));
                ++count;
                if (batch.size() >= 64 || lastBatch.elapsed() >= 50) {
                    emit batchReady(request.generation, batch);
                    batch.clear();
                    lastBatch.restart();
                }
                if (count >= request.options.maxResults) { limited = true; break; }
            }
            // Keep the directory itself discoverable; only its descendants
            // were already scanned first and must not be visited twice.
            if (type != DT_DIR || canceled() || path == preferredPath) continue;
            if (stack.size() >= 128) { ++skipped; continue; }
            const int child = openat(dirfd(frame.directory.get()), name.constData(),
                                     O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0) { ++skipped; continue; }
            Directory next(fdopendir(child));
            if (!next) { close(child); ++skipped; continue; }
            stack.push_back({path, std::move(next)});
        }
        finish();
    }
};

class LocalFileSearch::Private {
public:
    std::shared_ptr<Shared> state = std::make_shared<Shared>();
    quint64 generation = 0;
    bool running = false;
};

LocalFileSearch::LocalFileSearch(QObject *parent) : QObject(parent), d(std::make_unique<Private>()) {
    qRegisterMetaType<QList<QUrl>>();
    auto *worker = new LocalFileSearchWorker(d->state);
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &LocalFileSearchWorker::batchReady, this, [this](quint64 generation, const QList<QUrl> &files) {
        if (generation == d->generation && d->running) emit filesFound(generation, files);
    }, Qt::QueuedConnection);
    connect(worker, &LocalFileSearchWorker::complete, this,
            [this](quint64 generation, int count, bool limited, int skipped, const QString &failure, bool wasCanceled) {
        if (generation != d->generation || !d->running) return;
        d->running = false;
        if (wasCanceled) emit canceled(generation);
        else if (!failure.isEmpty()) emit error(generation, failure);
        else emit finished(generation, count, limited, skipped);
    }, Qt::QueuedConnection);
    worker->start();
}

LocalFileSearch::~LocalFileSearch() {
    {
        std::lock_guard lock(d->state->mutex);
        d->state->stopping = true;
        if (d->state->active) d->state->active->store(true, std::memory_order_relaxed);
        if (d->state->pending) d->state->pending->cancel->store(true, std::memory_order_relaxed);
        d->state->pending.reset();
    }
    d->state->wake.notify_one();
    // The independent worker owns its state and deletes itself after finishing.
    // Never join/wait on the UI thread if a local filesystem syscall is slow.
}

quint64 LocalFileSearch::start(const QUrl &root, const QString &filename) {
    return start(root, filename, Options{});
}

quint64 LocalFileSearch::start(const QUrl &root, const QString &filename, const Options &requested) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto previous = d->generation;
    const bool wasRunning = d->running;
    Options options = requested;
    options.maxResults = std::clamp(options.maxResults, 1, 10000);
    options.maxEntries = std::clamp(options.maxEntries, 1, 1000000);
    options.maxElapsedMs = std::clamp(options.maxElapsedMs, 1, 120000);
    options.maxContentBytes = std::clamp(options.maxContentBytes, 1, 4 * 1024 * 1024);
    const auto generation = ++d->generation;
    d->running = true;
    {
        std::lock_guard lock(d->state->mutex);
        if (d->state->active) d->state->active->store(true, std::memory_order_relaxed);
        if (d->state->pending) d->state->pending->cancel->store(true, std::memory_order_relaxed);
        d->state->pending = Request{generation, root, filename.trimmed(), options, std::make_shared<std::atomic_bool>(false)};
    }
    d->state->wake.notify_one();
    if (wasRunning) emit canceled(previous);
    emit started(generation, root, filename.trimmed());
    return generation;
}

void LocalFileSearch::cancel() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!d->running) return;
    {
        std::lock_guard lock(d->state->mutex);
        if (d->state->active) d->state->active->store(true, std::memory_order_relaxed);
        if (d->state->pending) d->state->pending->cancel->store(true, std::memory_order_relaxed);
        d->state->pending.reset();
    }
    d->running = false;
    emit canceled(d->generation);
}

quint64 LocalFileSearch::generation() const { return d->generation; }
bool LocalFileSearch::isRunning() const { return d->running; }

#include "search.moc"
