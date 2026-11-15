// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#include "archives.h"

#include <KArchiveDirectory>
#include <KIO/CopyJob>
#include <KIO/FileUndoManager>
#include <KIO/JobUiDelegateFactory>
#include <KIO/SimpleJob>
#include <KJobUiDelegate>
#include <KZip>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRunnable>
#include <QSet>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QWidget>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <sys/stat.h>
#include <unistd.h>

using FinderArchives::Limits;

struct ArchiveTemporary : QTemporaryDir {
    QStringList directories;
    explicit ArchiveTemporary(const QString &pattern) : QTemporaryDir(pattern) {}
    ~ArchiveTemporary() {
        // ZIP read-only directories keep their actual permissions for KIO.
        // Make only our recorded staging directories removable on teardown.
        for (const auto &directory : directories)
            ::chmod(QFile::encodeName(directory).constData(), 0700);
    }
};

struct ArchivesState {
    bool busy = false;
    quint64 generation = 0;
    Limits limits;
    std::shared_ptr<std::atomic_bool> cancelled;
    std::shared_ptr<ArchiveTemporary> temporary;
    QString staged;
    QUrl destination;
    QList<QUrl> outputs;
    QHash<QString, int> directoryModes;
    QList<QPair<QUrl, int>> publishedDirectories;
    QPointer<KJob> job;
};

class ArchiveTask : public QObject {
    Q_OBJECT
signals:
    void done();
    void progress(qint64 completed, qint64 total);
};

