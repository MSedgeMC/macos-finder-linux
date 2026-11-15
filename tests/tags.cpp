// SPDX-License-Identifier: MIT
// Independent xattr interoperability checks on private temporary fixtures.
#include "tags.h"
#include <KFileMetaData/UserMetaData>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <cstdio>
#include <stdexcept>
#include <sys/xattr.h>

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
QByteArray attribute(const QString &path, const char *name) {
    QByteArray result(65536, Qt::Uninitialized);
    const auto count = getxattr(QFile::encodeName(path).constData(), name, result.data(), result.size());
    require(count >= 0, "read test xattr");
    result.resize(count);
    return result;
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QJsonArray cases;
    try {
        QTemporaryDir temporary;
        require(temporary.isValid(), "temporary root");
        const auto path = temporary.path() + "/fixture.txt";
        QFile file(path);
        require(file.open(QIODevice::WriteOnly), "fixture file");
        file.write("unchanged contents\n");
        file.close();
        const auto url = QUrl::fromLocalFile(path);
        const auto encoded = QFile::encodeName(path);
        require(setxattr(encoded.constData(), "user.xdg.tags", "Existing,custom", 15, 0) == 0, "seed native tags");
        require(setxattr(encoded.constData(), "user.xdg.comment", "Keep comment", 12, 0) == 0, "seed unrelated attribute");
        QString error;
        auto tags = FileTags::tags(url, &error);
        require(error.isEmpty() && tags == QStringList{"Existing", "custom"}, "native tags read");
        cases.append("reads existing Linux user.xdg.tags");
        require(FileTags::addTag(url, " Red ").isEmpty(), "add color");
        tags = FileTags::tags(url, &error);
        require(error.isEmpty() && tags == QStringList{"Existing", "custom", "Red"}, "custom tags lost");
        require(attribute(path, "user.xdg.comment") == "Keep comment", "unrelated attribute changed");
        require(FileTags::addTag(url, "Red").isEmpty(), "idempotent color add");
        require(FileTags::tags(url).size() == 3, "duplicate tag stored");
        require(FileTags::removeTag(url, "Red").isEmpty(), "remove color");
        require(FileTags::tags(url) == QStringList{"Existing", "custom"}, "custom tag removal");
        cases.append("color toggles preserve custom tags and unrelated xattrs");
        require(FileTags::setTags(url, {"Blue", "Project α", "Blue", " ", " custom "}).isEmpty(), "custom unicode write");
        require(FileTags::tags(url) == QStringList{"Blue", "Project α", "custom"}, "normalization mismatch");
        KFileMetaData::UserMetaData metadata(path);
        require(metadata.tags() == FileTags::tags(url), "KDE cannot read saved tags");
        require(metadata.setTags({"Orange", "external application"}) == KFileMetaData::UserMetaData::NoError, "native API seed");
        require(FileTags::tags(url) == QStringList{"Orange", "external application"}, "external native write not read");
        cases.append("Unicode normalization and public KDE API interoperability");
        const auto before = attribute(path, "user.xdg.tags");
        require(!FileTags::setTags(url, {"bad,tag"}).isEmpty(), "comma accepted");
        require(!FileTags::setTags(url, {"bad\ntag"}).isEmpty(), "control accepted");
        require(!FileTags::setTags(url, {QString(257, 'x')}).isEmpty(), "oversize tag accepted");
        require(attribute(path, "user.xdg.tags") == before, "invalid tags changed metadata");
        cases.append("invalid tags rejected before metadata writes");
        require(FileTags::setTags(url, {}).isEmpty(), "clear tags");
        require(FileTags::tags(url, &error).isEmpty() && error.isEmpty(), "clear/read empty tags");
        require(FileTags::setTags(url, {}).isEmpty(), "idempotent clear absent tags");
        cases.append("empty tags remove attribute and clearing is idempotent");
        const auto folder = temporary.path() + "/folder";
        require(QDir().mkdir(folder), "directory fixture");
        require(FileTags::setTags(QUrl::fromLocalFile(folder), {"Green"}).isEmpty(), "directory tag write");
        require(FileTags::tags(QUrl::fromLocalFile(folder)) == QStringList{"Green"}, "directory tags read");
        cases.append("directory tags use the same native metadata");
        const auto link = temporary.path() + "/link";
        require(QFile::link(path, link), "symlink fixture");
        require(!FileTags::setTags(QUrl::fromLocalFile(link), {"Red"}).isEmpty(), "followed symlink for write");
        FileTags::tags(QUrl::fromLocalFile(link), &error);
        require(!error.isEmpty(), "symlink read fallback missing");
        const auto folderLink = temporary.path() + "/folder-link";
        require(QFile::link(folder, folderLink), "ancestor symlink fixture");
        require(!FileTags::setTags(QUrl::fromLocalFile(folderLink + "/child"), {"Red"}).isEmpty(), "ancestor symlink allowed");
        require(!FileTags::setTags(QUrl("smb://server/share/file"), {"Red"}).isEmpty(), "remote write allowed");
        require(!FileTags::setTags(QUrl::fromLocalFile("/proc/self/status"), {"Red"}).isEmpty(), "virtual mount write allowed");
        require(!FileTags::setTags(QUrl::fromLocalFile(temporary.path() + "/missing"), {"Red"}).isEmpty(), "missing file write allowed");
        cases.append("symlinks, remote/virtual storage and missing files report errors");
        QHash<QUrl, QString> batchErrors;
        const auto folderUrl = QUrl::fromLocalFile(folder);
        const auto linkUrl = QUrl::fromLocalFile(link);
        const auto remoteUrl = QUrl("smb://server/share/fixture");
        const auto batch = FileTags::tagsForUrls({url, folderUrl, linkUrl, remoteUrl}, &batchErrors);
        require(batch.value(url) == FileTags::tags(url) && batch.value(folderUrl) == QStringList{"Green"}, "batch metadata differs from native tags");
        require(batchErrors.size() == 2 && batchErrors.contains(linkUrl) && batchErrors.contains(remoteUrl), "batch storage errors lost");
        cases.append("bounded batch reads retain native tag values and per-file storage errors");
        require(FileTags::standardTags().size() == 7 && FileTags::tagColor("RED") == QColor("#ff3b30"), "color mapping");
        require(FileTags::tagColor("custom") == FileTags::tagColor("Gray"), "custom color fallback");
        cases.append("seven standard colors and neutral custom-tag fallback");
        require(file.open(QIODevice::ReadOnly) && file.readAll() == "unchanged contents\n", "file content changed");
        cases.append("file contents stay unchanged");
        std::puts(QJsonDocument(QJsonObject{{"passed", true}, {"tests", cases.size()}, {"cases", cases},
                    {"gui_windows_launched", 0}, {"production_files_changed", 0}}).toJson().constData());
        return 0;
    } catch (const std::exception &failure) {
        std::fprintf(stderr, "tag test failed: %s\n", failure.what());
        return 1;
    }
}
