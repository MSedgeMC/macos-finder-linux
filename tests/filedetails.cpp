// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
// Standalone target: this file + src/filedetails.{h,cpp}; AUTOMOC, C++20,
// Qt6::Widgets and Threads::Threads. Link --wrap=open, --wrap=openat,
// --wrap=fgetxattr, --wrap=fsetxattr and --wrap=fremovexattr.
// Run via tests/run-headless.py. Optional --screenshot <path> saves the private
// offscreen dialog rendering; never opens a preview on the working desktop.
#include "filedetails.h"

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QGroupBox>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdarg>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace {
using namespace FileDetails;
using namespace Qt::Literals::StringLiterals;
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
bool waitFor(const std::function<bool()> &condition, int milliseconds = 10000)
{
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return condition();
}
void drain(int milliseconds = 50) { waitFor([] { return false; }, milliseconds); }
void writeFile(const QString &path, const QByteArray &bytes = "unchanged contents\0including NUL"_ba)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "create file fixture");
}
QByteArray contents(const QString &path)
{
    QFile file(path); require(file.open(QIODevice::ReadOnly), "read file fixture"); return file.readAll();
}
void attr(const QString &path, const char *name, const QByteArray &bytes)
{
    require(::setxattr(QFile::encodeName(path).constData(), name, bytes.constData(), size_t(bytes.size()), 0) == 0, "seed real xattr");
}
QByteArray attr(const QString &path, const char *name)
{
    QByteArray bytes(65536, Qt::Uninitialized);
    const auto result = ::getxattr(QFile::encodeName(path).constData(), name, bytes.data(), size_t(bytes.size()));
    require(result >= 0, "read fixture xattr"); bytes.resize(result); return bytes;
}
bool missingAttr(const QString &path, const char *name)
{
    errno = 0;
    return ::getxattr(QFile::encodeName(path).constData(), name, nullptr, 0) == -1 && errno == ENODATA;
}

struct Hooks {
    std::mutex mutex;
    std::condition_variable wake;
    QList<quint64> gated;
    quint64 failInode = 0;
    int readError = 0, writeError = 0, entered = 0, active = 0, peak = 0;
    std::atomic_int probes{0}, guiProbes{0}, specialOpens{0};
    bool released = true;
    QByteArray renameSource, renameDestination;
    quint64 renameInode = 0;
} hooks;
bool guiThread() { return QCoreApplication::instance() && QThread::currentThread() == QCoreApplication::instance()->thread(); }
quint64 descriptorInode(int fd)
{
    struct stat info {};
    return ::fstat(fd, &info) == 0 ? quint64(info.st_ino) : 0;
}
void release()
{
    std::lock_guard lock(hooks.mutex);
    hooks.released = true;
    hooks.wake.notify_all();
}
void gate(const QList<Details> &details)
{
    std::lock_guard lock(hooks.mutex);
    hooks.gated.clear();
    for (const auto &entry : details) hooks.gated.append(entry.identity.inode);
    hooks.entered = hooks.active = hooks.peak = 0;
    hooks.released = false;
}
int entered() { std::lock_guard lock(hooks.mutex); return hooks.entered; }
int peak() { std::lock_guard lock(hooks.mutex); return hooks.peak; }
void errors(quint64 inode, int readError, int writeError)
{
    std::lock_guard lock(hooks.mutex);
    hooks.failInode = inode; hooks.readError = readError; hooks.writeError = writeError;
}
void completed(FileDetailsCache &cache)
{
    require(waitFor([&] { return !cache.isLoading(); }), "asynchronous read completion");
}
template<class T> T *widget(FileDetailsDialog &dialog, const char *name)
{
    auto *found = dialog.findChild<T *>(QString::fromLatin1(name)); require(found, "expected dialog control"); return found;
}
int noteOpen(int result, int flags)
{
    ++hooks.probes;
    if (guiThread()) ++hooks.guiProbes;
    if (result >= 0 && !(flags & O_PATH)) {
        struct stat info {};
        if (::fstat(result, &info) == 0 && !S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) ++hooks.specialOpens;
    }
    return result;
}
} // namespace