namespace {
constexpr qsizetype chunkSize = 65536;
constexpr int pathBytesLimit = 4096;
struct Fd {
    int value = -1;
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
};
struct Work {
    Limits limits;
    std::shared_ptr<std::atomic_bool> cancelled;
    QElapsedTimer clock, progressClock;
    std::function<void(qint64, qint64)> progress;
    QString error;
    qint64 processed = 0, total = 0;
    Work(Limits caps, std::shared_ptr<std::atomic_bool> flag,
         std::function<void(qint64, qint64)> callback)
        : limits(caps), cancelled(std::move(flag)), progress(std::move(callback)) { clock.start(); progressClock.start(); }
    bool fail(const QString &text) { if (error.isEmpty()) error = text; return false; }
    bool proceed() {
        if (cancelled->load()) return fail(QStringLiteral("Archive operation cancelled."));
        if (clock.elapsed() > limits.processingMilliseconds)
            return fail(QStringLiteral("The archive exceeded its processing time limit."));
        return error.isEmpty();
    }
    void report(bool force = false) {
        if (force || progressClock.elapsed() >= 80) { progress(processed, total); progressClock.restart(); }
    }
    bool systemError(const QString &action) { return fail(action + QStringLiteral(": ") + QString::fromLocal8Bit(std::strerror(errno))); }
};
struct Result {
    std::shared_ptr<ArchiveTemporary> temporary;
    QString staged;
    QUrl destination;
    QHash<QString, int> directoryModes;
    QString error;
};

QThreadPool &pool()
{
    static QThreadPool workers;
    static const bool configured = [] { workers.setMaxThreadCount(2); workers.setExpiryTimeout(10000); return true; }();
    Q_UNUSED(configured);
    return workers;
}

bool localUrl(const QUrl &url)
{
    return url.isValid() && url.isLocalFile() && !url.hasQuery() && !url.hasFragment()
        && (url.host().isEmpty() || url.host() == QStringLiteral("localhost")) && !url.toLocalFile().contains(QChar::Null);
}

QString parentPath(const QUrl &url) { return QFileInfo(url.toLocalFile()).absolutePath(); }

bool safeName(const QByteArray &raw, bool directory, Work &work, QString *decoded = nullptr)
{
    if (raw.isEmpty() || raw.size() > pathBytesLimit || raw.contains('\0') || raw.contains('\\') || raw.contains(':') || raw.startsWith('/'))
        return work.fail(QStringLiteral("The ZIP contains an unsafe or nonportable path."));
    const QByteArray name = directory && raw.endsWith('/') ? raw.chopped(1) : raw;
    const auto parts = name.split('/');
    if (parts.size() > work.limits.depth) return work.fail(QStringLiteral("The archive exceeds the folder depth limit."));
    for (const auto &part : parts)
        if (part.isEmpty() || part == "." || part == ".." || part.size() > 255)
            return work.fail(QStringLiteral("The ZIP contains an unsafe or nonportable path."));
    const QString text = QFile::decodeName(name);
    if (QFile::encodeName(text) != name) return work.fail(QStringLiteral("The ZIP contains a filename that cannot be represented safely."));
    if (decoded) *decoded = text;
    return true;
}

// Walk from / using descriptor-relative O_NOFOLLOW at every component. A
// symlink swapped in an ancestor cannot redirect source reads or output checks.
int openPath(const QString &path, int flags, Work &work)
{
    if (!work.proceed()) return -1;
    const auto raw = QFile::encodeName(QDir::cleanPath(path));
    if (!raw.startsWith('/') || raw.contains('\0')) { work.fail(QStringLiteral("A local absolute path is required.")); return -1; }
    const auto parts = raw.split('/');
    int fd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    for (int i = 1; fd >= 0 && i < parts.size(); ++i) {
        if (parts[i].isEmpty()) continue;
        const int requested = i + 1 == parts.size() ? flags : O_RDONLY | O_DIRECTORY;
        int next = ::openat(fd, parts[i].constData(), requested | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        ::close(fd); fd = next;
    }
    if (fd < 0) work.systemError(QStringLiteral("Cannot open %1 without following links").arg(path));
    return fd;
}

bool sameFile(const struct stat &a, const struct stat &b)
{
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_mode == b.st_mode && a.st_size == b.st_size
        && a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec
        && a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}

class BoundedDevice : public QIODevice {
public:
    QFile file;
    Work &work;
    qint64 readBytes = 0;
    explicit BoundedDevice(const QString &path, OpenMode mode, Work &context) : file(path), work(context) {
        if (file.open(mode)) open(mode); else { setErrorString(file.errorString()); work.fail(file.errorString()); }
    }
    bool seek(qint64 pos) override { return work.proceed() && pos >= 0 && pos <= work.limits.archiveBytes && file.seek(pos) && QIODevice::seek(pos); }
    qint64 size() const override { return file.size(); }
protected:
    qint64 readData(char *data, qint64 length) override {
        if (!work.proceed()) return -1;
        if (length > work.limits.archiveBytes * 4 - readBytes) { work.fail(QStringLiteral("The ZIP exceeded its compressed read budget.")); return -1; }
        const auto count = file.read(data, length);
        if (count < 0) work.fail(file.errorString()); else readBytes += count;
        return count;
    }
    qint64 writeData(const char *data, qint64 length) override {
        if (!work.proceed()) return -1;
        if (length > work.limits.archiveBytes - file.pos()) { work.fail(QStringLiteral("The ZIP exceeds the archive size limit.")); return -1; }
        const auto count = file.write(data, length);
        if (count != length) work.fail(file.errorString());
        return count;
    }
};

quint16 u16(const QByteArray &data, int at) { return qFromLittleEndian<quint16>(data.constData() + at); }
quint32 u32(const QByteArray &data, int at) { return qFromLittleEndian<quint32>(data.constData() + at); }
quint64 u64(const QByteArray &data, int at) { return qFromLittleEndian<quint64>(data.constData() + at); }
bool readAt(QFile &file, qint64 at, qint64 count, QByteArray &out, Work &work)
{
    if (!work.proceed()) return false;
    if (at < 0 || count < 0 || at > file.size() || count > file.size() - at || !file.seek(at))
        return work.fail(QStringLiteral("The ZIP contains a truncated or inconsistent header."));
    out = file.read(count);
    return out.size() == count || work.fail(file.errorString());
}
struct ZipEntry {
    QString path;
    QByteArray raw;
    quint64 size = 0, compressed = 0, offset = 0;
    quint32 crc = 0;
    quint16 method = 0, flags = 0;
    mode_t mode = 0644;
    bool directory = false, zip64 = false;
    QDateTime modified;
};

// Validate before KZip's parser normalizes paths or allocates its tree. Local
// headers must match the central directory; no hidden records or overlapping
// payloads, links, encryption, multi-volume records or duplicate paths.
bool inspectZip(QFile &file, QList<ZipEntry> &entries, Work &work)
{
    QByteArray tail;
    const qint64 start = std::max<qint64>(0, file.size() - 65557);
    if (!readAt(file, start, file.size() - start, tail, work)) return false;
    int end = -1;
    for (int i = tail.size() - 22; i >= 0; --i)
        if (u32(tail, i) == 0x06054b50 && i + 22 + u16(tail, i + 20) == tail.size()) { end = i; break; }
    if (end < 0) return work.fail(QStringLiteral("This is not a complete ZIP archive."));
    if (u16(tail, end + 4) || u16(tail, end + 6) || u16(tail, end + 8) != u16(tail, end + 10))
        return work.fail(QStringLiteral("Multi-volume ZIP archives are not supported."));
    quint64 count = u16(tail, end + 10), length = u32(tail, end + 12), offset = u32(tail, end + 16);
    qint64 centralEnd = start + end;
    if (count == 0xffff || length == 0xffffffff || offset == 0xffffffff) {
        QByteArray locator, record;
        if (!readAt(file, centralEnd - 20, 20, locator, work) || u32(locator, 0) != 0x07064b50 || u32(locator, 4) || u32(locator, 16) != 1)
            return work.fail(QStringLiteral("The ZIP64 directory locator is invalid."));
        const quint64 recordAt = u64(locator, 8);
        if (recordAt > quint64(file.size()) || !readAt(file, qint64(recordAt), 56, record, work)
            || u32(record, 0) != 0x06064b50 || u64(record, 4) != 44 || u32(record, 16) || u32(record, 20)
            || u64(record, 24) != u64(record, 32) || recordAt + 56 != quint64(centralEnd - 20))
            return work.fail(QStringLiteral("The ZIP64 directory is invalid or unsupported."));
        count = u64(record, 32); length = u64(record, 40); offset = u64(record, 48); centralEnd = qint64(recordAt);
    }
    if (count > quint64(work.limits.entries)) return work.fail(QStringLiteral("The archive exceeds the entry limit."));
    if (offset > quint64(file.size()) || length > quint64(file.size()) - offset || offset + length != quint64(centralEnd))
        return work.fail(QStringLiteral("The ZIP directory size or offset is inconsistent."));
    QHash<QString, bool> nodes;
    QSet<QString> explicitPaths;
    quint64 cursor = offset, expanded = 0;
    for (quint64 i = 0; i < count; ++i) {
        QByteArray header, variable;
        if (!readAt(file, qint64(cursor), 46, header, work) || u32(header, 0) != 0x02014b50)
            return work.fail(QStringLiteral("The ZIP directory entry is invalid."));
        const int nameLength = u16(header, 28), extraLength = u16(header, 30), commentLength = u16(header, 32);
        const quint64 next = cursor + 46 + nameLength + extraLength + commentLength;
        if (next > offset + length || !readAt(file, qint64(cursor + 46), nameLength + extraLength, variable, work))
            return work.fail(QStringLiteral("The ZIP directory entry extends outside its directory."));
        ZipEntry entry;
        entry.raw = variable.first(nameLength); entry.flags = u16(header, 8); entry.method = u16(header, 10);
        entry.crc = u32(header, 16); entry.compressed = u32(header, 20); entry.size = u32(header, 24); entry.offset = u32(header, 42);
        entry.directory = entry.raw.endsWith('/');
        entry.mode = (quint8(header[5]) == 3) ? mode_t(u32(header, 38) >> 16) : mode_t(entry.directory ? 0755 : 0644);
        const mode_t type = entry.mode & S_IFMT;
        if ((type && type != S_IFREG && type != S_IFDIR) || (type == S_IFDIR && !entry.directory)
            || (entry.flags & ~(0x0800 | 0x0008 | 0x0006)) || (entry.method != 0 && entry.method != 8) || u16(header, 34))
            return work.fail(QStringLiteral("The ZIP contains links, special files, encryption or an unsupported compression method."));
        if ((entry.mode & (entry.directory ? 0500 : 0400)) != (entry.directory ? 0500 : 0400))
            return work.fail(QStringLiteral("The ZIP contains entries without owner read/traverse permissions; native publication cannot preserve these modes."));
        const auto time = u16(header, 12), date = u16(header, 14);
        entry.modified = QDateTime(QDate(1980 + (date >> 9), (date >> 5) & 15, date & 31),
            QTime(time >> 11, (time >> 5) & 63, (time & 31) * 2));
        entry.zip64 = entry.size == 0xffffffff || entry.compressed == 0xffffffff || entry.offset == 0xffffffff;
        if (entry.zip64) {
            bool found = false;
            for (int at = nameLength; at + 4 <= variable.size();) {
                const int size = u16(variable, at + 2), stop = at + 4 + size;
                if (stop > variable.size()) return work.fail(QStringLiteral("The ZIP extra field is truncated."));
                if (u16(variable, at) == 1) {
                    int valueAt = at + 4;
                    for (quint64 *value : {&entry.size, &entry.compressed, &entry.offset}) {
                        if (*value != 0xffffffff) continue;
                        if (valueAt + 8 > stop) return work.fail(QStringLiteral("The ZIP64 entry is truncated."));
                        *value = u64(variable, valueAt); valueAt += 8;
                    }
                    found = true; break;
                }
                at = stop;
            }
            if (!found) return work.fail(QStringLiteral("The ZIP64 entry lacks its sizes or offset."));
        }
        if (!safeName(entry.raw, entry.directory, work, &entry.path)) return false;
        if (explicitPaths.contains(entry.path)) return work.fail(QStringLiteral("The ZIP contains duplicate paths."));
        explicitPaths.insert(entry.path);
        QString node;
        const auto components = entry.path.split('/');
        for (int part = 0; part < components.size(); ++part) {
            if (!node.isEmpty()) node += '/';
            node += components[part];
            const bool directory = part + 1 < components.size() || entry.directory;
            if (nodes.contains(node) && (!nodes[node] || !directory)) return work.fail(QStringLiteral("The ZIP uses one path as both a file and folder."));
            nodes.insert(node, directory);
            if (nodes.size() > work.limits.entries) return work.fail(QStringLiteral("The archive exceeds the entry limit, including folders."));
        }
        if (entry.size > quint64(work.limits.expandedBytes) - expanded || entry.compressed > quint64(file.size())
            || (entry.directory && (entry.size || entry.crc))) return work.fail(QStringLiteral("The ZIP exceeds its decompressed size limit or has invalid folder data."));
        expanded += entry.size;
        entries.append(entry); cursor = next;
    }
    if (cursor != offset + length) return work.fail(QStringLiteral("The ZIP entry count does not match its directory."));
    std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) { return a.offset < b.offset; });
    quint64 expected = 0;
    for (auto &entry : entries) {
        QByteArray local, name;
        if (entry.offset != expected || !readAt(file, qint64(entry.offset), 30, local, work) || u32(local, 0) != 0x04034b50
            || u16(local, 6) != entry.flags || u16(local, 8) != entry.method
            || !readAt(file, qint64(entry.offset + 30), u16(local, 26), name, work) || name != entry.raw)
            return work.fail(QStringLiteral("The ZIP local headers and directory do not match."));
        QByteArray extras;
        if (!readAt(file, qint64(entry.offset + 30 + name.size()), u16(local, 28), extras, work)) return false;
        for (int at = 0; at < extras.size();) {
            if (at + 4 > extras.size()) return work.fail(QStringLiteral("The ZIP local extra field is truncated."));
            const int stop = at + 4 + u16(extras, at + 2);
            if (stop > extras.size()) return work.fail(QStringLiteral("The ZIP local extra field is truncated."));
            if (u16(extras, at) == 0x5455 && stop - at >= 9 && (quint8(extras[at + 4]) & 1)) {
                // Signed 32-bit Unix timestamps, as used by Info-ZIP/KZip.
                // Applied explicitly: KZip may have made an implicit folder
                // before encountering its later explicit metadata record.
                entry.modified = QDateTime::fromSecsSinceEpoch(qint32(u32(extras, at + 5)), QTimeZone::UTC);
            }
            at = stop;
        }
        expected = entry.offset + 30 + u16(local, 26) + u16(local, 28);
        if (expected > offset || entry.compressed > offset - expected) return work.fail(QStringLiteral("The ZIP has overlapping or out-of-range data."));
        expected += entry.compressed;
        if (!(entry.flags & 8)) {
            quint64 size = u32(local, 22), compressed = u32(local, 18);
            if (size == 0xffffffff || compressed == 0xffffffff) {
                QByteArray extra;
                if (!readAt(file, qint64(entry.offset + 30 + name.size()), u16(local, 28), extra, work)) return false;
                bool found = false;
                for (int at = 0; at + 4 <= extra.size();) {
                    const int stop = at + 4 + u16(extra, at + 2);
                    if (stop > extra.size()) return work.fail(QStringLiteral("The ZIP local extra field is truncated."));
                    if (u16(extra, at) == 1) {
                        int valueAt = at + 4;
                        for (quint64 *value : {&size, &compressed}) {
                            if (*value != 0xffffffff) continue;
                            if (valueAt + 8 > stop) return work.fail(QStringLiteral("The ZIP64 local entry is truncated."));
                            *value = u64(extra, valueAt); valueAt += 8;
                        }
                        found = true; break;
                    }
                    at = stop;
                }
                if (!found) return work.fail(QStringLiteral("The ZIP64 local entry lacks its sizes."));
            }
            if (size != entry.size || compressed != entry.compressed || u32(local, 14) != entry.crc)
                return work.fail(QStringLiteral("The ZIP sizes or checksum headers do not match."));
        } else {
            QByteArray descriptor;
            if (!readAt(file, qint64(expected), 4, descriptor, work)) return false;
            if (u32(descriptor, 0) == 0x08074b50) expected += 4;
            const bool wide = entry.zip64 || u16(local, 4) >= 45;
            const int bytes = wide ? 20 : 12;
            if (!readAt(file, qint64(expected), bytes, descriptor, work) || u32(descriptor, 0) != entry.crc
                || (wide ? u64(descriptor, 4) : u32(descriptor, 4)) != entry.compressed
                || (wide ? u64(descriptor, 12) : u32(descriptor, 8)) != entry.size)
                return work.fail(QStringLiteral("The ZIP data descriptor is inconsistent."));
            expected += bytes;
        }
    }
    if (expected != offset) return work.fail(QStringLiteral("The ZIP contains unlisted local records or unexpected data."));
    work.total = qint64(expanded); work.report(true);
    return true;
}

