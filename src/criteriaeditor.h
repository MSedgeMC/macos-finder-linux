// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#pragma once

#include <QJsonObject>
#include <QWidget>
#include <memory>

// Visual editor for the portable LocalFileSearch criteria schema. Validation
// and evaluation remain in the worker; this widget performs no filesystem I/O.
class SearchCriteriaEditor : public QWidget {
public:
    explicit SearchCriteriaEditor(const QJsonObject &criteria = {}, QWidget *parent = nullptr);
    ~SearchCriteriaEditor() override;
    QJsonObject criteria() const;
private:
    struct Private;
    std::unique_ptr<Private> d;
};
