// SPDX-License-Identifier: MIT
#include "preferences.h"
#include <KProtocolInfo>
#include <QCoreApplication>
#include <QDir>
#include <QHash>
#include <QSettings>
#include <QUuid>
#include <QVariantMap>
#include <algorithm>

// Registry storage below uses only Qt Core/Gui. Keep it separate from widgets
// and from FileTags' inode metadata implementation; stored names remain native
// user.xdg.tags values, while IDs stabilize favorites across renames.
namespace {
const QList<FinderPreferences::Tag> &defaultTags() {
    static const QList<FinderPreferences::Tag> values{{"standard:red", "Red", QColor("#ff3b30")},
            {"standard:orange", "Orange", QColor("#ff9500")},
            {"standard:yellow", "Yellow", QColor("#ffcc00")},
            {"standard:green", "Green", QColor("#34c759")},
            {"standard:blue", "Blue", QColor("#007aff")},
            {"standard:purple", "Purple", QColor("#af52de")},
            {"standard:gray", "Gray", QColor("#8e8e93")}};
    return values;
}

struct RegistryCache {
    bool initialized = false;
    QString organization;
    QString domain;
    QString application;
    QList<FinderPreferences::Tag> tags;
    QHash<QString, QColor> colors;
};

RegistryCache &registryCache() {
    static RegistryCache cache;
    if (cache.organization != QCoreApplication::organizationName() || cache.application != QCoreApplication::applicationName()
        || cache.domain != QCoreApplication::organizationDomain()) {
        cache.initialized = false;
        cache.organization = QCoreApplication::organizationName();
        cache.domain = QCoreApplication::organizationDomain();
        cache.application = QCoreApplication::applicationName();
    }
    return cache;
}

void cacheRegistry(const QList<FinderPreferences::Tag> &tags) {
    auto &cache = registryCache();
    cache.tags = tags;
    cache.colors.clear();
    for (const auto &tag : tags) cache.colors.insert(tag.name, tag.color);
    cache.initialized = true;
}

bool appendObservedTags(QList<FinderPreferences::Tag> &tags, const QStringList &names) {
    bool changed = false;
    for (const auto &value : names) {
        const auto name = value.trimmed();
        if (!FinderPreferences::validTagName(name) || std::any_of(tags.cbegin(), tags.cend(), [&name](const auto &tag) { return tag.name == name; })) continue;
        QColor color("#8e8e93");
        for (const auto &standard : defaultTags())
            if (standard.name.compare(name, Qt::CaseInsensitive) == 0) { color = standard.color; break; }
        tags.append({QUuid::createUuid().toString(QUuid::WithoutBraces), name, color});
        changed = true;
    }
    return changed;
}

void writeFavoriteSlots(const QStringList &favoriteIds, const QList<FinderPreferences::Tag> &tags) {
    QSettings settings;
    settings.setValue("Tags/favoriteSlots", favoriteIds);
    // Legacy readers see only names, while the authoritative slot list retains
    // empty slots and stable identity. Sidebar membership uses the registry.
    QStringList names;
    for (const auto &id : favoriteIds)
        for (const auto &tag : tags)
            if (tag.id == id) { names.append(tag.name); break; }
    settings.setValue("Tags/favorites", names);
}
}