quint32 checksum(quint32 crc, const char *data, qsizetype size)
{
    static const auto table = [] {
        std::array<quint32, 256> values{};
        for (quint32 i = 0; i < values.size(); ++i) { quint32 c = i; for (int j = 0; j < 8; ++j) c = (c >> 1) ^ ((c & 1) ? 0xedb88320U : 0); values[i] = c; }
        return values;
    }();
    for (qsizetype i = 0; i < size; ++i) crc = table[(crc ^ quint8(data[i])) & 255] ^ (crc >> 8);
    return crc;
}

struct SourceEntry { QString path, name; struct stat metadata {}; };
bool collect(const QString &path, const QString &name, QList<SourceEntry> &entries, Work &work)
{
    if (!work.proceed() || !safeName(QFile::encodeName(name), false, work)) return false;
    Fd fd(openPath(path, O_RDONLY, work));
    if (fd.value < 0) return false;
    struct stat metadata {};
    if (::fstat(fd.value, &metadata)) return work.systemError(QStringLiteral("Cannot inspect %1").arg(path));
    if (!S_ISREG(metadata.st_mode) && !S_ISDIR(metadata.st_mode)) return work.fail(QStringLiteral("Links and special files cannot be compressed."));
    if ((metadata.st_mode & (S_ISDIR(metadata.st_mode) ? 0500 : 0400)) != (S_ISDIR(metadata.st_mode) ? 0500 : 0400))
        return work.fail(QStringLiteral("Selected items need owner read/traverse permissions for portable archive publication."));
    if (entries.size() >= work.limits.entries) return work.fail(QStringLiteral("The selection exceeds the archive entry limit."));
    if (S_ISREG(metadata.st_mode)) {
        if (metadata.st_size < 0 || metadata.st_size > work.limits.expandedBytes - work.total) return work.fail(QStringLiteral("The selection exceeds the decompressed size limit."));
        work.total += metadata.st_size;
    }
    entries.append({path, name, metadata});
    if (S_ISDIR(metadata.st_mode)) {
        DIR *directory = ::fdopendir(::dup(fd.value));
        if (!directory) return work.systemError(QStringLiteral("Cannot list %1").arg(path));
        QStringList children;
        bool good = true;
        while (good && work.proceed()) {
            errno = 0;
            const auto *entry = ::readdir(directory);
            if (!entry) { if (errno) good = work.systemError(QStringLiteral("Cannot list %1").arg(path)); break; }
            const QByteArray raw(entry->d_name);
            if (raw == "." || raw == "..") continue;
            QString child;
            if (!safeName(raw, false, work, &child)) { good = false; break; }
            children.append(child);
            if (children.size() + entries.size() > work.limits.entries) { good = work.fail(QStringLiteral("The selection exceeds the archive entry limit.")); break; }
        }
        ::closedir(directory);
        if (!good || !work.proceed()) return false;
        children.sort();
        for (const auto &child : children) if (!collect(path + '/' + child, name + '/' + child, entries, work)) return false;
    }
    return true;
}

