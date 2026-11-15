// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
// Private standalone target: this file + src/archives.cpp/.h, AUTOMOC;
// Qt6::Widgets KF6::Archive KF6::KIOCore KF6::KIOGui KF6::KIOWidgets Threads::Threads.
// Run using tests/run-headless.py with a private session D-Bus/XDG environment.
// No application windows/input on the working desktop; all writes are fixtures.
#include "archives.h"

#include <KArchiveDirectory>
#include <KIO/FileUndoManager>
#include <KIO/CopyJob>
#include <KZip>
#include <QApplication>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtEndian>
#include <functional>
#include <fcntl.h>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace {
void check(bool result, const char *message)
{
    if (!result) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
bool waitFor(const std::function<bool()> &condition, int timeout = 15000)
{
    QElapsedTimer clock; clock.start();
    while (!condition() && clock.elapsed() < timeout) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return condition();
}
void write(const QString &path, const QByteArray &data)
{
    QFile file(path); check(file.open(QIODevice::WriteOnly), "create fixture"); check(file.write(data) == data.size(), "write fixture");
}
QByteArray bytes(const QString &path)
{
    QFile file(path); check(file.open(QIODevice::ReadOnly), "read fixture"); return file.readAll();
}
void modeTime(const QString &path, mode_t mode, time_t timestamp)
{
    check(::chmod(QFile::encodeName(path).constData(), mode) == 0, "fixture mode");
    timespec times[2] {{timestamp, 0}, {timestamp, 0}};
    check(::utimensat(AT_FDCWD, QFile::encodeName(path).constData(), times, 0) == 0, "fixture time");
}
mode_t permissions(const QString &path)
{
    struct stat metadata {}; check(::stat(QFile::encodeName(path).constData(), &metadata) == 0, "stat fixture"); return metadata.st_mode & 0777;
}
struct Observer {
    int completed = 0;
    bool cancelled = false;
    QString error;
    QList<QUrl> outputs;
    qint64 progress = -1, total = -1;
    explicit Observer(Archives &archives) {
        QObject::connect(&archives, &Archives::errorOccurred, &archives, [this](const QString &text) { error = text; });
        QObject::connect(&archives, &Archives::outputCreated, &archives, [this](const QUrl &url) { outputs.append(url); });
        QObject::connect(&archives, &Archives::progressChanged, &archives, [this](qint64 count, qint64 maximum) { progress = count; total = maximum; });
        QObject::connect(&archives, &Archives::finished, &archives, [this](bool value) { ++completed; cancelled = value; });
    }
    void reset() { cancelled = false; error.clear(); outputs.clear(); progress = total = -1; }
    void finish(Archives &archives, int expected) {
        check(waitFor([&] { return !archives.busy() && completed == expected; }), "bounded asynchronous completion");
        if (!error.isEmpty()) std::cerr << "Archive result: " << error.toStdString() << '\n';
    }
};
void add16(QByteArray &data, quint16 value) { const auto size = data.size(); data.resize(size + 2); qToLittleEndian(value, data.data() + size); }
void add32(QByteArray &data, quint32 value) { const auto size = data.size(); data.resize(size + 4); qToLittleEndian(value, data.data() + size); }
void add64(QByteArray &data, quint64 value) { const auto size = data.size(); data.resize(size + 8); qToLittleEndian(value, data.data() + size); }
quint32 crc(const QByteArray &data)
{
    quint32 value = 0xffffffff;
    for (auto byte : data) { value ^= quint8(byte); for (int i = 0; i < 8; ++i) value = (value >> 1) ^ ((value & 1) ? 0xedb88320U : 0); }
    return value ^ 0xffffffff;
}
struct Entry {
    QByteArray name, data;
    mode_t mode = S_IFREG | 0644;
    quint16 method = 0, flags = 0x0800;
    qint64 declaredSize = -1;
};
QByteArray fixture(const QList<Entry> &entries, bool zip64 = false)
{
    QByteArray zip, central;
    for (const auto &entry : entries) {
        const quint32 offset = zip.size();
        const quint32 size = entry.declaredSize < 0 ? entry.data.size() : entry.declaredSize;
        const quint32 checksum = crc(entry.data);
        QByteArray extra;
        if (zip64) { add16(extra, 1); add16(extra, 16); add64(extra, size); add64(extra, entry.data.size()); }
        add32(zip, 0x04034b50); add16(zip, zip64 ? 45 : 20); add16(zip, entry.flags); add16(zip, entry.method);
        add16(zip, 0); add16(zip, 0x5b61); // 2025-11-01 00:00:00.
        add32(zip, entry.flags & 8 ? 0 : checksum);
        add32(zip, zip64 ? 0xffffffff : entry.flags & 8 ? 0 : entry.data.size());
        add32(zip, zip64 ? 0xffffffff : entry.flags & 8 ? 0 : size);
        add16(zip, entry.name.size()); add16(zip, extra.size()); zip += entry.name + extra + entry.data;
        if (entry.flags & 8) { add32(zip, 0x08074b50); add32(zip, checksum); add32(zip, entry.data.size()); add32(zip, size); }
        if (zip64) { extra.clear(); add16(extra, 1); add16(extra, 24); add64(extra, size); add64(extra, entry.data.size()); add64(extra, offset); }
        add32(central, 0x02014b50); add16(central, (3 << 8) | (zip64 ? 45 : 20)); add16(central, zip64 ? 45 : 20);
        add16(central, entry.flags); add16(central, entry.method); add16(central, 0); add16(central, 0x5b61);
        add32(central, checksum); add32(central, zip64 ? 0xffffffff : entry.data.size()); add32(central, zip64 ? 0xffffffff : size);
        add16(central, entry.name.size()); add16(central, extra.size()); add16(central, 0); add16(central, 0); add16(central, 0);
        add32(central, quint32(entry.mode) << 16); add32(central, zip64 ? 0xffffffff : offset); central += entry.name + extra;
    }
    const quint32 offset = zip.size(); zip += central;
    if (zip64) {
        const quint64 position = zip.size();
        add32(zip, 0x06064b50); add64(zip, 44); add16(zip, (3 << 8) | 45); add16(zip, 45); add32(zip, 0); add32(zip, 0);
        add64(zip, entries.size()); add64(zip, entries.size()); add64(zip, central.size()); add64(zip, offset);
        add32(zip, 0x07064b50); add32(zip, 0); add64(zip, position); add32(zip, 1);
    }
    add32(zip, 0x06054b50); add16(zip, 0); add16(zip, 0);
    add16(zip, zip64 ? 0xffff : entries.size()); add16(zip, zip64 ? 0xffff : entries.size());
    add32(zip, zip64 ? 0xffffffff : central.size()); add32(zip, zip64 ? 0xffffffff : offset); add16(zip, 0);
    return zip;
}
}

