// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
// Standalone Linux test: this source + src/smartfolder.cpp; Qt6::Core, C++17.
// No moc or GUI. Link with -Wl,--wrap=open,--wrap=read,--wrap=fstat for the
// deterministic slow-filesystem/concurrent-growth tests below. Run through
// tests/run-headless.py; all fixture paths and XDG directories are temporary.
#include "smartfolder.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <fcntl.h>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {
void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

// Only calls compiled in the standalone module are interposed. Qt's fixture
// I/O remains untouched. A gated open models an uninterruptible slow mount
// without creating a real mount or blocking the main/event-loop thread.
struct Hooks {
    std::mutex mutex;
    std::condition_variable wake;
    QList<QByteArray> gated;
    QByteArray deviceAlias;
    int entered = 0;
    int active = 0;
    int peak = 0;
    int supersededOpens = 0;
    bool released = false;
    bool concealSize = false;
    dev_t device = 0;
    ino_t inode = 0;
    qsizetype bytesRead = 0;
    qsizetype largestRead = 0;
} hooks;

void releaseGates()
{
    std::lock_guard lock(hooks.mutex);
    hooks.released = true;
    hooks.wake.notify_all();
}
bool waitFor(const std::function<bool()> &condition, int timeoutMs = 5000)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return condition();
}
void drain(int milliseconds = 50)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
}
void fixture(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "create private fixture");
    require(file.write(bytes) == bytes.size(), "write complete fixture");
}
QJsonObject definition(const QUrl &root)
{
    return {{"format", "macos-finder-linux-smart-folder"}, {"version", 1},
        {"root", root.toString()}, {"name", ""}, {"scope", "current"},
        {"kind", "image"}, {"content", ""}, {"minimumSize", -1},
        {"maximumSize", -1}, {"modifiedAfter", -1}};
}
void descriptor(const QString &path, const QJsonObject &object)
{
    fixture(path, QJsonDocument(object).toJson(QJsonDocument::Compact));
}
struct Result {
    QJsonObject definition;
    QUrl root;
    QString error;
    quint64 generation = 0;
    bool ended = false;
};
class BeforeDelivery final : public QObject {
public:
    std::function<void()> before;
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::MetaCall && before) {
            auto action = std::move(before);
            action();
        }
        return false;
    }
};
Result read(SmartFolderReader &reader, const QUrl &url)
{
    Result result;
    QElapsedTimer elapsed;
    elapsed.start();
    const auto generation = reader.read(url, [&](const QJsonObject &object, const QUrl &root,
                                               const QString &error, quint64 value) {
        require(QThread::currentThread() == QCoreApplication::instance()->thread(), "completion thread");
        require(!reader.isRunning(), "completion marked running");
        result = {object, root, error, value, true};
    });
    require(elapsed.elapsed() < 100, "read blocked event-loop thread");
    require(!result.ended, "read completed synchronously");
    require(waitFor([&] { return result.ended; }), "bounded async completion");
    require(result.generation == generation && reader.generation() == generation, "completion generation");
    return result;
}
void expectError(SmartFolderReader &reader, const QUrl &url)
{
    const auto result = read(reader, url);
    require(!result.error.isEmpty() && result.definition.isEmpty() && result.root.isEmpty(),
        "invalid descriptor accepted or partial result leaked");
}
} // namespace

extern "C" int __real_open(const char *, int, ...);
extern "C" ssize_t __real_read(int, void *, size_t);
extern "C" int __real_fstat(int, struct stat *);

