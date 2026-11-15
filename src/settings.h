// SPDX-License-Identifier: MIT
#pragma once
#include <QDialog>

class FinderSettings : public QDialog {
    Q_OBJECT
public:
    explicit FinderSettings(QWidget *parent = nullptr);
signals:
    void preferencesChanged();
};