bool zipSources(const QList<QUrl> &sources, const QString &staged, Work &work)
{
    QList<SourceEntry> entries;
    QSet<QString> names;
    for (const auto &source : sources) {
        const QString path = QDir::cleanPath(source.toLocalFile()), name = QFileInfo(path).fileName();
        if (names.contains(name)) return work.fail(QStringLiteral("Select distinct items from one folder."));
        names.insert(name);
        if (!collect(path, name, entries, work)) return false;
    }
    work.report(true);
    BoundedDevice output(staged, QIODevice::ReadWrite | QIODevice::Truncate, work);
    KZip zip(&output);
    zip.setExtraField(KZip::ModificationTime);
    if (!output.isOpen() || !zip.open(QIODevice::WriteOnly)) return work.fail(zip.errorString());
    std::array<char, chunkSize> buffer{};
    for (const auto &entry : entries) {
        if (!work.proceed()) return false;
        Fd input(openPath(entry.path, O_RDONLY, work));
        if (input.value < 0) return false;
        struct stat now {};
        if (::fstat(input.value, &now)) return work.systemError(QStringLiteral("Cannot inspect %1").arg(entry.path));
        if (!sameFile(entry.metadata, now)) return work.fail(QStringLiteral("A selected item changed while the ZIP was being prepared."));
        const auto mtime = QDateTime::fromSecsSinceEpoch(now.st_mtim.tv_sec, QTimeZone::UTC);
        const auto atime = QDateTime::fromSecsSinceEpoch(now.st_atim.tv_sec, QTimeZone::UTC);
        const mode_t permissions = now.st_mode & (S_IFMT | 0777);
        if (S_ISDIR(now.st_mode)) {
            if (!zip.writeDir(entry.name, {}, {}, permissions, atime, mtime, {})) return work.fail(zip.errorString());
            continue;
        }
        if (!zip.prepareWriting(entry.name, {}, {}, now.st_size, permissions, atime, mtime, {})) return work.fail(zip.errorString());
        qint64 read = 0;
        while (read < now.st_size) {
            if (!work.proceed()) return false;
            const auto count = ::read(input.value, buffer.data(), std::min<qint64>(chunkSize, now.st_size - read));
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) return work.systemError(QStringLiteral("Cannot read %1").arg(entry.path));
            if (!count) return work.fail(QStringLiteral("A selected file was truncated during compression."));
            if (!zip.writeData(buffer.data(), count)) return work.fail(zip.errorString());
            read += count; work.processed += count; work.report();
        }
        if (::fstat(input.value, &now) || !sameFile(entry.metadata, now)) return work.fail(QStringLiteral("A selected file changed during compression."));
        if (!zip.finishWriting(read)) return work.fail(zip.errorString());
    }
    if (!zip.close() || !output.file.flush()) return work.fail(output.file.error() == QFileDevice::NoError ? zip.errorString() : output.file.errorString());
    // Check directory changes too, so a disappearing/new item is never silently
    // treated as a complete snapshot. Sources are never removed or rewritten.
    for (const auto &entry : entries) {
        Fd input(openPath(entry.path, O_RDONLY, work));
        struct stat now {};
        if (input.value < 0 || ::fstat(input.value, &now) || !sameFile(entry.metadata, now)) return work.fail(QStringLiteral("The selection changed during compression."));
    }
    work.report(true);
    return work.proceed();
}

