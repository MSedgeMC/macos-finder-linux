// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#include "smartfolder.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMetaObject>
#include <QStringList>
#include <QThread>
#include <QThreadPool>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <optional>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {
constexpr qsizetype maximumBytes = 65536;
using Cancellation = std::shared_ptr<std::atomic_bool>;

struct Request {
    QUrl descriptor;
    quint64 generation;
    Cancellation cancel;
};
struct Result {
    QJsonObject definition;
    QUrl root;
    QString error;
};
struct State {
    std::mutex mutex;
    std::optional<Request> pending;
    Cancellation active;
    SmartFolderReader *receiver = nullptr; // Access only while mutex is held.
    quint64 generation = 0;
    bool scheduled = false;
    bool stopping = false;
};

QThreadPool &readerPool()
{
    // Process-owned deliberately: destroying a controller (or the application
    // on its GUI thread) must not wait for an unavailable filesystem. Idle
    // workers expire normally; the OS reclaims the pool at process exit.
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
    explicit Descriptor(int value) : value(value) {}
    ~Descriptor() { if (value >= 0) ::close(value); }
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
    int get() const { return value; }
private:
    int value;
};

Result failure(const QString &message) { return {{}, {}, message}; }
Result ioFailure(const char *operation, int code)
{
    return failure(QStringLiteral("Could not %1 the Smart Folder: %2")
        .arg(QString::fromLatin1(operation), QString::fromLocal8Bit(std::strerror(code))));
}

bool explicitLocalUrl(const QUrl &url)
{
    return url.isValid() && url.isLocalFile() && url.host().isEmpty()
        && url.userInfo().isEmpty() && url.port() == -1 && !url.hasQuery()
        && !url.hasFragment() && url.path().startsWith('/')
        && !url.path().contains(QChar::Null);
}

bool folderUrl(const QUrl &url)
{
    if (!url.isValid() || url.isRelative() || url.hasQuery() || url.hasFragment()
        || !url.path().startsWith('/') || url.path().contains(QChar::Null)) return false;
    if (url.scheme() == QStringLiteral("file")) return explicitLocalUrl(url);
    const QString scheme = url.scheme();
    if (scheme == QStringLiteral("smb"))
        return !url.host().isEmpty() || url.authority().isEmpty(); // smb:/ discovery root.
    if (scheme == QStringLiteral("sftp") || scheme == QStringLiteral("ftp")
        || scheme == QStringLiteral("fish") || scheme == QStringLiteral("webdav")
        || scheme == QStringLiteral("webdavs")) return !url.host().isEmpty();
    return (scheme == QStringLiteral("trash") || scheme == QStringLiteral("remote")
        || scheme == QStringLiteral("mtp")) && url.authority().isEmpty();
}

