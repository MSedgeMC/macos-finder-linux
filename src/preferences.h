// SPDX-License-Identifier: MIT
#pragma once
#include <QColor>
#include <QList>
#include <QStringList>
#include <QUrl>

// These helpers preserve explicit choices from older Finder versions. Store
// General/newWindowFolder as a URL string; Search/defaultScope is one of
// "computer", "current", "previous", and Search/lastScope excludes "previous".
namespace FinderPreferences {
// Core/Gui-only registry contract. Mutate from the application thread after
// bounded directory/search metadata discovery, never by scanning a whole disk.
// Tags/registry is a QVariantList of maps {id, name, color (#AARRGGBB)}.
// Tags/favoriteSlots is exactly seven stable IDs, with "" for an empty slot.
struct Tag {
    QString id;
    QString name;
    QColor color;
};
QList<Tag> tagRegistry();
QList<Tag> standardTagRegistry();
bool validTagName(const QString &name);
void setTagRegistry(const QList<Tag> &tags);
void setFavoriteTagIds(const QStringList &ids);
// Main-thread cache: registeredTagColor is O(1), with no QSettings reads after
// initialization. Registry writes refresh it; call this after external writes.
void invalidateTagRegistryCache();
bool registerObservedTags(const QStringList &names);
QStringList favoriteTagIds();
QStringList favoriteTags(); // Seven names, retaining empty shortcut slots.
QColor registeredTagColor(const QString &name);
QUrl newWindowUrl();
QString defaultSearchScope();
QString initialSearchScope();
void rememberSearchScope(const QString &scope);
bool sidebarItemVisible(const QString &key);
}