bool snapshot(const QUrl &source, const QString &target, Work &work)
{
    Fd input(openPath(source.toLocalFile(), O_RDONLY, work));
    if (input.value < 0) return false;
    struct stat before {}, after {};
    if (::fstat(input.value, &before)) return work.systemError(QStringLiteral("Cannot inspect the ZIP"));
    if (!S_ISREG(before.st_mode) || before.st_size < 22 || before.st_size > work.limits.archiveBytes)
        return work.fail(QStringLiteral("Select a regular ZIP within the archive size limit."));
    QFile output(target);
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return work.fail(output.errorString());
    std::array<char, chunkSize> buffer{};
    qint64 copied = 0;
    while (copied < before.st_size) {
        if (!work.proceed()) return false;
        const auto count = ::read(input.value, buffer.data(), std::min<qint64>(chunkSize, before.st_size - copied));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return work.systemError(QStringLiteral("Cannot read the ZIP"));
        if (!count) return work.fail(QStringLiteral("The ZIP was truncated while being read."));
        if (output.write(buffer.data(), count) != count) return work.fail(output.errorString());
        copied += count;
    }
    if (::fstat(input.value, &after) || !sameFile(before, after)) return work.fail(QStringLiteral("The ZIP changed while being read."));
    return output.flush() || work.fail(output.errorString());
}

bool metadata(const QString &path, mode_t mode, const QDateTime &mtime, bool directory, Work &work)
{
    Fd file(::open(QFile::encodeName(path).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | (directory ? O_DIRECTORY : 0)));
    if (file.value < 0) return work.systemError(QStringLiteral("Cannot preserve archive metadata"));
    if (mtime.isValid()) {
        struct timespec times[2] {{0, UTIME_OMIT}, {time_t(mtime.toSecsSinceEpoch()), 0}};
        if (::futimens(file.value, times)) return work.systemError(QStringLiteral("Cannot preserve archive modification time"));
    }
    if (::fchmod(file.value, mode & 0777)) return work.systemError(QStringLiteral("Cannot preserve archive permissions"));
    return true;
}