Result validate(const QByteArray &bytes, const Request &request)
{
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return failure(QStringLiteral("The Smart Folder does not contain a valid JSON object."));
    auto definition = document.object();
    if (definition.value("format") != QJsonValue(QStringLiteral("macos-finder-linux-smart-folder"))
        || !definition.value("version").isDouble() || definition.value("version").toDouble() != 1.0)
        return failure(QStringLiteral("The Smart Folder format or version is not supported."));
    if (!definition.value("root").isString())
        return failure(QStringLiteral("The Smart Folder needs an explicit folder URL."));
    QUrl root(definition.value("root").toString(), QUrl::StrictMode);
    if (!folderUrl(root))
        return failure(QStringLiteral("The Smart Folder root is not a supported explicit folder URL."));
    root = root.adjusted(QUrl::NormalizePathSegments | QUrl::StripTrailingSlash);
    if (root == request.descriptor.adjusted(QUrl::NormalizePathSegments | QUrl::StripTrailingSlash)
        || root.fileName().endsWith(QStringLiteral(".finder-search")))
        return failure(QStringLiteral("A Smart Folder cannot use another Smart Folder as its root."));

    for (const auto *key : {"name", "content", "scope", "kind"}) {
        if (definition.contains(key) && !definition.value(key).isString())
            return failure(QStringLiteral("The Smart Folder has an invalid %1 criterion.").arg(QString::fromLatin1(key)));
        if (!definition.contains(key))
            definition.insert(key, std::strcmp(key, "scope") == 0 ? QStringLiteral("computer") : QString());
    }
    const auto scope = definition.value("scope").toString();
    const auto kind = definition.value("kind").toString();
    if (scope != QStringLiteral("computer") && scope != QStringLiteral("current"))
        return failure(QStringLiteral("The Smart Folder search scope is not supported."));
    const QStringList kinds{"", "file", "folder", "image", "pdf", "text", "audio", "video", "archive"};
    if (!kinds.contains(kind))
        return failure(QStringLiteral("The Smart Folder file kind is not supported."));
    for (const auto *key : {"minimumSize", "maximumSize", "modifiedAfter", "modifiedBefore"}) {
        if (!definition.contains(key)) definition.insert(key, -1);
        const auto value = definition.value(key);
        if (!value.isDouble() || value.toInteger(std::numeric_limits<qint64>::min()) < -1)
            return failure(QStringLiteral("The Smart Folder has an invalid %1 criterion.").arg(QString::fromLatin1(key)));
    }
    for (const auto &keys : {std::pair{"minimumSize", "maximumSize"}, std::pair{"modifiedAfter", "modifiedBefore"}}) {
        const auto minimum = definition.value(keys.first).toInteger();
        const auto maximum = definition.value(keys.second).toInteger();
        if (minimum >= 0 && maximum >= 0 && minimum > maximum)
            return failure(QStringLiteral("The Smart Folder criterion range is reversed."));
    }
    if (request.cancel->load(std::memory_order_relaxed)) return {};
    if (root.isLocalFile()) {
        const QFileInfo info(root.toLocalFile());
        if (!info.exists() || !info.isDir())
            return failure(QStringLiteral("The Smart Folder root folder is unavailable."));
    }
    definition.insert("root", root.toString());
    return {definition, root, {}};
}

