// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#include "tags.h"

#include <KFileMetaData/UserMetaData>
#include <QDir>
#include <QFile>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using Metadata = KFileMetaData::UserMetaData;
struct Mount { QByteArray path; bool local; };
struct Handle {
    int descriptor = -1;
    ~Handle() { if (descriptor >= 0) close(descriptor); }
    QString path() const { return QStringLiteral("/proc/self/fd/%1").arg(descriptor); }
};

bool readMounts(QList<Mount> &mounts) {
    QFile file(QStringLiteral("/proc/self/mountinfo"));
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QList<QByteArray> localTypes{"ext2", "ext3", "ext4", "btrfs", "xfs", "f2fs", "zfs",
        "tmpfs", "ramfs", "overlay", "ubifs", "ufs", "jfs", "reiserfs", "ntfs", "ntfs3",
        "vfat", "exfat", "iso9660", "udf", "squashfs", "fuseblk", "bcachefs"};
    for (const auto &line : file.read(4 * 1024 * 1024).split('\n')) {
        const int separator = line.indexOf(" - ");
        if (separator < 0) continue;
        const auto fields = line.left(separator).split(' ');
        const auto trailing = line.mid(separator + 3).split(' ');
        if (fields.size() < 6 || trailing.isEmpty()) continue;
        QByteArray path = fields[4];
        path.replace("\\040", " "); path.replace("\\011", "\t");
        path.replace("\\012", "\n"); path.replace("\\134", "\\");
        mounts.append({path, localTypes.contains(trailing[0])});
    }
    std::stable_sort(mounts.begin(), mounts.end(), [](const Mount &a, const Mount &b) {
        return a.path.size() > b.path.size();
    });
    return std::any_of(mounts.cbegin(), mounts.cend(), [](const Mount &mount) { return mount.path == "/"; });
}

bool isLocal(const QByteArray &path, const QList<Mount> &mounts) {
    for (const auto &mount : mounts)
        if (path == mount.path || mount.path == "/" || path.startsWith(mount.path + '/')) return mount.local;
    return false;
}

QString systemError(int code) {
    if (code == ENOENT) return QStringLiteral("The selected file no longer exists.");
    if (code == ELOOP || code == ENOTDIR) return QStringLiteral("Tags are unavailable on symbolic links or paths through symbolic links.");
    if (code == EACCES || code == EPERM) return QStringLiteral("Permission is required to read or change this file's tags.");
    if (code == ENOTSUP) return QStringLiteral("This filesystem does not support file tags.");
    return QStringLiteral("Could not access file tags: %1").arg(QString::fromLocal8Bit(std::strerror(code)));
}

QString openFile(const QUrl &url, Handle &handle) {
    if (!url.isLocalFile() || (!url.host().isEmpty() && url.host() != "localhost"))
        return QStringLiteral("Tags are available for local files; this remote location is unsupported.");
    const auto path = QDir::cleanPath(url.toLocalFile());
    if (!QDir::isAbsolutePath(path)) return QStringLiteral("Choose an absolute local file path for tags.");
    QList<Mount> mounts;
    if (!readMounts(mounts)) return QStringLiteral("Cannot verify the local filesystem for file tags.");
    if (!isLocal("/", mounts)) return QStringLiteral("Tags are unavailable on network or virtual mounts.");
    handle.descriptor = open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (handle.descriptor < 0) return systemError(errno);
    const auto encoded = QFile::encodeName(path);
    auto parts = encoded.split('/');
    parts.removeAll(QByteArray());
    QByteArray current;
    for (int index = 0; index < parts.size(); ++index) {
        current += '/' + parts[index];
        if (!isLocal(current, mounts)) return QStringLiteral("Tags are unavailable on network or virtual mounts.");
        const bool last = index == parts.size() - 1;
        // O_PATH pins the selected inode without opening file contents or a
        // device. Metadata then uses /proc/self/fd/N through the public KF API.
        const int descriptor = openat(handle.descriptor, parts[index].constData(),
            O_NOFOLLOW | O_CLOEXEC | (last ? O_PATH : O_RDONLY | O_DIRECTORY));
        if (descriptor < 0) return systemError(errno);
        close(handle.descriptor);
        handle.descriptor = descriptor;
    }
    // O_PATH|O_NOFOLLOW can open a symlink itself; do not let metadata calls
    // follow that descriptor to its target. Only regular files/directories.
    struct stat status;
    if (fstat(handle.descriptor, &status) != 0) return systemError(errno);
    if (S_ISLNK(status.st_mode)) return systemError(ELOOP);
    if (!S_ISREG(status.st_mode) && !S_ISDIR(status.st_mode))
        return QStringLiteral("Tags are available on regular files and folders.");
    return {};
}