bool unzip(const QString &input, const QString &root, QList<ZipEntry> &entries, ArchiveTemporary &temporary, Work &work)
{
    QFile headers(input);
    if (!headers.open(QIODevice::ReadOnly)) return work.fail(headers.errorString());
    if (!inspectZip(headers, entries, work)) return false;
    BoundedDevice device(input, QIODevice::ReadOnly, work);
    KZip zip(&device);
    if (!zip.open(QIODevice::ReadOnly)) return work.fail(zip.errorString());
    if (!QDir().mkdir(root)) return work.fail(QStringLiteral("Cannot create extraction staging."));
    std::array<char, chunkSize> buffer{};
    for (const auto &entry : entries) {
        if (!work.proceed()) return false;
        const QString path = root + '/' + entry.path;
        const auto *native = zip.directory()->entry(entry.path);
        if (!native || !native->symLinkTarget().isEmpty() || native->isDirectory() != entry.directory)
            return work.fail(QStringLiteral("The ZIP parser and validated entry disagree."));
        if (!QDir().mkpath(entry.directory ? path : QFileInfo(path).absolutePath())) return work.fail(QStringLiteral("Cannot create an extracted folder."));
        if (entry.directory) continue;
        const auto *file = dynamic_cast<const KZipFileEntry *>(native);
        if (!file || file->size() != qint64(entry.size) || file->compressedSize() != qint64(entry.compressed) || file->crc32() != entry.crc)
            return work.fail(QStringLiteral("The ZIP parser and validated sizes disagree."));
        std::unique_ptr<QIODevice> stream(file->createDevice());
        if (!stream || !stream->isOpen()) return work.fail(QStringLiteral("The archive data cannot be decompressed."));
        QFile output(path);
        if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return work.fail(output.errorString());
        quint64 read = 0;
        quint32 crc = 0xffffffff;
        for (;;) {
            if (!work.proceed()) return false;
            const auto count = stream->read(buffer.data(), chunkSize);
            if (count < 0) return work.fail(stream->errorString());
            if (!count) break;
            if (quint64(count) > entry.size - read || count > work.limits.expandedBytes - work.processed)
                return work.fail(QStringLiteral("The decompressed ZIP exceeds its validated size limit."));
            if (output.write(buffer.data(), count) != count) return work.fail(output.errorString());
            crc = checksum(crc, buffer.data(), count); read += count; work.processed += count; work.report();
        }
        if (read != entry.size || (crc ^ 0xffffffff) != entry.crc) return work.fail(QStringLiteral("The ZIP data is damaged (size or checksum mismatch)."));
        if (!output.flush()) return work.fail(output.errorString());
        output.close();
        if (!metadata(path, entry.mode, entry.modified, false, work)) return false;
    }
    // Register every implicit/explicit directory for cleanup before applying
    // read-only modes. Ancestors are restored before descendants on teardown.
    QSet<QString> directories;
    directories.insert(root);
    for (const auto &entry : entries) {
        QString directory = entry.directory ? root + '/' + entry.path : QFileInfo(root + '/' + entry.path).absolutePath();
        while (directory != root) { directories.insert(directory); directory = QFileInfo(directory).absolutePath(); }
    }
    temporary.directories = directories.values();
    std::sort(temporary.directories.begin(), temporary.directories.end(), [](const auto &a, const auto &b) { return a.size() < b.size(); });
    // Deepest explicit directory first, regardless of ZIP record order.
    std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) { return a.path.size() < b.path.size(); });
    // Restore explicit directory timestamps after all children are written.
    for (auto it = entries.crbegin(); it != entries.crend(); ++it)
        if (it->directory && (!work.proceed() || !metadata(root + '/' + it->path, it->mode, it->modified, true, work))) return false;
    work.report(true);
    return work.proceed();
}

Result perform(const QList<QUrl> &sources, const QUrl &directory, bool extraction, Work &work)
{
    Result result;
    const QString parent = directory.toLocalFile();
    Fd destination(openPath(parent, O_RDONLY | O_DIRECTORY, work));
    if (destination.value < 0) { result.error = work.error; return result; }
    result.temporary = std::make_shared<ArchiveTemporary>(QDir::tempPath() + QStringLiteral("/finder-archive-XXXXXX"));
    if (!result.temporary->isValid()) { result.error = result.temporary->errorString(); return result; }
    QString name;
    if (extraction) {
        const QString input = result.temporary->filePath(QStringLiteral("input.zip"));
        const QString root = result.temporary->filePath(QStringLiteral("contents"));
        QList<ZipEntry> entries;
        if (!snapshot(sources.first(), input, work) || !unzip(input, root, entries, *result.temporary, work)) { result.error = work.error; return result; }
        for (const auto &directory : result.temporary->directories) {
            struct stat mode {};
            if (::lstat(QFile::encodeName(directory).constData(), &mode)) { work.systemError(QStringLiteral("Cannot inspect extracted permissions")); result.error = work.error; return result; }
            result.directoryModes.insert(directory, mode.st_mode & 0777);
        }
        const auto top = QDir(root).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
        if (top.size() == 1) { name = top.first(); result.staged = root + '/' + name; }
        else {
            name = QFileInfo(sources.first().toLocalFile()).fileName(); name.chop(4);
            if (name.isEmpty()) name = QStringLiteral("Archive");
            result.staged = root;
        }
    } else {
        name = Archives::suggestedArchiveName(sources);
        result.staged = result.temporary->filePath(QStringLiteral("output.zip"));
        if (!zipSources(sources, result.staged, work)) { result.error = work.error; return result; }
    }
    if (!safeName(QFile::encodeName(name), false, work)) { result.error = work.error; return result; }
    // Select a free candidate without any filesystem mutation. KIO still
    // auto-renames atomically on a later race; no Overwrite flag is ever used.
    QString candidate = name;
    for (int suffix = 2; suffix <= work.limits.entries + 2; ++suffix) {
        if (!work.proceed()) { result.error = work.error; return result; }
        struct stat existing {};
        if (::fstatat(destination.value, QFile::encodeName(candidate).constData(), &existing, AT_SYMLINK_NOFOLLOW)) {
            if (errno == ENOENT) { result.destination = QUrl::fromLocalFile(parent + '/' + candidate); return result; }
            work.systemError(QStringLiteral("Cannot inspect the output name")); result.error = work.error; return result;
        }
        candidate = name.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive)
            ? name.chopped(4) + QStringLiteral(" %1.zip").arg(suffix) : name + QStringLiteral(" %1").arg(suffix);
    }
    result.error = QStringLiteral("No free archive output name exists within the collision limit.");
    return result;
}
}

