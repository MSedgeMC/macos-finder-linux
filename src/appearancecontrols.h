// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#pragma once
#include "appearance.h"
#include <QApplication>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <functional>

// One set of immediate, signal-blocked controls shared by Finder and Preview.
namespace FinderAppearanceControls {
inline void addRows(QFormLayout *form, QWidget *owner, std::function<void()> changed = {}) {
    auto combo = [=](const QString &label, const QString &name, const QString &key,
                     const QList<QPair<QString, QString>> &choices, const QString &fallback) {
        auto *box = new QComboBox(owner); box->setObjectName(name); box->setAccessibleName(label);
        for (const auto &choice : choices) box->addItem(choice.first, choice.second);
        auto reload = [=] {
            const QSignalBlocker blocker(box);
            const int index = box->findData(QSettings().value(key, fallback));
            box->setCurrentIndex(index < 0 ? box->findData(fallback) : index);
        };
        reload(); form->addRow(label, box);
        QObject::connect(FinderAppearance::instance(), &FinderAppearance::changed, box, reload);
        QObject::connect(box, &QComboBox::currentIndexChanged, owner, [=] {
            QSettings().setValue(key, box->currentData()); FinderAppearance::apply(qApp);
            if (changed) changed();
        });
    };
    combo(QObject::tr("Window controls:"), "finderSettingsWindowControlsStyle", "Appearance/windowControlsStyle",
          {{QObject::tr("Glossy"), "glossy"}, {QObject::tr("Flat"), "flat"}}, "glossy");
    combo(QObject::tr("Position:"), "finderSettingsWindowControlsSide", "Appearance/windowControlsSide",
          {{QObject::tr("Right"), "right"}, {QObject::tr("Left"), "left"}}, "right");
    combo(QObject::tr("Control symbols:"), "finderSettingsWindowControlsSymbols", "Appearance/windowControlsSymbols",
          {{QObject::tr("Always visible"), "always"}, {QObject::tr("On hover"), "hover"}}, "always");
    auto *surface = new QWidget(owner); auto *row = new QHBoxLayout(surface); row->setContentsMargins(0,0,0,0);
    auto *slider = new QSlider(Qt::Horizontal, surface); slider->setObjectName("finderSettingsSidebarTranslucency");
    slider->setAccessibleName(QObject::tr("Sidebar translucency")); slider->setRange(0,40); slider->setSingleStep(1); slider->setPageStep(5);
    auto *value = new QLabel(surface); value->setObjectName("finderSettingsSidebarTranslucencyValue"); value->setMinimumWidth(38);
    row->addWidget(slider,1); row->addWidget(value); form->addRow(QObject::tr("Sidebar translucency:"), surface);
    auto *help = new QLabel(owner); help->setWordWrap(true); help->setProperty("settingsHelp",true); form->addRow(help);
    auto reload = [=] {
        const QSignalBlocker blocker(slider);
        slider->setValue(FinderAppearance::sidebarTranslucency()); value->setText(QString::number(slider->value())+"%");
        const bool solid = FinderAppearance::reduceTransparency() || FinderAppearance::highContrast();
        slider->setEnabled(!solid);
        help->setText(solid ? QObject::tr("Accessibility settings keep the sidebar opaque. Your translucency preference is saved.")
                           : QObject::tr("0% is opaque. Higher values reveal more of the blurred backdrop. Applies to Finder and Preview; blur requires compositor support."));
    };
    reload(); QObject::connect(FinderAppearance::instance(), &FinderAppearance::changed, slider, reload);
    QObject::connect(slider, &QSlider::valueChanged, owner, [=](int percent) {
        QSettings().setValue("Appearance/sidebarTranslucency",percent); FinderAppearance::apply(qApp);
        if(changed) changed();
    });
}
}
