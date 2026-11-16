// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#pragma once
#include <KFileItem>

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
        source.setUrl(target);
        if (!target.fileName().isEmpty()) source.setName(target.fileName());
    }
    return source;
}
}
