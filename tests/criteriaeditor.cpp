// SPDX-License-Identifier: MIT
#include "criteriaeditor.h"
#include <QApplication>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QJsonArray>
#include <QLineEdit>
#include <QPushButton>
#include <iostream>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    auto require = [](bool value, const char *message) { if (!value) { std::cerr << "FAIL " << message << '\n'; std::exit(1); } };
    SearchCriteriaEditor empty;
    require(empty.criteria().isEmpty(), "empty editor means no restriction");
    empty.findChild<QPushButton *>("finderCriteriaAddCondition")->click();
    empty.findChild<QLineEdit *>("finderCriterionValue")->setText("planet");
    require(empty.criteria().value("criteria").toArray().first().toObject().value("value") == "planet", "edited condition serialized");
    auto *field = empty.findChild<QComboBox *>("finderCriterionField"); field->setCurrentIndex(field->findData("size"));
    empty.findChild<QLineEdit *>("finderCriterionValue")->setText("9007199254740993");
    require(empty.criteria().value("criteria").toArray().first().toObject().value("value") == "9007199254740993", "64-bit size remains exact string");
    const QJsonObject definition{{"match", "none"}, {"criteria", QJsonArray{
        QJsonObject{{"field", "modified"}, {"op", "greaterThan"}, {"value", "1700000000"}},
        QJsonObject{{"match", "any"}, {"criteria", QJsonArray{
            QJsonObject{{"field", "kind"}, {"op", "equals"}, {"value", "pdf"}},
            QJsonObject{{"field", "tag"}, {"op", "equals"}, {"value", "Project Ω"}}}}}}}};
    SearchCriteriaEditor nested(definition);
    require(nested.criteria() == definition, "nested none/any dates kinds and Unicode tags roundtrip");
    const QJsonObject leaf{{"field", "name"}, {"op", "endsWith"}, {"value", ".pdf"}};
    SearchCriteriaEditor single(leaf);
    require(single.criteria().value("criteria").toArray().first().toObject() == leaf, "single leaf wrapped without loss");
    auto *value = single.findChild<QLineEdit *>("finderCriterionValue");
    value->setText(QString(5000, 'a')); require(value->text().size() == 4096, "text input bounded");
    for (auto *button : single.findChildren<QPushButton *>()) if (button->accessibleName() == "Remove condition") { button->click(); break; }
    require(single.criteria().isEmpty(), "removal immediately excludes deferred widget");
    std::cout << "PASS criteria editor roundtrip, nested groups, exact values, bounds and removal\n";
}
