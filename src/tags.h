// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#pragma once

#include <QColor>
#include <QStringList>
#include <QUrl>
#include <QHash>

// KDE-compatible local file tags in user.xdg.tags. Calls touch only tags on
// the selected local inode; unsupported/remote/symlink paths report errors.
// Call from a worker if a storage device may be slow. No hidden tag database.
namespace FileTags {
QStringList tags(const QUrl &url, QString *error = nullptr);
QHash<QUrl, QStringList> tagsForUrls(const QList<QUrl> &urls, QHash<QUrl, QString> *errors = nullptr);
QString setTags(const QUrl &url, const QStringList &tags);
QString addTag(const QUrl &url, const QString &tag);
QString removeTag(const QUrl &url, const QString &tag);
QStringList standardTags();
QColor tagColor(const QString &tag);
}
