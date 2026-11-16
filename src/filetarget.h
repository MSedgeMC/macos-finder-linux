// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#pragma once
#include <KFileItem>
#include <KIO/UDSEntry>

// Virtual listings (Recents, tags, search) keep their own model identity.
// Operations use the provider's explicit target, never a guessed local path.
namespace FileTarget {
inline QUrl url(const KFileItem &item) {
    const QUrl target = item.targetUrl();
    return target.isValid() && !target.isEmpty() ? target : item.url();
}
inline KFileItem item(KFileItem source) {
    const QUrl target = url(source);
    if (!source.isNull() && target != source.url()) {
        // Reconstruct from provider metadata so KFileItem's cached local-file
        // flags match the target too. setUrl() alone leaves those flags stale.
        auto metadata = source.entry();
        metadata.replace(KIO::UDSEntry::UDS_URL, target.toString());
        if (!target.fileName().isEmpty()) {
            metadata.replace(KIO::UDSEntry::UDS_NAME, target.fileName());
            metadata.replace(KIO::UDSEntry::UDS_DISPLAY_NAME, target.fileName());
        }
        return KFileItem(metadata, target, true);
    }
    return source;
}
}