Result readDefinition(const Request &request)
{
    if (!SmartFolderReader::isSmartFolderUrl(request.descriptor))
        return failure(QStringLiteral("Choose a local .finder-search file."));
    const QByteArray path = QFile::encodeName(request.descriptor.toLocalFile());
    // O_PATH pins an inode without opening FIFO/device drivers. O_NOFOLLOW
    // makes a final symlink itself the pinned inode, which fstat rejects.
    const Descriptor pinned(::open(path.constData(), O_PATH | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    if (pinned.get() < 0) return ioFailure("open", errno);
    struct stat initial {};
    if (::fstat(pinned.get(), &initial) != 0) return ioFailure("inspect", errno);
    if (!S_ISREG(initial.st_mode))
        return failure(QStringLiteral("The Smart Folder descriptor must be a regular file, without a symbolic link."));
    if (initial.st_size > maximumBytes)
        return failure(QStringLiteral("The Smart Folder exceeds 65,536 bytes."));
    if (request.cancel->load(std::memory_order_relaxed)) return {};

    // Reopen the pinned regular inode, never a path replacement. Following
    // this procfs descriptor link is intentional; O_NOFOLLOW was applied to
    // the user path above. The actual read descriptor is validated again.
    const auto pinnedPath = QByteArray("/proc/self/fd/") + QByteArray::number(pinned.get());
    const Descriptor file(::open(pinnedPath.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
    if (file.get() < 0) return ioFailure("read", errno);
    struct stat opened {};
    if (::fstat(file.get(), &opened) != 0) return ioFailure("inspect", errno);
    if (!S_ISREG(opened.st_mode) || opened.st_dev != initial.st_dev || opened.st_ino != initial.st_ino)
        return failure(QStringLiteral("The Smart Folder descriptor changed while opening."));
    if (opened.st_size > maximumBytes)
        return failure(QStringLiteral("The Smart Folder exceeds 65,536 bytes."));

    QByteArray bytes(maximumBytes + 1, Qt::Uninitialized);
    qsizetype used = 0;
    while (used < bytes.size()) {
        if (request.cancel->load(std::memory_order_relaxed)) return {};
        const auto count = ::read(file.get(), bytes.data() + used, size_t(bytes.size() - used));
        if (count < 0) {
            if (errno == EINTR) continue;
            return ioFailure("read", errno);
        }
        if (count == 0) break;
        used += count;
    }
    if (used > maximumBytes) return failure(QStringLiteral("The Smart Folder exceeds 65,536 bytes."));
    bytes.resize(used);
    if (request.cancel->load(std::memory_order_relaxed)) return {};
    return validate(bytes, request);
}
} // namespace

class SmartFolderReader::Private {
public:
    explicit Private(SmartFolderReader *owner) : state(std::make_shared<State>()) { state->receiver = owner; }
    std::shared_ptr<State> state;
    Completion completion;
    bool running = false;

    static void work(const std::shared_ptr<State> &state)
    {
        for (;;) {
            std::optional<Request> request;
            {
                std::lock_guard lock(state->mutex);
                if (state->stopping || !state->pending) {
                    state->scheduled = false;
                    state->active.reset();
                    return;
                }
                request = std::move(state->pending);
                state->pending.reset();
                state->active = request->cancel;
            }
            if (request->cancel->load(std::memory_order_relaxed)) continue;
            auto result = readDefinition(*request);
            std::lock_guard lock(state->mutex);
            if (state->stopping || request->cancel->load(std::memory_order_relaxed)
                || request->generation != state->generation) continue;
            auto *receiver = state->receiver;
            // Destruction clears receiver under this same lock, so posting
            // never races QObject destruction. Qt drops queued calls when
            // their context dies; the queued generation check covers cancel
            // or replacement after posting but before event delivery.
            QMetaObject::invokeMethod(receiver, [receiver, state, result = std::move(result), generation = request->generation] {
                {
                    std::lock_guard lock(state->mutex);
                    if (state->stopping || state->generation != generation) return;
                }
                receiver->d->running = false;
                auto completion = std::move(receiver->d->completion);
                if (completion) completion(result.definition, result.root, result.error, generation);
            }, Qt::QueuedConnection);
        }
    }
};

SmartFolderReader::SmartFolderReader(QObject *parent) : QObject(parent), d(std::make_unique<Private>(this)) {}
SmartFolderReader::~SmartFolderReader()
{
    std::lock_guard lock(d->state->mutex);
    d->state->stopping = true;
    d->state->receiver = nullptr;
    if (d->state->active) d->state->active->store(true, std::memory_order_relaxed);
    d->state->pending.reset();
}

bool SmartFolderReader::isSmartFolderUrl(const QUrl &url)
{
    return explicitLocalUrl(url) && url.fileName().endsWith(QStringLiteral(".finder-search"));
}

quint64 SmartFolderReader::read(const QUrl &descriptor, Completion completion)
{
    Q_ASSERT(QThread::currentThread() == thread());
    d->completion = std::move(completion);
    d->running = true;
    bool schedule = false;
    quint64 generation;
    {
        std::lock_guard lock(d->state->mutex);
        generation = ++d->state->generation;
        if (d->state->active) d->state->active->store(true, std::memory_order_relaxed);
        d->state->pending = Request{descriptor, generation, std::make_shared<std::atomic_bool>(false)};
        if (!d->state->scheduled) { d->state->scheduled = true; schedule = true; }
    }
    if (schedule) readerPool().start([state = d->state] { Private::work(state); });
    return generation;
}

void SmartFolderReader::cancel()
{
    Q_ASSERT(QThread::currentThread() == thread());
    {
        std::lock_guard lock(d->state->mutex);
        ++d->state->generation;
        if (d->state->active) d->state->active->store(true, std::memory_order_relaxed);
        d->state->pending.reset();
    }
    d->running = false;
    d->completion = {};
}
quint64 SmartFolderReader::generation() const
{
    Q_ASSERT(QThread::currentThread() == thread());
    std::lock_guard lock(d->state->mutex);
    return d->state->generation;
}
bool SmartFolderReader::isRunning() const
{
    Q_ASSERT(QThread::currentThread() == thread());
    return d->running;
}
