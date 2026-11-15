// SPDX-License-Identifier: MIT
#pragma once
class QWidget;
namespace FinderWindowFrame {
// Installs native edge resize and cursor feedback on one frameless top-level.
// Menus/dialogs and maximized/fullscreen windows are excluded. No idle polling.
void install(QWidget *window);
}