QString normalize(const QStringList &requested, QStringList &result) {
    for (const auto &value : requested) {
        const auto tag = value.trimmed();
        if (tag.isEmpty()) continue;
        if (tag.size() > 256 || tag.contains(',') || std::any_of(tag.cbegin(), tag.cend(), [](QChar character) {
            return character.category() == QChar::Other_Control;
        })) return QStringLiteral("A tag must contain at most 256 characters and cannot contain a comma or control character.");
        if (!result.contains(tag)) result.append(tag);
    }
    if (result.join(',').toUtf8().size() > 65536)
        return QStringLiteral("The tags exceed this filesystem's metadata size limit.");
    return {};
}

QString metadataError(Metadata::Error error) {
    switch (error) {
    case Metadata::NoError: return {};
    case Metadata::NotSupported: return QStringLiteral("This filesystem does not support file tags.");
    case Metadata::NoSpace: return QStringLiteral("There is not enough space to save file tags.");
    case Metadata::MissingPermission: return QStringLiteral("Permission is required to change this file's tags.");
    case Metadata::ValueTooBig: return QStringLiteral("The tags exceed this filesystem's metadata size limit.");
    case Metadata::NameToolong: return QStringLiteral("This filesystem rejected the tag metadata name or size.");
    default: return QStringLiteral("The filesystem could not save the file tags.");
    }
}

QStringList readTags(const Handle &handle, QString &error) {
    Metadata metadata(handle.path());
    errno = 0;
    const auto tags = metadata.tags();
    const int code = errno;
    if (code != 0 && code != ENODATA) error = systemError(code);
    return tags;
}

QString writeTags(const Handle &handle, const QStringList &requested) {
    QStringList normalized;
    auto failure = normalize(requested, normalized);
    if (!failure.isEmpty()) return failure;
    Metadata metadata(handle.path());
    return metadataError(metadata.setTags(normalized));
}
}

QStringList FileTags::tags(const QUrl &url, QString *error) {
    Handle handle;
    auto failure = openFile(url, handle);
    QStringList result;
    if (failure.isEmpty()) result = readTags(handle, failure);
    if (error) *error = failure;
    return result;
}

QString FileTags::setTags(const QUrl &url, const QStringList &requested) {
    QStringList normalized;
    auto failure = normalize(requested, normalized);
    if (!failure.isEmpty()) return failure;
    Handle handle;
    failure = openFile(url, handle);
    if (!failure.isEmpty()) return failure;
    return writeTags(handle, normalized);
}

QString FileTags::addTag(const QUrl &url, const QString &tag) {
    Handle handle;
    auto failure = openFile(url, handle);
    if (!failure.isEmpty()) return failure;
    auto existing = readTags(handle, failure);
    if (!failure.isEmpty()) return failure;
    existing.append(tag);
    return writeTags(handle, existing);
}

QString FileTags::removeTag(const QUrl &url, const QString &tag) {
    Handle handle;
    auto failure = openFile(url, handle);
    if (!failure.isEmpty()) return failure;
    auto existing = readTags(handle, failure);
    if (!failure.isEmpty()) return failure;
    existing.removeAll(tag.trimmed());
    return writeTags(handle, existing);
}

QStringList FileTags::standardTags() {
    return {QStringLiteral("Red"), QStringLiteral("Orange"), QStringLiteral("Yellow"), QStringLiteral("Green"),
            QStringLiteral("Blue"), QStringLiteral("Purple"), QStringLiteral("Gray")};
}

QColor FileTags::tagColor(const QString &tag) {
    const auto name = tag.trimmed().toLower();
    if (name == "red") return QColor("#ff3b30");
    if (name == "orange") return QColor("#ff9500");
    if (name == "yellow") return QColor("#ffcc00");
    if (name == "green") return QColor("#34c759");
    if (name == "blue") return QColor("#007aff");
    if (name == "purple") return QColor("#af52de");
    return QColor("#8e8e93");
}
