// SPDX-License-Identifier: MIT
#include "taboverview.h"
#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <iostream>
#include <cstdlib>

static void check(bool ok, const char *message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
static void key(QWidget *target, int code, Qt::KeyboardModifiers modifiers = {}) {
    QKeyEvent event(QEvent::KeyPress, code, modifiers); QApplication::sendEvent(target, &event);
}
int main(int argc, char **argv) {
    check(qgetenv("QT_QPA_PLATFORM") == "offscreen" && qEnvironmentVariableIsSet("XDG_CONFIG_HOME"), "private runner");
    QApplication app(argc, argv);
    auto *tabs = new QTabWidget;
    auto *a = new QLabel("Alpha contents"), *b = new QLabel("Beta contents"), *c = new QLabel("Gamma contents");
    tabs->addTab(a, "Alpha"); tabs->addTab(b, "Beta"); tabs->addTab(c, "Gamma");
    tabs->resize(800, 500); tabs->show(); app.processEvents();
    auto *overview = new FinderTabOverview(tabs); overview->show(); app.processEvents();
    auto *cards = overview->findChild<QListWidget *>("finderTabOverviewList");
    check(cards && cards->count() == 3 && !cards->item(0)->icon().isNull(), "real tab cards");
    cards->setCurrentRow(1); key(cards, Qt::Key_Right, Qt::AltModifier);
    check(tabs->widget(2) == b && cards->currentRow() == 2, "keyboard reorder moves same tab");
    // An externally moved tab must retain the card's target identity.
    tabs->tabBar()->moveTab(2, 0); app.processEvents();
    check(cards->currentItem()->text() == "Beta", "refresh retains card identity");
    key(cards, Qt::Key_Return); app.processEvents();
    check(tabs->currentWidget() == b && !overview->isVisible(), "activate target after reorder");
    auto *second = new FinderTabOverview(tabs); second->show(); app.processEvents();
    auto *secondCards = second->findChild<QListWidget *>("finderTabOverviewList");
    QObject::connect(second, &FinderTabOverview::closeTabRequested, tabs, [tabs](int index) { tabs->removeTab(index); });
    key(secondCards, Qt::Key_Delete); app.processEvents();
    check(tabs->count() == 2 && secondCards->count() == 2 && tabs->indexOf(b) < 0, "close selected tab refresh");
    QObject::connect(second, &FinderTabOverview::newTabRequested, tabs, [tabs] { tabs->addTab(new QLabel("New contents"), "New"); tabs->setCurrentIndex(tabs->count() - 1); });
    second->findChild<QPushButton *>("finderOverviewNewTab")->click(); app.processEvents();
    check(tabs->count() == 3 && secondCards->count() == 3, "new tab refresh");
    second->resize(370, 300); app.processEvents();
    check(second->findChild<QPushButton *>("finderOverviewCloseTab")->isVisible(), "narrow actions visible");
    delete tabs; app.processEvents(); check(!second->isVisible(), "borrowed tab destruction closes overview");
    std::cout << "PASS tab cards, reorder, identity, activation, close, new tab, narrow layout, lifecycle\n";

}
