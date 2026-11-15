// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#include "criteriaeditor.h"

#include <QComboBox>
#include <QDateTimeEdit>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <functional>
#include <algorithm>
#include <vector>

namespace {
class Condition : public QWidget {
public:
    explicit Condition(const QJsonObject &initial, QWidget *parent) : QWidget(parent) {
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        field = new QComboBox(this); field->setObjectName("finderCriterionField"); field->setAccessibleName("Criterion field");
        for (const auto &entry : QList<QPair<QString, QString>>{{"Name", "name"}, {"Kind", "kind"}, {"Size", "size"}, {"Date Modified", "modified"}, {"Date Created", "created"}, {"Tags", "tag"}, {"Contents", "content"}})
            field->addItem(entry.first, entry.second);
        operation = new QComboBox(this); operation->setObjectName("finderCriterionOperator"); operation->setAccessibleName("Comparison");
        values = new QStackedWidget(this);
        text = new QLineEdit(this); text->setObjectName("finderCriterionValue"); text->setAccessibleName("Criterion value"); text->setMaxLength(4096);
        kinds = new QComboBox(this); kinds->setObjectName("finderCriterionKind"); kinds->setAccessibleName("File kind");
        for (const auto &entry : QList<QPair<QString, QString>>{{"Document", "text"}, {"Folder", "folder"}, {"Image", "image"}, {"PDF", "pdf"}, {"Music", "audio"}, {"Movie", "video"}, {"Archive", "archive"}, {"File", "file"}})
            kinds->addItem(entry.first, entry.second);
        date = new QDateTimeEdit(QDateTime::currentDateTime(), this); date->setObjectName("finderCriterionDate"); date->setAccessibleName("Criterion date and time"); date->setCalendarPopup(true); date->setDisplayFormat("yyyy-MM-dd HH:mm:ss");
        values->addWidget(text); values->addWidget(kinds); values->addWidget(date);
        layout->addWidget(field); layout->addWidget(operation); layout->addWidget(values, 1);
        remove = new QPushButton(QStringLiteral("−"), this); remove->setAccessibleName("Remove condition"); remove->setFixedWidth(28); layout->addWidget(remove);
        connect(field, &QComboBox::currentIndexChanged, this, [this] { configure(); });
        field->setCurrentIndex(qMax(0, field->findData(initial.value("field").toString()))); configure();
        const auto value = initial.value("value");
        text->setText(value.isString() ? value.toString() : value.isDouble() ? QString::number(value.toInteger()) : QString());
        kinds->setCurrentIndex(qMax(0, kinds->findData(value.toString())));
        bool valid = false; const qint64 seconds = value.isString() ? value.toString().toLongLong(&valid) : value.toInteger();
        if (value.isDouble() || valid) date->setDateTime(QDateTime::fromSecsSinceEpoch(seconds));
        operation->setCurrentIndex(qMax(0, operation->findData(initial.value("op").toString())));
    }
    QJsonObject definition() const {
        const auto key = field->currentData().toString();
        QString value = key == "kind" ? kinds->currentData().toString() : key == "modified" || key == "created" ? QString::number(date->dateTime().toSecsSinceEpoch()) : text->text();
        return {{"field", key}, {"op", operation->currentData().toString()}, {"value", value}};
    }
    QPushButton *remove;
private:
    void configure() {
        const auto key = field->currentData().toString(); const auto previous = operation->currentData(); operation->clear();
        QList<QPair<QString, QString>> options;
        if (key == "size") options = {{"is", "equals"}, {"is at least", "atLeast"}, {"is at most", "atMost"}, {"is greater than", "greaterThan"}, {"is less than", "lessThan"}};
        else if (key == "modified" || key == "created") options = {{"is", "equals"}, {"is on or after", "atLeast"}, {"is on or before", "atMost"}, {"is after", "greaterThan"}, {"is before", "lessThan"}};
        else if (key == "kind" || key == "tag") options = {{"is", "equals"}};
        else options = {{"contains", "contains"}, {"is", "equals"}, {"begins with", "beginsWith"}, {"ends with", "endsWith"}};
        for (const auto &entry : options) operation->addItem(entry.first, entry.second);
        operation->setCurrentIndex(qMax(0, operation->findData(previous)));
        values->setCurrentWidget(key == "kind" ? static_cast<QWidget *>(kinds) : key == "modified" || key == "created" ? static_cast<QWidget *>(date) : static_cast<QWidget *>(text));
        text->setPlaceholderText(key == "size" ? "Size in bytes" : key == "tag" ? "Tag name" : "Text");
    }
    QComboBox *field, *operation, *kinds;
    QLineEdit *text;
    QDateTimeEdit *date;
    QStackedWidget *values;
};

class Group : public QFrame {
public:
    Group(const QJsonObject &initial, int depth, QWidget *parent) : QFrame(parent), depth(depth) {
        if (depth) setFrameShape(QFrame::StyledPanel);
        auto *layout = new QVBoxLayout(this); layout->setContentsMargins(depth ? 10 : 0, depth ? 8 : 0, depth ? 10 : 0, depth ? 8 : 0);
        auto *header = new QHBoxLayout;
        header->addWidget(new QLabel("Match", this));
        match = new QComboBox(this); match->setObjectName("finderCriteriaMatch"); match->setAccessibleName("Match conditions");
        for (const auto &entry : QList<QPair<QString, QString>>{{"All", "all"}, {"Any", "any"}, {"None", "none"}}) match->addItem(entry.first, entry.second);
        match->setCurrentIndex(qMax(0, match->findData(initial.value("match").toString("all")))); header->addWidget(match); header->addWidget(new QLabel("of the following:", this)); header->addStretch();
        remove = new QPushButton(QStringLiteral("−"), this); remove->setAccessibleName("Remove condition group"); remove->setFixedWidth(28); if (depth) header->addWidget(remove); else remove->hide();
        layout->addLayout(header); rows = new QVBoxLayout; layout->addLayout(rows);
        auto *buttons = new QHBoxLayout;
        auto *condition = new QPushButton("Add Condition", this); condition->setObjectName("finderCriteriaAddCondition");
        auto *group = new QPushButton("Add Group", this); group->setObjectName("finderCriteriaAddGroup"); group->setEnabled(depth < 11);
        buttons->addWidget(condition); buttons->addWidget(group); buttons->addStretch(); layout->addLayout(buttons);
        connect(condition, &QPushButton::clicked, this, [this] { add({{"field", "name"}, {"op", "contains"}, {"value", ""}}); });
        connect(group, &QPushButton::clicked, this, [this] { add({{"match", "any"}, {"criteria", QJsonArray{QJsonObject{{"field", "name"}, {"op", "contains"}, {"value", ""}}}}}); });
        for (const auto &value : initial.value("criteria").toArray()) if (value.isObject()) add(value.toObject());
    }
    QJsonObject definition() const {
        QJsonArray array; for (const auto &entry : children) if (entry.widget) array.append(entry.read());
        return depth == 0 && array.isEmpty() ? QJsonObject{} : QJsonObject{{"match", match->currentData().toString()}, {"criteria", array}};
    }
    QPushButton *remove;
private:
    struct Entry { QPointer<QWidget> widget; std::function<QJsonObject()> read; };
    void add(const QJsonObject &initial) {
        QWidget *widget; QPushButton *button; std::function<QJsonObject()> read;
        if (initial.contains("match") && depth < 11) {
            auto *group = new Group(initial, depth + 1, this); widget = group; button = group->remove; read = [group] { return group->definition(); };
        } else {
            auto *condition = new Condition(initial, this); widget = condition; button = condition->remove; read = [condition] { return condition->definition(); };
        }
        children.push_back({widget, std::move(read)}); rows->addWidget(widget);
        connect(button, &QPushButton::clicked, this, [this, widget] {
            children.erase(std::remove_if(children.begin(), children.end(), [widget](const Entry &entry) { return entry.widget == widget; }), children.end());
            rows->removeWidget(widget); widget->deleteLater();
        });
    }
    int depth;
    QComboBox *match;
    QVBoxLayout *rows;
    std::vector<Entry> children;
};
}

struct SearchCriteriaEditor::Private { Group *root; };
SearchCriteriaEditor::SearchCriteriaEditor(const QJsonObject &criteria, QWidget *parent) : QWidget(parent), d(std::make_unique<Private>()) {
    setObjectName("finderCriteriaEditor"); setAccessibleName("Search criteria");
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0);
    QJsonObject group = criteria;
    if (!criteria.isEmpty() && !criteria.contains("match")) group = {{"match", "all"}, {"criteria", QJsonArray{criteria}}};
    d->root = new Group(group, 0, this); layout->addWidget(d->root);
}
SearchCriteriaEditor::~SearchCriteriaEditor() = default;
QJsonObject SearchCriteriaEditor::criteria() const { return d->root->definition(); }