Archives::Archives(QWidget *parent, const Limits &limits) : QObject(parent), m_parent(parent), m_state(std::make_shared<ArchivesState>())
{
    const Limits hard;
    m_state->limits = {std::clamp(limits.entries, 1, hard.entries), std::clamp<qint64>(limits.archiveBytes, 22, hard.archiveBytes),
        std::clamp<qint64>(limits.expandedBytes, 1, hard.expandedBytes), std::clamp(limits.depth, 1, hard.depth),
        std::clamp(limits.processingMilliseconds, 1, hard.processingMilliseconds)};
}
Archives::~Archives()
{
    if (m_state->cancelled) m_state->cancelled->store(true);
    if (m_state->job) { disconnect(m_state->job, nullptr, this, nullptr); m_state->job->setUiDelegate(nullptr); m_state->job->kill(KJob::EmitResult); }
}
bool Archives::busy() const { return m_state->busy; }
QString Archives::suggestedArchiveName(const QList<QUrl> &sources)
{
    if (sources.isEmpty()) return {};
    return sources.size() == 1 ? QFileInfo(sources.first().toLocalFile()).fileName() + QStringLiteral(".zip") : QStringLiteral("Archive.zip");
}
FinderArchives::Capability Archives::canCompress(const QList<QUrl> &sources, const QUrl &directory)
{
    if (sources.isEmpty() || sources.size() > Limits{}.entries) return {false, tr("Select 1–10,000 local files or folders.")};
    const QString parent = parentPath(sources.first());
    QSet<QString> selected;
    for (const auto &source : sources) {
        if (!localUrl(source)) return {false, tr("Compression supports local files and folders; this protocol is unavailable.")};
        if (parentPath(source) != parent || QFileInfo(source.toLocalFile()).fileName().isEmpty() || selected.contains(source.toLocalFile()))
            return {false, tr("Select distinct items in one folder.")};
        selected.insert(source.toLocalFile());
    }
    if (!directory.isEmpty() && !localUrl(directory)) return {false, tr("Archive output requires a local directory; this protocol is unavailable.")};
    return {true, tr("Create a portable ZIP beside the selection; existing outputs receive a new name. Links and resource limits are checked asynchronously.")};
}
FinderArchives::Capability Archives::canExtract(const QUrl &archive)
{
    if (!localUrl(archive)) return {false, tr("Extraction supports local ZIP files; this protocol is unavailable.")};
    if (!archive.path().endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive)) return {false, tr("Select a ZIP archive.")};
    return {true, tr("Expand beside the ZIP with native conflict handling and Undo. Links, unsafe paths, encrypted archives and resource limits are checked before publication.")};
}
void Archives::compress(const QList<QUrl> &sources, const QUrl &directory)
{
    if (busy()) { emit errorOccurred(tr("An archive operation is already running.")); return; }
    const auto capability = canCompress(sources, directory);
    if (!capability.enabled) { emit errorOccurred(capability.explanation); return; }
    start(sources, directory.isEmpty() ? QUrl::fromLocalFile(parentPath(sources.first())) : directory, false);
}
void Archives::extract(const QUrl &archive)
{
    if (busy()) { emit errorOccurred(tr("An archive operation is already running.")); return; }
    const auto capability = canExtract(archive);
    if (!capability.enabled) { emit errorOccurred(capability.explanation); return; }
    start({archive}, QUrl::fromLocalFile(parentPath(archive)), true);
}
void Archives::start(const QList<QUrl> &sources, const QUrl &directory, bool extraction)
{
    m_state->cancelled = std::make_shared<std::atomic_bool>(false);
    const auto flag = m_state->cancelled;
    const auto generation = ++m_state->generation;
    const auto limits = m_state->limits;
    m_state->outputs.clear(); m_state->publishedDirectories.clear(); m_state->directoryModes.clear(); m_state->busy = true;
    auto task = std::shared_ptr<ArchiveTask>(new ArchiveTask, [](ArchiveTask *object) { object->deleteLater(); });
    auto result = std::make_shared<Result>();
    connect(task.get(), &ArchiveTask::progress, this, [this, generation](qint64 done, qint64 total) {
        if (busy() && generation == m_state->generation) emit progressChanged(done, total);
    }, Qt::QueuedConnection);
    connect(task.get(), &ArchiveTask::done, this, [this, generation, result, task] {
        if (generation != m_state->generation) return;
        if (m_state->cancelled->load()) { complete(true); return; }
        if (!result->error.isEmpty()) { complete(false, result->error); return; }
        m_state->temporary = result->temporary; m_state->staged = result->staged; m_state->destination = result->destination;
        m_state->directoryModes = result->directoryModes;
        publish();
    }, Qt::ConnectionType(Qt::QueuedConnection | Qt::SingleShotConnection));
    emit busyChanged(true);
    emit statusChanged(extraction ? tr("Validating and expanding ZIP…") : tr("Preparing ZIP…"));
    emit progressChanged(0, 0);
    pool().start(QRunnable::create([sources, directory, extraction, flag, limits, result, task] {
        Work work(limits, flag, [task](qint64 done, qint64 total) { emit task->progress(done, total); });
        *result = perform(sources, directory, extraction, work);
        if (result->error.isEmpty() && !work.error.isEmpty()) result->error = work.error;
        emit task->done();
    }));
}
void Archives::publish()
{
    if (m_state->cancelled->load()) { complete(true); return; }
    emit statusChanged(tr("Saving archive output…"));
    if (m_state->cancelled->load()) { complete(true); return; }
    // copyAs preserves Undo as a creation; moveAs would make Undo refer to
    // ephemeral staging. KIO owns atomic per-file writes, metadata, conflicts
    // and partial-copy Undo. One top-level item gives one logical Undo record.
    auto *job = KIO::copyAs(QUrl::fromLocalFile(m_state->staged), m_state->destination);
    m_state->job = job;
    job->setAutoRename(true);
    job->setUiDelegate(KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, m_parent));
    KIO::FileUndoManager::self()->recordCopyJob(job);
    const auto from = QUrl::fromLocalFile(m_state->staged);
    auto actual = std::make_shared<QUrl>(m_state->destination);
    auto created = std::make_shared<bool>(false);
    connect(job, &KIO::CopyJob::renamed, this, [actual](KIO::Job *, const QUrl &old, const QUrl &to) { if (*actual == old) *actual = to; });
    connect(job, &KIO::CopyJob::creatingDir, this, [actual, created](KIO::Job *, const QUrl &to) { if (to == *actual) *created = true; });
    connect(job, &KIO::CopyJob::copyingDone, this, [this, from, actual, created](KIO::Job *, const QUrl &source, const QUrl &to, const QDateTime &, bool directory, bool) {
        if (directory && m_state->directoryModes.contains(source.toLocalFile()))
            m_state->publishedDirectories.append({to, m_state->directoryModes.value(source.toLocalFile())});
        if (source == from) { *actual = to; *created = true; }
    });
    connect(job, &KJob::percentChanged, this, [this](KJob *, unsigned long percent) { emit publicationProgressChanged(int(percent)); });
    connect(job, &KJob::result, job, [temporary = m_state->temporary] { Q_UNUSED(temporary); });
    connect(job, &KJob::result, this, [this, actual, created](KJob *finished) {
        m_state->job = nullptr;
        if (*created) { m_state->outputs.append(*actual); emit outputCreated(*actual); }
        if (m_state->cancelled->load() || finished->error() == KJob::KilledJobError) { complete(true); return; }
        if (finished->error()) { complete(false, finished->errorText()); return; }
        std::sort(m_state->publishedDirectories.begin(), m_state->publishedDirectories.end(),
            [](const auto &a, const auto &b) { return a.first.path().size() > b.first.path().size(); });
        preserveNextDirectory();
    });
}
void Archives::preserveNextDirectory()
{
    if (m_state->cancelled->load()) { complete(true); return; }
    if (m_state->publishedDirectories.isEmpty()) { complete(false); return; }
    // KIO 6.24 CopyJob deliberately creates directories with writable defaults
    // and restores mtime, but has a TODO for restoring directory permissions.
    // Use its public native chmod job on directories we just created, deepest
    // first. Undo still belongs to the single creation CopyJob, not staging.
    const auto directory = m_state->publishedDirectories.takeFirst();
    auto *job = KIO::chmod(directory.first, directory.second);
    m_state->job = job;
    job->setUiDelegate(KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, m_parent));
    connect(job, &KJob::result, this, [this](KJob *finished) {
        m_state->job = nullptr;
        if (m_state->cancelled->load() || finished->error() == KJob::KilledJobError) { complete(true); return; }
        if (finished->error()) { complete(false, finished->errorText()); return; }
        preserveNextDirectory();
    });
}
void Archives::cancel()
{
    if (!busy()) return;
    m_state->cancelled->store(true);
    emit statusChanged(tr("Cancelling archive operation…"));
    if (m_state->job) m_state->job->kill(KJob::EmitResult);
}
void Archives::complete(bool cancelled, const QString &error)
{
    if (!busy()) return;
    const auto outputs = m_state->outputs;
    m_state->temporary.reset(); m_state->staged.clear(); m_state->job = nullptr; m_state->busy = false;
    emit busyChanged(false);
    if (!error.isEmpty()) emit errorOccurred(error);
    emit statusChanged(cancelled ? tr("Archive operation cancelled. %1 output items retained.").arg(outputs.size())
        : !error.isEmpty() ? error : tr("Archive operation complete."));
    if (!outputs.isEmpty()) emit outputsCreated(outputs);
    emit finished(cancelled);
}

#include "archives.moc"
