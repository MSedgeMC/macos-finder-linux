// SPDX-License-Identifier: MIT
#pragma once
#include <QDialog>
#include <QPointer>

class QTabWidget;
class QListWidget;
class QPushButton;

// A borrowed tab host. Cards carry widget identity, never a stale tab number.
class FinderTabOverview final : public QDialog {
    Q_OBJECT
public:
    explicit FinderTabOverview(QTabWidget *tabs, QWidget *parent = nullptr);
    void refresh();
signals:
    void newTabRequested();
    void closeTabRequested(int index);
private:
    QPointer<QTabWidget> m_tabs;
    QListWidget *m_cards;
    QPushButton *m_closeTab;
    bool m_rebuilding = false;
    void activateCurrent();
    void closeCurrent();
    void reorder();
    int currentTab() const;
    bool eventFilter(QObject *object, QEvent *event) override;
};

Q_DECLARE_METATYPE(QPointer<QWidget>)