namespace FinderPreferences {
QList<Tag> standardTagRegistry() { return defaultTags(); }

bool validTagName(const QString &name) {
    return !name.isEmpty() && name == name.trimmed() && name.size() <= 256 && !name.contains(',') &&
        std::none_of(name.cbegin(), name.cend(), [](QChar c) { return c.category() == QChar::Other_Control; });
}

void setTagRegistry(const QList<Tag> &requested) {
    QList<Tag> tags;
    QVariantList values;
    for (auto tag : requested) {
        if (tag.id.isEmpty() || !validTagName(tag.name) ||
            std::any_of(tags.cbegin(), tags.cend(), [&tag](const auto &other) { return tag.id == other.id || tag.name == other.name; })) continue;
        if (!tag.color.isValid()) tag.color = QColor("#8e8e93");
        tags.append(tag);
        values.append(QVariantMap{{"id", tag.id}, {"name", tag.name}, {"color", tag.color.name(QColor::HexArgb)}});
    }
    QSettings().setValue("Tags/registry", values);
    cacheRegistry(tags);
}

void invalidateTagRegistryCache() { registryCache().initialized = false; }

QList<Tag> tagRegistry() {
    if (registryCache().initialized) return registryCache().tags;
    QSettings settings;
    QList<Tag> result;
    if (!settings.contains("Tags/registry")) {
        result = defaultTags();
        appendObservedTags(result, settings.value("Tags/favorites").toStringList());
        appendObservedTags(result, settings.value("Sidebar/hiddenTags").toStringList());
        setTagRegistry(result);
        return result;
    }
    for (const auto &value : settings.value("Tags/registry").toList()) {
        const auto map = value.toMap();
        Tag tag{map.value("id").toString(), map.value("name").toString(), QColor(map.value("color").toString())};
        if (tag.id.isEmpty() || !validTagName(tag.name) ||
            std::any_of(result.cbegin(), result.cend(), [&tag](const Tag &other) { return other.id == tag.id || other.name == tag.name; })) continue;
        if (!tag.color.isValid()) tag.color = QColor("#8e8e93");
        result.append(tag);
    }
    cacheRegistry(result);
    return result;
}

bool registerObservedTags(const QStringList &names) {
    auto tags = tagRegistry();
    if (!appendObservedTags(tags, names)) return false;
    setTagRegistry(tags);
    return true;
}

QStringList favoriteTagIds() {
    QSettings settings;
    const auto tags = tagRegistry();
    QStringList result;
    if (settings.contains("Tags/favoriteSlots")) result = settings.value("Tags/favoriteSlots").toStringList();
    else {
        auto names = settings.value("Tags/favorites").toStringList();
        if (!settings.contains("Tags/favorites"))
            for (const auto &tag : defaultTags()) names.append(tag.name);
        for (const auto &name : names)
            for (const auto &tag : tags)
                if (tag.name == name) { result.append(tag.id); break; }
    }
    while (result.size() > 7) result.removeLast();
    while (result.size() < 7) result.append(QString());
    QStringList used;
    for (auto &id : result) {
        if (id.isEmpty()) continue;
        if (used.contains(id) || std::none_of(tags.cbegin(), tags.cend(), [&id](const Tag &tag) { return tag.id == id; })) id.clear();
        else used.append(id);
    }
    if (!settings.contains("Tags/favoriteSlots") || settings.value("Tags/favoriteSlots").toStringList() != result)
        writeFavoriteSlots(result, tags);
    return result;
}

void setFavoriteTagIds(const QStringList &requested) {
    auto ids = requested;
    const auto tags = tagRegistry();
    while (ids.size() > 7) ids.removeLast();
    while (ids.size() < 7) ids.append(QString());
    QStringList used;
    for (auto &id : ids) {
        if (id.isEmpty()) continue;
        if (used.contains(id) || std::none_of(tags.cbegin(), tags.cend(), [&id](const Tag &tag) { return tag.id == id; })) id.clear();
        else used.append(id);
    }
    writeFavoriteSlots(ids, tags);
}

QStringList favoriteTags() {
    const auto tags = tagRegistry();
    QStringList names;
    for (const auto &id : favoriteTagIds()) {
        QString name;
        for (const auto &tag : tags) if (tag.id == id) { name = tag.name; break; }
        names.append(name);
    }
    return names;
}

QColor registeredTagColor(const QString &name) {
    if (!registryCache().initialized) tagRegistry();
    const auto &colors = registryCache().colors;
    const auto found = colors.constFind(name);
    if (found != colors.cend()) return found.value();
    for (const auto &tag : defaultTags()) if (tag.name.compare(name, Qt::CaseInsensitive) == 0) return tag.color;
    return QColor("#8e8e93");
}

QUrl newWindowUrl() {
    const auto saved = QSettings().value("General/newWindowFolder").toString();
    if (!saved.isEmpty()) {
        // Older releases saved absolute paths. Do not reinterpret '#' or '%'
        // in those paths as URL syntax or replace an explicit saved choice.
        if (QDir::isAbsolutePath(saved)) return QUrl::fromLocalFile(saved);
        const QUrl url(saved);
        if (url.isValid() && !url.scheme().isEmpty()) return url;
    }
    return KProtocolInfo::isKnownProtocol("recentlyused")
        ? QUrl("recentlyused:/files") : QUrl::fromLocalFile(QDir::homePath());
}

QString defaultSearchScope() {
    QSettings settings;
    const auto scope = settings.value("Search/defaultScope").toString();
    if (scope == "computer" || scope == "current" || scope == "previous") return scope;
    // The old false choice searched Home, rather than the whole computer.
    // Migrate the explicit current-folder choice; broaden the other choice to
    // the documented computer scope, whose actual coverage the search reports.
    return settings.contains("Search/currentFolder") && settings.value("Search/currentFolder").toBool()
        ? QStringLiteral("current") : QStringLiteral("computer");
}

QString initialSearchScope() {
    const auto scope = defaultSearchScope();
    if (scope != "previous") return scope;
    const auto last = QSettings().value("Search/lastScope", "computer").toString();
    return last == "current" ? QStringLiteral("current") : QStringLiteral("computer");
}

void rememberSearchScope(const QString &scope) {
    if (scope == "computer" || scope == "current") QSettings().setValue("Search/lastScope", scope);
}

bool sidebarItemVisible(const QString &key) {
    QSettings settings;
    auto hidden = settings.value("Sidebar/hiddenKeys").toStringList();
    if (settings.value("Sidebar/locationGroupsVersion", 0).toInt() < 1) {
        if (hidden.removeAll("devices"))
            for (const auto &group : {"internalDisks", "externalDisks", "opticalMedia"})
                if (!hidden.contains(group)) hidden << group;
        if (hidden.contains("network") && !hidden.contains("connectedServers")) hidden << "connectedServers";
        settings.setValue("Sidebar/hiddenKeys", hidden);
        settings.setValue("Sidebar/locationGroupsVersion", 1);
    }
    if (hidden.contains(key)) return false;
    if (key == "filesystem" && hidden.contains("internalDisks")) return false;
    return true;
}
}