int main(int argc, char **argv)
{
    check(qEnvironmentVariable("QT_QPA_PLATFORM") == "offscreen", "offscreen test required");
    check(qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"), "working displays disconnected");
    check(!qEnvironmentVariableIsEmpty("XDG_RUNTIME_DIR") && !qEnvironmentVariableIsEmpty("XDG_CONFIG_HOME")
        && !qEnvironmentVariableIsEmpty("XDG_DATA_HOME") && !qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS"), "private XDG/session required");
    QApplication app(argc, argv);
    // KIO refuses Trash on /tmp's pseudo-filesystem when it is a different
    // device from the login home. Keep fixtures in a private directory on the
    // home filesystem; the worker's Trash still uses PRIVATE XDG_DATA_HOME.
    QTemporaryDir files(QDir::homePath() + QStringLiteral("/.finder-archives-test-XXXXXX")); check(files.isValid(), "private fixtures");
    auto path = [&](const QString &name) { return files.filePath(name); };
    auto url = [&](const QString &name) { return QUrl::fromLocalFile(path(name)); };
    const QByteArray original("Finder portable ZIP\0binary\n", 27);
    write(path(QStringLiteral("report final.α.txt")), original);
    write(path(QStringLiteral("--option.txt")), "literal option filename");
    check(QDir().mkpath(path("Project/nested/empty")), "nested empty folder");
    write(path("Project/nested/run.sh"), "#!/bin/sh\necho portable\n");
    write(path("Project/.hidden"), "hidden bytes");
    write(path("Project/zero"), {});
    constexpr time_t timestamp = 1700000001;
    modeTime(path("Project/nested/run.sh"), 0751, timestamp);
    modeTime(path("Project/nested/empty"), 0750, timestamp);
    modeTime(path("Project/nested"), 0750, timestamp);
    modeTime(path("Project"), 0750, timestamp);
    check(!Archives::canCompress({}).enabled && !Archives::canCompress({QUrl("sftp://host/file")}).enabled, "compression protocol/selection capability");
    check(!Archives::canExtract(QUrl("https://host/file.zip")).enabled && !Archives::canExtract(url("file.tar")).enabled, "extraction protocol/type capability");
    check(Archives::canExtract(url("file.ZIP")).enabled, "ZIP extension capability");
    check(Archives::suggestedArchiveName({url("report final.α.txt")}) == QStringLiteral("report final.α.txt.zip"), "single full filename naming");
    check(Archives::suggestedArchiveName({url("one"), url("two")}) == "Archive.zip", "multi naming");

    Archives archives; Observer observed(archives); int completed = 0;
    int undo = 0;
    QObject::connect(KIO::FileUndoManager::self(), &KIO::FileUndoManager::undoJobFinished, &app, [&] { ++undo; });
    archives.compress({url("report final.α.txt")}); observed.finish(archives, ++completed);
    check(observed.error.isEmpty() && observed.outputs == QList<QUrl>{url("report final.α.txt.zip")}, "native single ZIP publication");
    const auto singleZip = bytes(path("report final.α.txt.zip"));
    check((qFromLittleEndian<quint16>(singleZip.constData() + 6) & 0x0800) != 0, "portable ZIP UTF-8 filename flag");
    KZip single(path("report final.α.txt.zip")); check(single.open(QIODevice::ReadOnly), "read created portable ZIP");
    const auto *record = dynamic_cast<const KArchiveFile *>(single.directory()->entry(QStringLiteral("report final.α.txt")));
    check(record && record->data() == original, "ZIP original bytes and UTF-8 path"); single.close();
    QProcess unzip; unzip.start("/usr/bin/unzip", {"-t", path("report final.α.txt.zip")});
    check(unzip.waitForFinished(5000) && unzip.exitCode() == 0, "independent Info-ZIP integrity oracle");
    check(observed.progress == original.size() && observed.total == original.size(), "bounded staging byte progress");
    KIO::FileUndoManager::self()->undo(); check(waitFor([&] { return undo == 1; }), "ZIP native Undo completes");
    check(!QFile::exists(path("report final.α.txt.zip")) && bytes(path("report final.α.txt")) == original, "Undo removes ZIP only");
    write(path("report final.α.txt.zip"), singleZip);
    observed.reset(); archives.compress({url("report final.α.txt")}); observed.finish(archives, ++completed);
    check(observed.error.isEmpty() && observed.outputs == QList<QUrl>{url("report final.α.txt 2.zip")}
        && bytes(path("report final.α.txt.zip")) == singleZip, "collision-safe single ZIP naming");
    observed.reset(); archives.extract(url("report final.α.txt.zip")); observed.finish(archives, ++completed);
    check(observed.error.isEmpty() && observed.outputs == QList<QUrl>{url("report final.α.txt 2")}
        && bytes(path("report final.α.txt 2")) == original, "extract direct single item beside ZIP without overwrite");
    KIO::FileUndoManager::self()->undo(); check(waitFor([&] { return undo == 2; }) && !QFile::exists(path("report final.α.txt 2")), "extract native Undo");

    observed.reset(); archives.compress({url("Project")}); observed.finish(archives, ++completed);
    check(observed.error.isEmpty() && observed.outputs == QList<QUrl>{url("Project.zip")}, "folder compression");
    observed.reset(); archives.extract(url("Project.zip")); observed.finish(archives, ++completed);
    check(observed.error.isEmpty() && observed.outputs == QList<QUrl>{url("Project 2")}, "single root folder extraction");
    check(QFileInfo(path("Project 2/nested/empty")).isDir() && bytes(path("Project 2/nested/run.sh")) == bytes(path("Project/nested/run.sh"))
        && bytes(path("Project 2/.hidden")) == "hidden bytes" && QFileInfo(path("Project 2/zero")).size() == 0, "hierarchy/hidden/empty/zero preserved");
    check(permissions(path("Project 2/nested/run.sh")) == 0751 && permissions(path("Project 2/nested/empty")) == 0750
        && permissions(path("Project 2")) == 0750, "file and directory permissions preserved");
    check(QFileInfo(path("Project 2/nested/run.sh")).lastModified().toSecsSinceEpoch() == timestamp
        && QFileInfo(path("Project 2")).lastModified().toSecsSinceEpoch() == timestamp, "file and folder modification times preserved");
    KIO::FileUndoManager::self()->undo(); check(waitFor([&] { return undo == 3; }) && !QFile::exists(path("Project 2")), "one native Undo removes folder hierarchy");

    write(path("Archive.zip"), "keep existing archive");
    observed.reset(); archives.compress({url("report final.α.txt"), url("--option.txt"), url("Project")}); observed.finish(archives, ++completed);
    check(observed.error.isEmpty() && observed.outputs == QList<QUrl>{url("Archive 2.zip")} && bytes(path("Archive.zip")) == "keep existing archive", "multi ZIP collision-safe naming");
    observed.reset(); archives.extract(url("Archive 2.zip")); observed.finish(archives, ++completed);
    check(observed.error.isEmpty() && observed.outputs == QList<QUrl>{url("Archive 2")}
        && bytes(path("Archive 2/report final.α.txt")) == original && bytes(path("Archive 2/--option.txt")) == "literal option filename"
        && QFileInfo(path("Archive 2/Project/nested/empty")).isDir(), "multiple roots grouped beside archive with original hierarchy");
    KIO::FileUndoManager::self()->undo(); check(waitFor([&] { return undo == 4; }) && !QFile::exists(path("Archive 2")), "grouped output one Undo");

    // A conflict introduced after name selection exercises KIO's own race
    // handling and reports the actual renamed target, preserving the occupant.
    write(path("race.zip"), fixture({{"race.txt", "archive race bytes"}}));
    auto raceConnection = QObject::connect(&archives, &Archives::statusChanged, &archives, [&](const QString &text) {
        if (text == "Saving archive output…") write(path("race.txt"), "racing occupant");
    });
    observed.reset(); archives.extract(url("race.zip")); observed.finish(archives, ++completed);
    QObject::disconnect(raceConnection);
    check(observed.error.isEmpty() && observed.outputs.size() == 1 && observed.outputs.first() != url("race.txt")
        && bytes(observed.outputs.first().toLocalFile()) == "archive race bytes" && bytes(path("race.txt")) == "racing occupant", "KIO race conflict/actual renamed URL");

    auto reject = [&](const QByteArray &data, const char *message) {
        write(path("hostile.zip"), data); observed.reset(); archives.extract(url("hostile.zip")); observed.finish(archives, ++completed);
        check(!observed.error.isEmpty() && observed.outputs.isEmpty() && !QFile::exists(path("hostile"))
            && bytes(path("hostile.zip")) == data, message);
    };
    for (const auto &name : {QByteArray("../escape"), QByteArray("safe/../../escape"), QByteArray("/absolute"), QByteArray("C:/drive"),
                            QByteArray("back\\escape"), QByteArray("a/./b"), QByteArray("a//b"), QByteArray("a\0b", 3)})
        reject(fixture({{name, "evil"}}), "ZIP-slip/nonportable/NUL paths rejected");
    reject(fixture({{"link", "../../escape", S_IFLNK | 0777}}), "ZIP symlink rejected before writes");
    reject(fixture({{"device", {}, S_IFCHR | 0600}}), "special ZIP entry rejected");
    reject(fixture({{"duplicate", "first"}, {"duplicate", "second"}}), "duplicate paths rejected");
    reject(fixture({{"a", "file"}, {"a/child", "nested"}}), "file/directory conflict rejected");
    reject(fixture({{"secret", "encrypted", S_IFREG | 0644, 0, 1}}), "encrypted ZIP capability error");
    reject(fixture({{"method", "unsupported", S_IFREG | 0644, 99}}), "unsupported compression error");
    reject(fixture({{"bomb", "small", S_IFREG | 0644, 0, 0x0800, 2LL * 1024 * 1024 * 1024}}), "declared decompression bomb rejected");
    auto mismatched = fixture({{"good", "bytes"}}); mismatched[30] = 'e'; reject(mismatched, "local/central path mismatch rejected");
    auto damaged = fixture({{"damaged", "content"}}); damaged[30 + 7] = 'X'; reject(damaged, "CRC validation catches damaged payload");
    reject(fixture({{"short", "small", S_IFREG | 0644, 0, 0x0800, 1}}), "actual decompressed output budget enforced");
    const QByteArray inflated(1024 * 1024, 'z');
    const QByteArray zlib = qCompress(inflated);
    auto deflateBomb = fixture({{"deflate-bomb", zlib.mid(2, zlib.size() - 6), S_IFREG | 0644, 8, 0x0800, 1}});
    const int central = deflateBomb.indexOf(QByteArray("PK\x01\x02", 4));
    qToLittleEndian<quint32>(crc(inflated), deflateBomb.data() + 14);
    qToLittleEndian<quint32>(crc(inflated), deflateBomb.data() + central + 16);
    reject(deflateBomb, "streamed actual deflate expansion exceeds declared bytes");
    auto truncated = fixture({{"truncated", "data"}}); truncated.chop(8); reject(truncated, "truncated ZIP rejected");
    auto multiDisk = fixture({{"multi", "data"}}); qToLittleEndian<quint16>(1, multiDisk.data() + multiDisk.size() - 18); reject(multiDisk, "multi-volume ZIP rejected");
    QByteArray deep;
    for (int i = 0; i < 65; ++i) deep += "x/";
    reject(fixture({{deep + "deep", "bytes"}}), "ZIP depth limit enforced");
    QByteArray oversizedName(256, 'a'); reject(fixture({{oversizedName, "bytes"}}), "filename bound enforced");
    reject(fixture({{"no-access", "bytes", S_IFREG | 0000}}), "unpublishable metadata reports unsupported mode");
    check(!QFile::exists(files.path() + "/../escape"), "no ZIP-slip output outside private fixture");

    // Valid streaming descriptor, ZIP64 and independent raw fixture inputs.
    write(path("descriptor.zip"), fixture({{"descriptor.txt", "streamed", S_IFREG | 0644, 0, 0x0808}}));
    observed.reset(); archives.extract(url("descriptor.zip")); observed.finish(archives, ++completed);
    check(observed.error.isEmpty() && observed.outputs == QList<QUrl>{url("descriptor.txt")} && bytes(path("descriptor.txt")) == "streamed", "streaming ZIP descriptor");
    write(path("wide.zip"), fixture({{"wide.txt", "ZIP64"}}, true));
    observed.reset(); archives.extract(url("wide.zip")); observed.finish(archives, ++completed);
    check(observed.error.isEmpty() && bytes(path("wide.txt")) == "ZIP64", "bounded ZIP64 sizes and offsets");
    write(path("empty.zip"), fixture({}));
    observed.reset(); archives.extract(url("empty.zip")); observed.finish(archives, ++completed);
    check(observed.error.isEmpty() && QFileInfo(path("empty")).isDir(), "empty ZIP produces empty named folder");

    FinderArchives::Limits low; low.entries = 2; low.expandedBytes = 32; low.depth = 2;
    Archives limited(nullptr, low); Observer limitedObserved(limited); int smallCompleted = 0;
    auto rejectLimited = [&](const QByteArray &data) {
        write(path("limited.zip"), data); limitedObserved.reset(); limited.extract(url("limited.zip")); limitedObserved.finish(limited, ++smallCompleted);
        check(!limitedObserved.error.isEmpty() && limitedObserved.outputs.isEmpty(), "reduced resource cap pre-publication");
    };
    rejectLimited(fixture({{"one", {}}, {"two", {}}, {"three", {}}}));
    rejectLimited(fixture({{"a/b/c", {}}}));
    rejectLimited(fixture({{"large", QByteArray(33, 'x')}}));
    limitedObserved.reset(); limited.compress({url("Project")}); limitedObserved.finish(limited, ++smallCompleted);
    check(!limitedObserved.error.isEmpty() && limitedObserved.outputs.isEmpty(), "compression entry/depth caps");
    FinderArchives::Limits tinyOutput; tinyOutput.archiveBytes = 32;
    Archives outputLimited(nullptr, tinyOutput); Observer outputObserved(outputLimited);
    outputLimited.compress({url("report final.α.txt")}); outputObserved.finish(outputLimited, 1);
    check(!outputObserved.error.isEmpty() && outputObserved.outputs.isEmpty(), "compressed output write bound");
    outputObserved.reset(); outputLimited.extract(url("report final.α.txt.zip")); outputObserved.finish(outputLimited, 2);
    check(!outputObserved.error.isEmpty() && outputObserved.outputs.isEmpty(), "compressed input snapshot bound");

    check(::symlink(QFile::encodeName(path("Project")).constData(), QFile::encodeName(path("linked-folder")).constData()) == 0, "symlink fixture");
    observed.reset(); archives.compress({url("linked-folder")}); observed.finish(archives, ++completed);
    check(!observed.error.isEmpty() && observed.outputs.isEmpty(), "compression symlink source rejected");
    observed.reset(); archives.compress({url("linked-folder/nested/run.sh")}); observed.finish(archives, ++completed);
    check(!observed.error.isEmpty() && observed.outputs.isEmpty(), "ancestor symlink rejected");
    check(::symlink(QFile::encodeName(path("Project.zip")).constData(), QFile::encodeName(path("linked.zip")).constData()) == 0, "linked ZIP fixture");
    observed.reset(); archives.extract(url("linked.zip")); observed.finish(archives, ++completed);
    check(!observed.error.isEmpty() && observed.outputs.isEmpty(), "extract symlink source rejected");
    check(::mkfifo(QFile::encodeName(path("fifo")).constData(), 0600) == 0, "FIFO fixture");
    observed.reset(); archives.compress({url("fifo")}); observed.finish(archives, ++completed);
    check(!observed.error.isEmpty() && observed.outputs.isEmpty(), "special source never blocks worker");
    observed.reset(); archives.extract(url("missing.zip")); observed.finish(archives, ++completed);
    check(!observed.error.isEmpty() && observed.outputs.isEmpty(), "native missing-source error");
    observed.reset(); archives.compress({url("report final.α.txt")}, url("missing-output")); observed.finish(archives, ++completed);
    check(!observed.error.isEmpty() && observed.outputs.isEmpty(), "native destination error");

    // Native KIO error after staging; no output or original mutation.
    check(QDir().mkdir(path("readonly")), "readonly fixture");
    auto permissionConnection = QObject::connect(&archives, &Archives::statusChanged, &archives, [&](const QString &text) {
        if (text == "Saving archive output…") check(::chmod(QFile::encodeName(path("readonly")).constData(), 0500) == 0, "remove publish permission");
    });
    QTimer errorDialogs; errorDialogs.setInterval(10);
    QObject::connect(&errorDialogs, &QTimer::timeout, &app, [] {
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *dialog = qobject_cast<QDialog *>(widget); dialog && dialog->isVisible()) dialog->accept();
    });
    errorDialogs.start();
    observed.reset(); archives.compress({url("report final.α.txt")}, url("readonly")); observed.finish(archives, ++completed);
    errorDialogs.stop();
    QObject::disconnect(permissionConnection);
    check(!observed.error.isEmpty() && observed.outputs.isEmpty(), "native KIO permission error delivered to host");
    check(::chmod(QFile::encodeName(path("readonly")).constData(), 0700) == 0, "restore fixture permissions");

    observed.reset(); archives.compress({url("Project")}); check(archives.busy(), "busy immediate before async work"); archives.cancel(); observed.finish(archives, ++completed);
    check(observed.cancelled && observed.error.isEmpty() && observed.outputs.isEmpty(), "cooperative pre-publication cancel");
    auto cancelConnection = QObject::connect(&archives, &Archives::statusChanged, &archives, [&](const QString &text) { if (text == "Saving archive output…") archives.cancel(); });
    observed.reset(); archives.compress({url("--option.txt")}); observed.finish(archives, ++completed); QObject::disconnect(cancelConnection);
    check(observed.cancelled && observed.outputs.isEmpty() && !QFile::exists(path("--option.txt.zip")), "cancel from host publication signal");

    write(path("large.dat"), QByteArray(32 * 1024 * 1024, 'x'));
    int ticks = 0; QTimer responsiveness; responsiveness.setInterval(1); QObject::connect(&responsiveness, &QTimer::timeout, &app, [&] { ++ticks; }); responsiveness.start();
    observed.reset(); archives.compress({url("large.dat")}); observed.finish(archives, ++completed); responsiveness.stop();
    check(observed.error.isEmpty() && ticks > 10, "compression leaves GUI event loop responsive");
    FinderArchives::Limits deadline; deadline.processingMilliseconds = 1;
    Archives timeLimited(nullptr, deadline); Observer timed(timeLimited);
    timeLimited.compress({url("large.dat")}); timed.finish(timeLimited, 1);
    check(!timed.error.isEmpty() && timed.outputs.isEmpty(), "worker processing deadline");
    const auto liveCancel = QObject::connect(&archives, &Archives::progressChanged, &archives, [&](qint64 count, qint64) {
        if (count > 0) archives.cancel();
    });
    observed.reset(); archives.compress({url("large.dat")}); observed.finish(archives, ++completed); QObject::disconnect(liveCancel);
    check(observed.cancelled && observed.outputs.isEmpty(), "cancel during streamed work before publication");

    QList<Entry> partialEntries;
    for (int i = 0; i < 200; ++i) partialEntries.append({"partial-root/file-" + QByteArray::number(i), QByteArray(4096, 'p')});
    write(path("partial-root.zip"), fixture(partialEntries));
    QTimer partialCancel; partialCancel.setInterval(1);
    QObject::connect(&partialCancel, &QTimer::timeout, &archives, [&] {
        if (!QDir(path("partial-root")).entryList(QDir::Files).isEmpty()) { partialCancel.stop(); archives.cancel(); }
    });
    observed.reset(); partialCancel.start(); archives.extract(url("partial-root.zip")); observed.finish(archives, ++completed); partialCancel.stop();
    check(observed.cancelled && observed.outputs == QList<QUrl>{url("partial-root")} && QFileInfo(path("partial-root")).isDir(), "native publish cancellation reports retained partial root");
    errorDialogs.start();
    KIO::FileUndoManager::self()->undo();
    // Native Put/Trash can emit undoJobFinished twice in KIO 6.24. The output
    // disappearance and unlocked manager establish the completed operation.
    const bool undone = waitFor([&] { return undo >= 5 && !QFile::exists(path("partial-root")); });
    check(undone && !QFile::exists(path("partial-root")), "native partial output remains one Undo");
    errorDialogs.stop();
    QDir privateTrash(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/Trash"));
    // Remove only this uniquely named fixture from the private test Trash.
    const auto trashed = QDir(privateTrash.filePath("files")).entryList({QStringLiteral("partial-root*")}, QDir::AllEntries | QDir::NoDotAndDotDot);
    for (const auto &item : trashed) {
        QDir(privateTrash.filePath("files/" + item)).removeRecursively();
        QFile::remove(privateTrash.filePath("info/" + item + ".trashinfo"));
    }

    for (int i = 0; i < 6; ++i) {
        auto *closing = new Archives;
        closing->compress({url("large.dat")}); delete closing;
    }
    check(bytes(path("report final.α.txt")) == original && bytes(path("Project/nested/run.sh")) == "#!/bin/sh\necho portable\n"
        && bytes(path("report final.α.txt.zip")) == singleZip, "original files and ZIP unchanged across all operations");
    std::cout << "PASS: portable single/multiple/folder ZIP, independent unzip integrity, collision/KIO race, native Undo\n";
    std::cout << "PASS: hierarchy/hidden/empty/permissions/times, ZIP64/descriptor, unsafe paths/links/CRC/bombs/caps\n";
    std::cout << "PASS: protocols/native errors, progress/cancel, responsive worker and nonblocking destruction\n";
    return 0;
}
