// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#pragma once

#include <KFilePlacesView>

// Finder-inspired presentation with KDE's native bookmarks, mount/eject,
// activation, keyboard, context-menu, and drop handling kept in the base view.
class FinderPlacesView : public KFilePlacesView {
public:
    explicit FinderPlacesView(QWidget *parent = nullptr);
};