extern "C" int __wrap_open(const char *path, int flags, ...)
{
    bool deviceAlias = false;
    {
        std::unique_lock lock(hooks.mutex);
        const QByteArray name(path);
        deviceAlias = name == hooks.deviceAlias;
        if (name.contains("superseded-")) ++hooks.supersededOpens;
        if (hooks.gated.contains(name) && !hooks.released) {
            ++hooks.entered;
            ++hooks.active;
            hooks.peak = std::max(hooks.peak, hooks.active);
            hooks.wake.wait(lock, [] { return hooks.released; });
            --hooks.active;
        }
    }
    if (deviceAlias) {
        require((flags & O_PATH) && (flags & O_NOFOLLOW), "device inspected without opening its driver");
        return __real_open("/dev/zero", flags);
    }
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list args;
        va_start(args, flags);
        const auto mode = va_arg(args, int);
        va_end(args);
        return __real_open(path, flags, mode);
    }
    return __real_open(path, flags);
}
extern "C" int __wrap_fstat(int fd, struct stat *info)
{
    const int result = __real_fstat(fd, info);
    std::lock_guard lock(hooks.mutex);
    if (result == 0 && hooks.concealSize && info->st_dev == hooks.device && info->st_ino == hooks.inode)
        info->st_size = 0; // Models a file growing after its metadata was checked.
    return result;
}
extern "C" ssize_t __wrap_read(int fd, void *buffer, size_t count)
{
    struct stat info {};
    const bool valid = __real_fstat(fd, &info) == 0;
    const auto result = __real_read(fd, buffer, count);
    std::lock_guard lock(hooks.mutex);
    if (valid && hooks.concealSize && info.st_dev == hooks.device && info.st_ino == hooks.inode) {
        hooks.largestRead = std::max(hooks.largestRead, qsizetype(count));
        if (result > 0) hooks.bytesRead += result;
    }
    return result;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QJsonArray cases;
    try {
        require(qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"),
            "working display disconnected");
        require(!qEnvironmentVariableIsEmpty("XDG_RUNTIME_DIR") && !qEnvironmentVariableIsEmpty("XDG_CONFIG_HOME")
            && !qEnvironmentVariableIsEmpty("XDG_DATA_HOME"), "private XDG directories required");
        QTemporaryDir temporary;
        require(temporary.isValid(), "private fixtures");
        const QUrl root = QUrl::fromLocalFile(temporary.path());
        const QString path = temporary.path() + "/valid.finder-search";
        const QUrl url = QUrl::fromLocalFile(path);
        auto object = definition(root);
        descriptor(path, object);
        SmartFolderReader reader;
        require(SmartFolderReader::isSmartFolderUrl(url), "recognize descriptor URL without I/O");
        for (const QUrl &invalid : {QUrl("file:relative.finder-search"), QUrl("file://host/tmp/a.finder-search"),
            QUrl("smb://host/a.finder-search"), QUrl("file:///tmp/a.finder-search?query"),
            QUrl("file:///tmp/a.finder-search#fragment"), QUrl("file:///tmp/a%00.finder-search"),
            QUrl("file:///tmp/a.txt")}) {
            require(!SmartFolderReader::isSmartFolderUrl(invalid), "unsafe descriptor URL recognized");
            expectError(reader, invalid);
        }
        auto result = read(reader, url);
        require(result.error.isEmpty() && result.root == root && result.definition.value("kind") == QJsonValue("image")
            && result.definition.value("name").toString().isEmpty(), "valid criteria-only descriptor");
        cases.append("pure URL recognition and valid criteria-only async result");

        object = {{"format", "macos-finder-linux-smart-folder"}, {"version", 1}, {"root", root.toString()}, {"content", "needle"}};
        descriptor(path, object);
        result = read(reader, url);
        require(result.error.isEmpty() && result.definition.value("scope") == QJsonValue("computer")
            && result.definition.value("maximumSize").toInteger() == -1, "optional criteria defaults");
        cases.append("content-only descriptor and current-format optional defaults");

        expectError(reader, QUrl::fromLocalFile(temporary.path() + "/missing.finder-search"));
        for (const QByteArray &bytes : {QByteArray(), QByteArray("{invalid"), QByteArray("[]"), QByteArray("null"),
            QByteArray("{} trailing")}) { fixture(path, bytes); expectError(reader, url); }
        auto valid = definition(root);
        for (const auto &entry : QList<QPair<QString, QJsonValue>>{
            {"format", "other"}, {"version", "1"}, {"version", 1.5}, {"version", 2},
            {"root", QJsonValue::Null}, {"name", 42}, {"content", true}, {"scope", "unknown"},
            {"kind", "unknown"}, {"minimumSize", "100"}, {"maximumSize", -2},
            {"modifiedAfter", 1.5}, {"modifiedBefore", 1e100}, {"minimumSize", QJsonValue::Null}}) {
            object = valid;
            object.insert(entry.first, entry.second);
            descriptor(path, object);
            expectError(reader, url);
        }
        for (const auto *key : {"format", "version", "root"}) {
            object = valid;
            object.remove(key);
            descriptor(path, object);
            expectError(reader, url);
        }
        for (const auto &keys : {std::pair{"minimumSize", "maximumSize"}, std::pair{"modifiedAfter", "modifiedBefore"}}) {
            object = valid;
            object.insert(keys.first, 20);
            object.insert(keys.second, 10);
            descriptor(path, object);
            expectError(reader, url);
        }
        cases.append("missing, malformed, unsupported, wrong-type and reversed-range descriptors");

        QByteArray boundary = QJsonDocument(valid).toJson(QJsonDocument::Compact);
        boundary.append(QByteArray(65536 - boundary.size(), ' '));
        fixture(path, boundary);
        require(read(reader, url).error.isEmpty(), "exactly 65,536 bytes accepted");
        fixture(path, boundary + ' ');
        expectError(reader, url);
        QFile huge(path);
        require(huge.open(QIODevice::WriteOnly) && huge.resize(1024 * 1024 * 1024), "sparse huge fixture");
        huge.close();
        expectError(reader, url);
        fixture(path, boundary + QByteArray(1024, ' '));
        struct stat info {};
        require(::stat(QFile::encodeName(path).constData(), &info) == 0, "growth fixture identity");
        {
            std::lock_guard lock(hooks.mutex);
            hooks.device = info.st_dev;
            hooks.inode = info.st_ino;
            hooks.concealSize = true;
        }
        expectError(reader, url);
        {
            std::lock_guard lock(hooks.mutex);
            require(hooks.bytesRead == 65537 && hooks.largestRead <= 65537,
                "actual read exceeded 65,537 bytes or relied on size");
            hooks.concealSize = false;
        }
        cases.append("65,536-byte boundary, sparse huge file and actual 65,537-byte read limit despite changed metadata");

        const QString fifo = temporary.path() + "/pipe.finder-search";
        require(::mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0, "FIFO fixture without writer");
        QElapsedTimer elapsed;
        elapsed.start();
        expectError(reader, QUrl::fromLocalFile(fifo));
        require(elapsed.elapsed() < 1000, "FIFO rejection was not immediate");
        const QString directory = temporary.path() + "/directory.finder-search";
        require(QDir().mkdir(directory), "descriptor directory fixture");
        expectError(reader, QUrl::fromLocalFile(directory));
        descriptor(path, valid);
        const QString link = temporary.path() + "/link.finder-search";
        require(::symlink(QFile::encodeName(path).constData(), QFile::encodeName(link).constData()) == 0, "descriptor symlink fixture");
        expectError(reader, QUrl::fromLocalFile(link));
        const QString device = temporary.path() + "/device.finder-search";
        require(::symlink("/dev/zero", QFile::encodeName(device).constData()) == 0, "device-link fixture");
        expectError(reader, QUrl::fromLocalFile(device));
        {
            std::lock_guard lock(hooks.mutex);
            hooks.deviceAlias = QFile::encodeName(temporary.path() + "/character.finder-search");
        }
        expectError(reader, QUrl::fromLocalFile(temporary.path() + "/character.finder-search"));
        {
            std::lock_guard lock(hooks.mutex);
            hooks.deviceAlias.clear();
        }
        cases.append("FIFO without writer immediately rejected; directories, symlinks and a character-device inode rejected");

        for (const QString &rootText : {QString(), QString("relative/folder"), QString("file:relative"),
            QString("file:///missing-smartfolder-test-root"), url.toString(), QUrl::fromLocalFile(directory).toString(),
            QString("javascript:/folder"), QString("https://host/folder"), QString("sftp:/folder"),
            QString("file://host/tmp"), QString("file:///tmp?query"), QString("file:///tmp#fragment"),
            QString("file:///tmp/%00"), QString("file:///tmp/%zz"), QString("smb://host/other.finder-search/"),
            QString("trash://host/"), QString("sftp://host")}) {
            object = valid;
            object.insert("root", rootText);
            descriptor(path, object);
            expectError(reader, url);
        }
        const QString regularRoot = temporary.path() + "/regular";
        fixture(regularRoot, "regular root");
        object = definition(QUrl::fromLocalFile(regularRoot));
        descriptor(path, object);
        expectError(reader, url);
        for (const QString &rootText : {QString("smb://server/share"), QString("sftp://server/path"),
            QString("ftp://server/path"), QString("fish://server/path"), QString("webdav://server/path"),
            QString("webdavs://server/path"), QString("smb:/"), QString("trash:/"), QString("remote:/"), QString("mtp:/")}) {
            object = valid;
            object.insert("root", rootText);
            descriptor(path, object);
            result = read(reader, url);
            require(result.error.isEmpty() && result.root == QUrl(rootText), "supported explicit remote folder URL");
        }
        object = definition(QUrl::fromLocalFile(temporary.path() + "/./"));
        descriptor(path, object);
        result = read(reader, url);
        require(result.error.isEmpty() && result.root == root
            && result.definition.value("root").toString() == root.toString(), "normalized local folder URL");
        cases.append("explicit supported root URLs, worker local-directory validation and recursive Smart Folder rejection");

        int canceledCalls = 0;
        descriptor(path, valid);
        const auto canceledGeneration = reader.read(url, [&](const auto &, const auto &, const auto &, quint64) { ++canceledCalls; });
        reader.cancel();
        require(reader.generation() > canceledGeneration && !reader.isRunning(), "cancel invalidates generation immediately");
        drain();
        require(canceledCalls == 0, "canceled completion delivered");
        int staleCalls = 0;
        BeforeDelivery beforeDelivery;
        reader.installEventFilter(&beforeDelivery);
        const auto staleGeneration = reader.read(QUrl::fromLocalFile(fifo), [&](const auto &, const auto &, const auto &, quint64) { ++staleCalls; });
        // Replace exactly when the old error's queued call is about to run.
        bool replacementEnded = false;
        beforeDelivery.before = [&] {
            reader.read(url, [&](const auto &value, const auto &folder, const auto &error, quint64 generation) {
                result = {value, folder, error, generation, true};
                replacementEnded = true;
            });
        };
        require(waitFor([&] { return replacementEnded; }), "queued stale error replaced");
        require(result.error.isEmpty() && result.generation > staleGeneration && staleCalls == 0, "queued stale error delivered");
        reader.read(url, [&](const auto &, const auto &, const auto &, quint64) { ++staleCalls; });
        beforeDelivery.before = [&] { reader.cancel(); };
        require(waitFor([&] { return !reader.isRunning(); }), "queued success canceled");
        require(staleCalls == 0, "queued canceled success delivered");
        reader.removeEventFilter(&beforeDelivery);
        cases.append("cancel generation, queued stale error replacement and queued success cancellation");

        int reentrantCalls = 0;
        quint64 nextGeneration = 0;
        reader.read(url, [&](const auto &, const auto &, const auto &, quint64 firstGeneration) {
            ++reentrantCalls;
            nextGeneration = reader.read(url, [&, firstGeneration](const auto &, const auto &, const auto &error, quint64 value) {
                require(error.isEmpty() && value > firstGeneration && value == nextGeneration, "reentrant generation");
                ++reentrantCalls;
            });
        });
        require(waitFor([&] { return reentrantCalls == 2; }), "completion can start next request");
        QPointer<SmartFolderReader> selfDeleting = new SmartFolderReader;
        selfDeleting->read(url, [&](const auto &, const auto &, const auto &, quint64) { delete selfDeleting.data(); });
        require(waitFor([&] { return selfDeleting.isNull(); }), "completion can destroy reader");
        cases.append("callbacks safely start a new read or destroy the controller");

        const QString blockedA = temporary.path() + "/blocked-a.finder-search";
        const QString blockedB = temporary.path() + "/blocked-b.finder-search";
        descriptor(blockedA, valid);
        descriptor(blockedB, valid);
        {
            std::lock_guard lock(hooks.mutex);
            hooks.gated = {QFile::encodeName(blockedA), QFile::encodeName(blockedB)};
        }
        SmartFolderReader blockedReader;
        auto destroyedReader = std::make_unique<SmartFolderReader>();
        int obsoleteCalls = 0;
        blockedReader.read(QUrl::fromLocalFile(blockedA), [&](const auto &, const auto &, const auto &, quint64) { ++obsoleteCalls; });
        destroyedReader->read(QUrl::fromLocalFile(blockedB), [&](const auto &, const auto &, const auto &, quint64) { ++obsoleteCalls; });
        require(waitFor([] { std::lock_guard lock(hooks.mutex); return hooks.entered == 2; }), "two worker gate entries");
        int heartbeats = 0;
        QTimer heartbeat;
        QObject::connect(&heartbeat, &QTimer::timeout, &app, [&] { ++heartbeats; });
        heartbeat.start(1);
        elapsed.restart();
        for (int i = 0; i < 2000; ++i)
            blockedReader.read(QUrl::fromLocalFile(temporary.path() + QString("/superseded-%1.finder-search").arg(i)),
                [&](const auto &, const auto &, const auto &, quint64) { ++obsoleteCalls; });
        require(elapsed.elapsed() < 1000, "request storm blocked caller");
        bool newestEnded = false;
        quint64 newestGeneration = 0;
        newestGeneration = blockedReader.read(url, [&](const auto &value, const auto &folder, const auto &error, quint64 generation) {
            require(error.isEmpty() && !value.isEmpty() && folder == root && generation == newestGeneration, "coalesced newest request");
            newestEnded = true;
        });
        SmartFolderReader queuedReader;
        queuedReader.read(QUrl::fromLocalFile(fifo), [&](const auto &, const auto &, const auto &, quint64) { ++obsoleteCalls; });
        bool queuedEnded = false;
        queuedReader.read(url, [&](const auto &, const auto &, const auto &error, quint64) {
            require(error.isEmpty(), "queued reader completion");
            queuedEnded = true;
        });
        drain();
        require(heartbeats > 0 && !queuedEnded && !newestEnded, "GUI responsive and pool limited with both workers blocked");
        elapsed.restart();
        destroyedReader.reset();
        require(elapsed.elapsed() < 100, "destructor waited for blocked worker");
        queuedReader.cancel();
        require(!queuedReader.isRunning(), "cancel queued request immediately");
        queuedReader.read(url, [&](const auto &, const auto &, const auto &error, quint64) {
            require(error.isEmpty(), "replacement after queued cancellation");
            queuedEnded = true;
        });
        releaseGates();
        require(waitFor([&] { return newestEnded && queuedEnded; }), "coalesced pool finishes after slow filesystem recovers");
        drain();
        {
            std::lock_guard lock(hooks.mutex);
            require(hooks.peak == 2 && hooks.active == 0 && hooks.supersededOpens == 0, "worker bound or pending coalescing broken");
        }
        require(obsoleteCalls == 0, "obsolete or destroyed-reader callback delivered");
        cases.append("two workers globally, event-loop heartbeat during blocked opens, 2,000-request coalescing, nonblocking destruction and cancellation");

        std::puts(QJsonDocument(QJsonObject{{"passed", true}, {"tests", cases.size()}, {"cases", cases}}).toJson(QJsonDocument::Compact).constData());
    } catch (const std::exception &error) {
        releaseGates();
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    return 0;
}