extern "C" int __real_open(const char *, int, ...);
extern "C" int __real_openat(int, const char *, int, ...);
extern "C" ssize_t __real_fgetxattr(int, const char *, void *, size_t);
extern "C" int __real_fsetxattr(int, const char *, const void *, size_t, int);
extern "C" int __real_fremovexattr(int, const char *);
extern "C" int __wrap_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args); }
    return noteOpen(__real_open(path, flags, mode), flags);
}
extern "C" int __wrap_openat(int parent, const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args); }
    return noteOpen(__real_openat(parent, path, flags, mode), flags);
}
extern "C" ssize_t __wrap_fgetxattr(int fd, const char *name, void *value, size_t size)
{
    ++hooks.probes;
    if (guiThread()) ++hooks.guiProbes;
    const auto inode = descriptorInode(fd);
    {
        std::unique_lock lock(hooks.mutex);
        if (!hooks.released && hooks.gated.contains(inode)) {
            ++hooks.entered; ++hooks.active; hooks.peak = std::max(hooks.peak, hooks.active);
            hooks.wake.notify_all();
            hooks.wake.wait(lock, [] { return hooks.released; });
            --hooks.active;
        }
        if (hooks.failInode == inode && hooks.readError) { errno = hooks.readError; return -1; }
    }
    return __real_fgetxattr(fd, name, value, size);
}
extern "C" int __wrap_fsetxattr(int fd, const char *name, const void *value, size_t size, int flags)
{
    ++hooks.probes;
    if (guiThread()) ++hooks.guiProbes;
    const auto inode = descriptorInode(fd);
    {
        std::lock_guard lock(hooks.mutex);
        if (hooks.failInode == inode && hooks.writeError) { errno = hooks.writeError; return -1; }
        if (hooks.renameInode == inode) {
            require(::rename(hooks.renameSource.constData(), hooks.renameDestination.constData()) == 0, "rename pinned fixture during write");
            // A malicious replacement at the original path must not redirect
            // the in-flight metadata write to this new inode.
            const int replacement = __real_open(hooks.renameSource.constData(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
            require(replacement >= 0, "replacement fixture during write");
            ::close(replacement);
            hooks.renameInode = 0;
        }
    }
    return __real_fsetxattr(fd, name, value, size, flags);
}
extern "C" int __wrap_fremovexattr(int fd, const char *name)
{
    ++hooks.probes;
    if (guiThread()) ++hooks.guiProbes;
    const auto inode = descriptorInode(fd);
    {
        std::lock_guard lock(hooks.mutex);
        if (hooks.failInode == inode && hooks.writeError) { errno = hooks.writeError; return -1; }
    }
    return __real_fremovexattr(fd, name);
}

int main(int argc, char **argv)
{
    using namespace Qt::Literals::StringLiterals;
    try {
        require(qgetenv("QT_QPA_PLATFORM") == "offscreen" && !qEnvironmentVariableIsSet("DISPLAY")
            && !qEnvironmentVariableIsSet("WAYLAND_DISPLAY") && qEnvironmentVariableIsSet("DBUS_SESSION_BUS_ADDRESS")
            && qEnvironmentVariableIsSet("XDG_RUNTIME_DIR") && qEnvironmentVariableIsSet("XDG_CONFIG_HOME"), "private headless runner required");
        QApplication app(argc, argv);
        app.setApplicationName("FinderFileDetailsPrivateTest");
        QTemporaryDir files;
        require(files.isValid(), "fixture directory");
        const auto path = files.filePath("report.txt");
        const auto secondPath = files.filePath("other.custom");
        writeFile(path); writeFile(secondPath, "second");
        const auto url = QUrl::fromLocalFile(path), secondUrl = QUrl::fromLocalFile(secondPath);
        const auto originalBytes = contents(path);
        attr(path, "user.xdg.tags", "Red,Project");
        attr(path, "user.unrelated.fixture", "unchanged\0binary"_ba);
        const auto original = FileDetails::read(url);
        const auto second = FileDetails::read(secondUrl);
        require(original.available() && original.extensionError.ok() && original.commentError.ok(), "real metadata reads supported on fixtures");
        require(original.extensionOverride == ExtensionOverride::Inherit && original.comment.isEmpty(), "absent metadata defaults");
        require(FileDetails::setExtensionOverride(url, ExtensionOverride::Show, original.identity).ok(), "explicit Show write");
        require(attr(path, hideExtensionAttribute) == "0", "honest extension xattr schema");
        require(FileDetails::read(url).extensionOverride == ExtensionOverride::Show, "Show round trip");
        require(FileDetails::setExtensionOverride(url, ExtensionOverride::Hide, original.identity).ok(), "explicit Hide write");
        require(attr(path, hideExtensionAttribute) == "1" && FileDetails::read(url).extensionOverride == ExtensionOverride::Hide, "Hide round trip");
        const auto comment = QString::fromUtf8("Review tomorrow\nनमस्ते ☃");
        require(FileDetails::setComment(url, comment, original.identity).ok() && FileDetails::read(url).comment == comment, "Unicode multiline comments persist");
        require(attr(path, commentAttribute) == comment.toUtf8(), "KDE-compatible xdg comment bytes");
        require(contents(path) == originalBytes && attr(path, "user.xdg.tags") == "Red,Project"
            && attr(path, "user.unrelated.fixture") == "unchanged\0binary"_ba, "contents tags and unrelated xattrs preserved");
        require(FileDetails::setComment(url, {}).ok() && missingAttr(path, commentAttribute), "empty comment removes attribute");
        require(FileDetails::setExtensionOverride(url, ExtensionOverride::Inherit).ok() && missingAttr(path, hideExtensionAttribute), "Inherit removes attribute");
        require(FileDetails::setExtensionOverride(url, ExtensionOverride(9)).code == ErrorCode::InvalidInput, "invalid extension rejected");
        require(FileDetails::setComment(url, QString(maximumCommentBytes + 1, 'x')).code == ErrorCode::TooLarge, "oversize comment rejected");
        require(FileDetails::setComment(url, QString(maximumCommentBytes / 2, QChar(0x20ac))).code == ErrorCode::TooLarge, "UTF-8 byte limit enforced");
        require(FileDetails::setComment(url, QString(QChar::Null)).code == ErrorCode::InvalidInput, "NUL comment rejected");
        require(FileDetails::setComment(url, QString(QChar(0xd800))).code == ErrorCode::InvalidInput, "unpaired surrogate rejected");
        attr(path, hideExtensionAttribute, "");
        require(FileDetails::read(url).extensionError.code == ErrorCode::InvalidMetadata, "empty existing extension is malformed");
        attr(path, hideExtensionAttribute, "x");
        require(FileDetails::read(url).extensionError.code == ErrorCode::InvalidMetadata, "invalid stored extension surfaced");
        attr(path, commentAttribute, QByteArray(1, char(0xff)));
        require(FileDetails::read(url).commentError.code == ErrorCode::InvalidMetadata, "invalid UTF-8 surfaced");
        attr(path, commentAttribute, "nul\0comment"_ba);
        require(FileDetails::read(url).commentError.code == ErrorCode::InvalidMetadata, "existing NUL rejected");
        // tmpfs allows values above the editor limit; ext4 may refuse them itself.
        const auto large = QByteArray(maximumCommentBytes + 1, 'x');
        if (::setxattr(QFile::encodeName(path).constData(), commentAttribute, large.constData(), size_t(large.size()), 0) == 0)
            require(FileDetails::read(url).commentError.code == ErrorCode::TooLarge, "oversized existing comments reported without truncation");
        require(FileDetails::setComment(url, {}).ok() && FileDetails::setExtensionOverride(url, ExtensionOverride::Inherit).ok(), "repair malformed metadata");

        require(displayName("archive.tar.gz", false, "tar.gz", ExtensionOverride::Inherit, false) == "archive", "default compound MIME suffix");
        require(displayName("thing.custom", false, {}, ExtensionOverride::Inherit, false) == "thing.custom", "default unknown suffix retained");
        require(displayName("thing.custom", false, {}, ExtensionOverride::Hide, false) == "thing", "explicit unknown suffix hidden");
        require(displayName("REPORT.TXT", false, "txt", ExtensionOverride::Hide, false) == "REPORT", "case-insensitive known suffix");
        for (auto override : {ExtensionOverride::Show, ExtensionOverride::Hide, ExtensionOverride::Inherit})
            require(displayName("report.txt", false, "txt", override, true) == "report.txt", "showAllExtensions overrides all per-file choices");
        require(displayName("folder.txt", true, "txt", ExtensionOverride::Hide, false) == "folder.txt", "directory names intact");
        require(displayName("report.txt", false, "txt", ExtensionOverride::Show, false) == "report.txt", "explicit Show overrides default");
        require(displayName(".bashrc", false, {}, ExtensionOverride::Hide, false) == ".bashrc"
            && displayName("noext.", false, {}, ExtensionOverride::Hide, false) == "noext.", "dotfiles and trailing dot intact");

        const auto link = files.filePath("link");
        const auto fifo = files.filePath("fifo");
        const auto directory = files.filePath("folder");
        require(QDir().mkpath(directory), "folder fixture");
        require(::symlink(QFile::encodeName(path).constData(), QFile::encodeName(link).constData()) == 0, "symlink fixture");
        require(::mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0, "FIFO fixture");
        const auto ancestorLink = files.filePath("ancestor-link");
        require(::symlink(QFile::encodeName(files.path()).constData(), QFile::encodeName(ancestorLink).constData()) == 0, "symlink ancestor fixture");
        const QList<QUrl> unsafe{QUrl::fromLocalFile(link), QUrl::fromLocalFile(fifo), QUrl::fromLocalFile("/dev/null"),
            QUrl::fromLocalFile(ancestorLink + "/report.txt")};
        for (const auto &unsafeUrl : unsafe) {
            require(FileDetails::read(unsafeUrl).error.code == ErrorCode::UnsafeFile, "unsafe read rejected");
            require(FileDetails::setComment(unsafeUrl, "bad").code == ErrorCode::UnsafeFile, "unsafe write rejected");
        }
        require(hooks.specialOpens == 0, "no FIFO or device opened without O_PATH");
        require(FileDetails::read(QUrl("sftp://example.invalid/a")).error.code == ErrorCode::NonLocal, "remote URL rejected");
        require(FileDetails::read(QUrl("file://evil.invalid/tmp/a")).error.code == ErrorCode::NonLocal, "remote file host rejected");
        require(FileDetails::read(QUrl::fromLocalFile(path + "/../other.custom")).error.code == ErrorCode::InvalidInput, "parent traversal rejected");
        require(FileDetails::read(QUrl::fromLocalFile(files.filePath("missing"))).error.code == ErrorCode::Missing, "missing file reported");
        auto localhost = url; localhost.setHost("localhost");
        require(FileDetails::read(localhost).identity == original.identity, "localhost normalized as local inode");
        auto query = url; query.setQuery("unsafe");
        require(FileDetails::read(query).error.code == ErrorCode::InvalidInput, "query URL rejected");
        const auto directoryUrl = QUrl::fromLocalFile(directory);
        require(FileDetails::setComment(directoryUrl, "Folder comment").ok() && FileDetails::read(directoryUrl).directory, "directory metadata supported without recursion");

        errors(original.identity.inode, ENOTSUP, ENOTSUP);
        auto unsupported = FileDetails::read(url);
        require(unsupported.extensionError.code == ErrorCode::Unsupported && unsupported.commentError.code == ErrorCode::Unsupported, "unsupported storage read surfaced");
        require(FileDetails::setComment(url, "bad").code == ErrorCode::Unsupported, "unsupported storage write surfaced");
        errors(original.identity.inode, EACCES, EROFS);
        require(FileDetails::read(url).commentError.code == ErrorCode::PermissionDenied, "read permission error surfaced");
        require(FileDetails::setComment(url, "bad").code == ErrorCode::PermissionDenied, "read-only write surfaced");
        errors(original.identity.inode, 0, ENOSPC);
        require(FileDetails::setComment(url, "bad").code == ErrorCode::StorageError, "out-of-space write surfaced");
        errors(0, 0, 0);

        const auto linkedPath = files.filePath("hardlink.txt");
        require(::link(QFile::encodeName(path).constData(), QFile::encodeName(linkedPath).constData()) == 0, "hardlink fixture");
        require(FileDetails::setComment(url, "inode comment", original.identity).ok()
            && FileDetails::read(QUrl::fromLocalFile(linkedPath)).comment == "inode comment", "hardlinks share inode metadata");
        const auto moved = files.filePath("moved.txt");
        {
            std::lock_guard lock(hooks.mutex);
            hooks.renameSource = QFile::encodeName(path); hooks.renameDestination = QFile::encodeName(moved);
            hooks.renameInode = original.identity.inode;
        }
        require(FileDetails::setComment(url, "pinned after rename", original.identity).ok(), "pinned descriptor write through rename");
        require(FileDetails::read(QUrl::fromLocalFile(moved)).comment == "pinned after rename"
            && missingAttr(path, commentAttribute), "replacement did not receive metadata");
        require(FileDetails::setComment(url, "wrong inode", original.identity).code == ErrorCode::Changed, "stale identity rejects replacement");
        const auto current = FileDetails::read(url);
        const auto cancelled = std::make_shared<std::atomic_bool>(true);
        require(FileDetails::readBatch({url, secondUrl}, {}, cancelled).cancelled, "pre-cancelled batch");
        require(FileDetails::readBatch({url, secondUrl}, {1, 5000}).limited, "entry-bounded batch");
        require(FileDetails::readBatch({url}, {4096, 0}).limited, "time-bounded batch");

        hooks.guiProbes = 0;
        FileDetailsCache cache;
        gate({current});
        QElapsedTimer dispatch; dispatch.start();
        cache.setUrls({url});
        require(dispatch.elapsed() < 100 && cache.isLoading(), "read dispatch nonblocking");
        require(waitFor([] { return entered() >= 1; }), "gated async worker entered");
        int beats = 0;
        QTimer heartbeat; heartbeat.setInterval(1);
        QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++beats; }); heartbeat.start();
        drain(30);
        require(beats > 5, "GUI remains responsive with blocked metadata I/O");
        cache.setUrls({secondUrl});
        release(); completed(cache);
        require(!cache.cached(url) && cache.cached(secondUrl)->identity == second.identity, "replacement generation rejects old results");
        const auto probes = hooks.probes.load();
        for (int i = 0; i < 2000; ++i) {
            cache.cached(secondUrl);
            const auto index = cache.model()->index(0, 0);
            for (int role : {int(Qt::DisplayRole), int(Qt::ToolTipRole), int(FileDetailsCache::UrlRole), int(FileDetailsCache::ExtensionOverrideRole),
                            int(FileDetailsCache::CommentRole), int(FileDetailsCache::PendingRole), int(FileDetailsCache::ErrorRole)}) index.data(role);
        }
        require(hooks.probes == probes && hooks.guiProbes == 0, "cache and painting roles have no filesystem calls");
        require(FileDetails::setExtensionOverride(secondUrl, ExtensionOverride::Show).ok(), "external edit fixture");
        hooks.guiProbes = 0;
        cache.refresh(); completed(cache);
        require(cache.cached(secondUrl)->extensionOverride == ExtensionOverride::Show, "refresh updates cached xattrs");
        QList<Error> writeResults;
        QObject::connect(&cache, &FileDetailsCache::writeFinished, [&](quint64, const QUrl &, const Error &error) { writeResults.append(error); });
        gate({second});
        const auto requestId = cache.setComment(secondUrl, "Async comment");
        require(requestId != 0 && cache.isWriting(), "async write admitted with cached identity");
        require(waitFor([] { return entered() >= 1; }), "write readback gated");
        const auto rejectedId = cache.setComment(secondUrl, "Another");
        require(rejectedId == 0 && waitFor([&] { return !writeResults.isEmpty(); }) && writeResults.first().code == ErrorCode::Busy, "busy write rejected honestly");
        // Navigation while writing still receives the completion and must not
        // repopulate the previous selection's cached metadata.
        cache.setUrls({url});
        release();
        require(waitFor([&] { return !cache.isWriting(); }), "write completion delivered across selection change");
        completed(cache);
        require(writeResults.size() == 2 && writeResults.last().ok() && !cache.cached(secondUrl), "exactly one success callback and no stale write cache");
        require(FileDetails::read(secondUrl).comment == "Async comment", "async comment actually persisted");
        hooks.guiProbes = 0;
        gate({current}); cache.refresh();
        require(waitFor([] { return entered() >= 1; }), "cancellation gated worker entered");
        dispatch.restart(); cache.cancel();
        require(dispatch.elapsed() < 100 && !cache.isLoading() && cache.lastBatch().cancelled, "cancel does not wait for blocked syscall");
        release(); drain();
        require(!cache.isLoading() && cache.lastBatch().cancelled, "cancelled result cannot replace cache state");
        cache.setUrls({url, secondUrl}, {1, 5000}); completed(cache);
        require(cache.lastBatch().limited && cache.lastBatch().items.size() == 1 && !cache.cached(secondUrl), "uninspected entries remain unavailable");
        cache.setUrls(QList<QUrl>(maximumMetadataItems + 10, url), {1, 5000}); completed(cache);
        require(cache.urls().size() == maximumMetadataItems && cache.lastBatch().requestedCount == maximumMetadataItems + 10 && cache.lastBatch().limited, "captured selection and work bounded");

        // Four caches never occupy more than two filesystem workers. Repeated
        // refreshes overwrite one pending read rather than growing a task queue.
        gate({current, second});
        auto *a = new FileDetailsCache, *b = new FileDetailsCache, *c = new FileDetailsCache, *dead = new FileDetailsCache;
        a->setUrls({url}); b->setUrls({secondUrl}); c->setUrls({url}); dead->setUrls({secondUrl});
        require(waitFor([] { return entered() >= 2; }), "two pooled workers entered");
        for (int i = 0; i < 100; ++i) { a->refresh(); c->refresh(); }
        dispatch.restart(); delete dead;
        require(dispatch.elapsed() < 100, "destruction does not join worker");
        require(peak() == 2, "global worker concurrency bounded to two");
        release(); completed(*a); completed(*b); completed(*c);
        require(peak() == 2 && a->cached(url) && c->cached(url), "coalesced requests and deleted receiver settle safely");
        delete a; delete b; delete c;

        gate({current});
        QPointer<FileDetailsCache> destroyed = new FileDetailsCache;
        destroyed->setUrls({url});
        require(waitFor([] { return entered() >= 1; }), "active receiver destruction gate entered");
        dispatch.restart(); delete destroyed.data();
        require(dispatch.elapsed() < 100 && destroyed.isNull(), "active receiver destruction is nonblocking");
        release(); drain();
        QPointer<FileDetailsCache> deletedOnDelivery = new FileDetailsCache;
        QObject::connect(deletedOnDelivery, &FileDetailsCache::loadingChanged, [&](bool loading) {
            if (!loading) delete deletedOnDelivery.data();
        });
        deletedOnDelivery->setUrls({url});
        require(waitFor([&] { return deletedOnDelivery.isNull(); }), "receiver can be deleted in a completion callback");
        FileDetailsCache redirected;
        bool redirect = true;
        QList<QUrl> delivered;
        QObject::connect(&redirected, &FileDetailsCache::loadingChanged, [&](bool loading) {
            if (!loading && redirect) { redirect = false; redirected.setUrls({secondUrl}); }
        });
        QObject::connect(&redirected, &FileDetailsCache::batchFinished, [&](const Batch &batch) {
            for (const auto &details : batch.items) delivered.append(details.url);
        });
        redirected.setUrls({url});
        require(waitFor([&] { return !redirect && !redirected.isLoading(); }), "reentrant completion refresh finishes");
        require(delivered == QList<QUrl>{secondUrl}, "reentrant refresh rejects remaining old completion signals");

        // Async cached writes must also reject a replaced inode, leaving the
        // replacement and its unrelated attributes untouched.
        FileDetailsCache stale;
        stale.setUrls({secondUrl}); completed(stale);
        const auto retiredSecond = files.filePath("retired.custom");
        require(QFile::rename(secondPath, retiredSecond), "replace async write target");
        writeFile(secondPath, "new replacement");
        Error staleError;
        bool staleDone = false;
        QObject::connect(&stale, &FileDetailsCache::writeFinished, [&](quint64, const QUrl &, const Error &error) { staleError = error; staleDone = true; });
        require(stale.setComment(secondUrl, "must not overwrite") != 0, "stale cached request admitted for worker validation");
        require(waitFor([&] { return staleDone; }) && staleError.code == ErrorCode::Changed, "async stale inode rejection reported");
        require(missingAttr(secondPath, commentAttribute), "async replacement has no written comment");

        hooks.guiProbes = 0;
        FileDetailsDialog dialog;
        dialog.setExtensionPresentation("txt", false);
        dialog.setUrl(url); completed(*dialog.metadataCache());
        dialog.show(); drain();
        auto *name = widget<QLineEdit>(dialog, "fileDetailsName");
        auto *hide = widget<QCheckBox>(dialog, "fileDetailsHideExtension");
        auto *commentEditor = widget<QTextEdit>(dialog, "fileDetailsComment");
        auto *save = widget<QPushButton>(dialog, "fileDetailsSaveComment");
        auto *rename = widget<QPushButton>(dialog, "fileDetailsRename");
        auto *native = widget<QPushButton>(dialog, "fileDetailsNativeProperties");
        auto *defaults = widget<QPushButton>(dialog, "fileDetailsUseDefault");
        require(dialog.findChild<QGroupBox *>()->title() == "Name && Extension", "section ampersand displayed literally");
        require(name->text() == "report.txt" && hide->isChecked(), "full Name & Extension and inherited checkbox shown");
        QUrl renameUrl, propertiesUrl;
        QString requestedName;
        int saved = 0;
        QObject::connect(&dialog, &FileDetailsDialog::renameRequested, [&](QUrl url, QString name) { renameUrl = url; requestedName = name; });
        QObject::connect(&dialog, &FileDetailsDialog::propertiesRequested, [&](QUrl url) { propertiesUrl = url; });
        QObject::connect(&dialog, &FileDetailsDialog::metadataSaved, [&](QUrl url) { require(url == dialog.url(), "saved callback URL"); ++saved; });
        name->setText("changed.txt"); rename->click();
        require(renameUrl == url && requestedName == "changed.txt" && QFile::exists(path), "rename is a host request and leaves native operation to host");
        native->click(); require(propertiesUrl == url, "native properties host signal");
        name->setText("../unsafe"); requestedName.clear(); rename->click();
        require(requestedName.isEmpty(), "invalid rename does not request native action");
        name->setText("report.txt");
        commentEditor->setPlainText("Draft stays while hide extension changes");
        hide->click();
        require(waitFor([&] { return !dialog.metadataCache()->isWriting(); }), "dialog hide write settles");
        require(dialog.metadataCache()->cached(url)->extensionOverride == ExtensionOverride::Show
            && commentEditor->toPlainText() == "Draft stays while hide extension changes" && save->isEnabled(), "Hide extension callback persists and preserves comment draft");
        save->click();
        require(waitFor([&] { return !dialog.metadataCache()->isWriting(); }), "dialog comment save settles");
        require(!save->isEnabled() && dialog.metadataCache()->cached(url)->comment == "Draft stays while hide extension changes" && saved == 2, "comment callback persists and reports success");
        defaults->click(); require(waitFor([&] { return !dialog.metadataCache()->isWriting(); }), "Use default write settles");
        require(dialog.metadataCache()->cached(url)->extensionOverride == ExtensionOverride::Inherit && saved == 3, "Use default removes override");
        errors(current.identity.inode, 0, ENOTSUP);
        commentEditor->setPlainText("Unsaveable draft"); save->click();
        require(waitFor([&] { return !dialog.metadataCache()->isWriting(); }), "failed comment write settles");
        require(saved == 3 && save->isEnabled() && widget<QLabel>(dialog, "fileDetailsStatus")->text().contains("support"), "failed write retains draft and reports unsupported storage");
        errors(0, 0, 0);
        dialog.setExtensionPresentation("txt", true);
        require(hide->isChecked(), "showAll overrides presentation without deleting stored preference");

        // A hidden page cancels reads, never the freshness of a successful
        // write. Exercise read cancellation too, not just hiding an idle cache.
        commentEditor->setPlainText("Draft survives hidden extension save");
        gate({current}); hide->click();
        require(waitFor([] { return entered() >= 1; }), "hidden extension write readback gated");
        dialog.metadataCache()->refresh();
        dialog.hide();
        require(!dialog.metadataCache()->isLoading() && dialog.metadataCache()->isWriting(), "hide cancels pending read but retains save");
        release();
        require(waitFor([&] { return !dialog.metadataCache()->isWriting(); }), "hidden extension write completed");
        dialog.show(); drain();
        require(dialog.metadataCache()->cached(url)->extensionOverride == ExtensionOverride::Show
            && attr(path, hideExtensionAttribute) == "0" && !hide->isChecked()
            && commentEditor->toPlainText() == "Draft survives hidden extension save" && save->isEnabled(),
            "hidden write refreshes cache and checkbox while preserving unrelated draft");
        gate({current}); save->click();
        require(waitFor([] { return entered() >= 1; }), "hidden comment write readback gated");
        dialog.hide(); release();
        require(waitFor([&] { return !dialog.metadataCache()->isWriting(); }), "hidden comment write completed");
        dialog.show(); drain();
        require(dialog.metadataCache()->cached(url)->comment == "Draft survives hidden extension save"
            && attr(path, commentAttribute) == "Draft survives hidden extension save" && !save->isEnabled(),
            "hidden comment success updates cache and clears saved draft");

        const auto targetPath = files.filePath("retarget.txt");
        require(hooks.guiProbes == 0, "hidden dialog performs no filesystem calls on GUI thread");
        writeFile(targetPath);
        const auto targetUrl = QUrl::fromLocalFile(targetPath);
        require(FileDetails::setComment(targetUrl, "Other file comment").ok(), "retarget comment fixture");
        hooks.guiProbes = 0;
        const int beforeRetarget = saved;
        commentEditor->setPlainText("Old URL save");
        gate({current}); save->click();
        require(waitFor([] { return entered() >= 1; }), "retarget write readback gated");
        dialog.setUrl(targetUrl);
        commentEditor->setPlainText("New URL draft");
        release();
        require(waitFor([&] { return !dialog.metadataCache()->isWriting() && !dialog.metadataCache()->isLoading(); }), "real retarget settles");
        require(!dialog.metadataCache()->cached(url) && dialog.metadataCache()->cached(targetUrl)->comment == "Other file comment"
            && commentEditor->toPlainText() == "New URL draft" && save->isEnabled() && saved == beforeRetarget,
            "old write cannot populate retargeted cache or mutate new draft and saved signals");
        require(attr(path, commentAttribute) == "Old URL save", "retarget does not cancel admitted inode write");
        // Returning to the same URL is still a new editor revision. An old
        // completion must not emit metadataSaved for that newer revision.
        dialog.setUrl(url); completed(*dialog.metadataCache());
        commentEditor->setPlainText("Save before return");
        gate({current}); save->click();
        require(waitFor([] { return entered() >= 1; }), "return retarget write gated");
        dialog.setUrl(targetUrl); dialog.setUrl(url);
        commentEditor->setPlainText("Draft after return");
        release();
        require(waitFor([&] { return !dialog.metadataCache()->isWriting() && !dialog.metadataCache()->isLoading(); }), "return retarget settles");
        require(saved == beforeRetarget && commentEditor->toPlainText() == "Draft after return" && save->isEnabled(),
            "same URL after retarget rejects old completion mutation");

        // Occupy both workers so the dialog save is admitted but not started.
        // Reject, Escape and window close must remain asynchronous and refuse
        // destruction until the queued operation completes.
        FileDetailsCache blockerA, blockerB;
        require(hooks.guiProbes == 0, "retargeted dialog performs no filesystem calls on GUI thread");
        const auto targetDetails = FileDetails::read(targetUrl);
        hooks.guiProbes = 0;
        gate({current, targetDetails});
        blockerA.setUrls({url}); blockerB.setUrls({targetUrl});
        require(waitFor([] { return entered() >= 2; }), "queued close policy occupies both workers");
        save->click();
        require(dialog.metadataCache()->isWriting(), "queued standalone save admitted");
        dispatch.restart(); dialog.reject();
        require(dispatch.elapsed() < 100 && dialog.isVisible(), "standalone reject refuses queued save without waiting");
        dispatch.restart();
        require(!dialog.close() && dispatch.elapsed() < 100 && dialog.isVisible(), "standalone close refuses queued save without waiting");
        QKeyEvent standaloneEscape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(name, &standaloneEscape);
        require(dialog.isVisible() && widget<QLabel>(dialog, "fileDetailsStatus")->text().contains("before closing"),
            "standalone Escape keeps editor and visible blocked-close status");
        release(); completed(blockerA); completed(blockerB);
        require(waitFor([&] { return !dialog.metadataCache()->isWriting(); })
            && attr(path, commentAttribute) == "Draft after return", "queued standalone save survives close attempts");
        require(dialog.close() && !dialog.isVisible(), "standalone closes after successful save");
        dialog.show(); drain();
        commentEditor->setPlainText("Already started save");
        gate({current}); save->click();
        require(waitFor([] { return entered() >= 1; }), "standalone started save readback gated");
        dispatch.restart(); dialog.reject();
        require(dispatch.elapsed() < 100 && dialog.isVisible() && !dialog.close(),
            "standalone started save also refuses reject and close without waiting");
        require(attr(path, commentAttribute) == "Already started save", "started save reached actual xattr before close attempt");
        release(); require(waitFor([&] { return !dialog.metadataCache()->isWriting(); }), "started save settles after close attempt");
        require(!save->isEnabled() && dialog.metadataCache()->cached(url)->comment == "Already started save",
            "started save reports fresh success after blocked close");
        errors(current.identity.inode, 0, ENOSPC);
        commentEditor->setPlainText("Failed save draft");
        gate({current, targetDetails}); blockerA.refresh(); blockerB.refresh();
        require(waitFor([] { return entered() >= 2; }), "failed queued save workers gated");
        save->click(); dialog.reject();
        require(dialog.isVisible() && dialog.metadataCache()->isWriting(), "failed pending save also refuses reject");
        release(); completed(blockerA); completed(blockerB);
        require(waitFor([&] { return !dialog.metadataCache()->isWriting(); }) && save->isEnabled()
            && commentEditor->toPlainText() == "Failed save draft"
            && widget<QLabel>(dialog, "fileDetailsStatus")->text().contains("space"), "failed save restores draft and error after blocked close");
        errors(0, 0, 0);
        dialog.reject(); require(!dialog.isVisible(), "standalone rejection permitted after failed save settles");

        QDialog host;
        auto *embedded = new FileDetailsDialog(&host);
        embedded->setWindowFlags(Qt::Widget);
        auto *hostLayout = new QVBoxLayout(&host); hostLayout->addWidget(embedded);
        embedded->setUrl(targetUrl); completed(*embedded->metadataCache());
        host.show(); drain();
        int closeRequests = 0;
        QObject::connect(embedded, &FileDetailsDialog::closeRequested, &host, [&] {
            ++closeRequests;
            if (embedded->metadataCache()->isWriting()) embedded->showOperationMessage("Host is waiting for the metadata save.");
            else host.reject();
        });
        auto *embeddedComment = widget<QTextEdit>(*embedded, "fileDetailsComment");
        embeddedComment->setPlainText("Embedded save");
        gate({targetDetails}); widget<QPushButton>(*embedded, "fileDetailsSaveComment")->click();
        require(waitFor([] { return entered() >= 1; }), "embedded pending save readback gated");
        const QList<QWidget *> escapeControls{
            widget<QLineEdit>(*embedded, "fileDetailsName"), embeddedComment,
            widget<QCheckBox>(*embedded, "fileDetailsHideExtension"), widget<QPushButton>(*embedded, "fileDetailsRefresh")};
        for (auto *control : escapeControls) {
            const int before = closeRequests;
            control->setFocus();
            QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(control, &escape);
            require(closeRequests == before + 1 && host.isVisible() && embedded->isVisible() && !embedded->isHidden(),
                "embedded Escape routes close request without hiding or blanking editor");
        }
        embedded->reject();
        require(closeRequests == 5 && embedded->isVisible(), "direct embedded reject forwards to host too");
        release(); require(waitFor([&] { return !embedded->metadataCache()->isWriting(); }), "embedded save completes");
        require(attr(targetPath, commentAttribute) == "Embedded save", "embedded Escape preserves save");
        QKeyEvent finalEscape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(widget<QLineEdit>(*embedded, "fileDetailsName"), &finalEscape);
        require(closeRequests == 6 && !host.isVisible() && !embedded->isHidden(), "embedded Escape closes host without explicitly hiding editor");
        host.show(); drain();
        require(embedded->isVisible(), "embedded page remains recoverable when host is shown again");
        host.hide(); dialog.show(); drain();
        require(hooks.guiProbes == 0, "dialog performs no filesystem calls on GUI thread");
        for (int i = 1; i + 1 < argc; ++i)
            if (QString::fromLocal8Bit(argv[i]) == "--screenshot") require(dialog.grab().save(QString::fromLocal8Bit(argv[i + 1])), "save isolated screenshot");
        dialog.hide();
        std::cout << "PASS: inode xattrs, safety, preservation, presentation, bounded async lifecycle and Get Info callbacks\n";
        return 0;
    } catch (const std::exception &error) {
        release();
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
