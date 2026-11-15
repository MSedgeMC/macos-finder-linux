// SPDX-License-Identifier: MIT
#pragma once
#include <QDialog>
#include <QUrl>

// These helpers preserve explicit choices from older Finder versions. Store
// General/newWindowFolder as a URL string; Search/defaultScope is one of
// "computer", "current", "previous", and Search/lastScope excludes "previous".
namespace FinderPreferences {
QUrl newWindowUrl();
QString defaultSearchScope();
QString initialSearchScope();
void rememberSearchScope(const QString &scope);
bool sidebarItemVisible(const QString &key);
}

class FinderSettings : public QDialog {
    Q_OBJECT
public:
    explicit FinderSettings(QWidget *parent = nullptr);
signals:
    void preferencesChanged();
};
