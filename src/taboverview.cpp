// SPDX-License-Identifier: MIT
#include "taboverview.h"
#include <QAbstractItemModel>
#include <QApplication>
#include <QDialogButtonBox>
#include <QDropEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <functional>

namespace {
class TabCards final : public QListWidget {
public:
    using QListWidget::QListWidget;
    std::function<void()> orderChanged;
protected:
    void dropEvent(QDropEvent *event) override {
        if (event->source() != this) { event->ignore(); return; }
        const int source = currentRow();
        const auto *target = itemAt(event->position().toPoint());
        const int destination = target ? row(target) : count() - 1;
        if (source < 0 || destination < 0 || source == destination) { event->ignore(); return; }
        auto *card = takeItem(source); insertItem(destination, card); setCurrentItem(card);
        event->setDropAction(Qt::MoveAction); event->accept();
        if (orderChanged) orderChanged();
    }
};
}

FinderTabOverview::FinderTabOverview(QTabWidget *tabs, QWidget *parent)
    : QDialog(parent), m_tabs(tabs), m_cards(new TabCards(this)) {
    setObjectName("finderTabOverview"); setWindowTitle(tr("All Tabs"));
    setAttribute(Qt::WA_DeleteOnClose); resize(720, 480);
    auto *layout = new QVBoxLayout(this);
    auto *help = new QLabel(tr("Choose a tab, or drag cards to change their order."), this);
    help->setWordWrap(true); layout->addWidget(help);
    m_cards->setObjectName("finderTabOverviewList");
    m_cards->setAccessibleName(tr("Finder tabs"));
    m_cards->setViewMode(QListView::IconMode); m_cards->setResizeMode(QListView::Adjust);
    m_cards->setMovement(QListView::Snap); m_cards->setDragDropMode(QAbstractItemView::InternalMove);
    m_cards->setDefaultDropAction(Qt::MoveAction); m_cards->setSelectionMode(QAbstractItemView::SingleSelection);
    m_cards->setIconSize(QSize(200, 112)); m_cards->setGridSize(QSize(220, 160));
    m_cards->setWordWrap(true); m_cards->setSpacing(8); layout->addWidget(m_cards, 1);
    m_cards->installEventFilter(this);
    static_cast<TabCards *>(m_cards)->orderChanged = [this] { reorder(); };
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto *add = buttons->addButton(tr("New Tab"), QDialogButtonBox::ActionRole);
    add->setObjectName("finderOverviewNewTab");
    m_closeTab = buttons->addButton(tr("Close Tab"), QDialogButtonBox::ActionRole);
    m_closeTab->setObjectName("finderOverviewCloseTab");
    auto *open = buttons->addButton(tr("Open Tab"), QDialogButtonBox::AcceptRole);
    open->setObjectName("finderOverviewOpenTab"); open->setDefault(true);
    layout->addWidget(buttons);
    connect(add, &QPushButton::clicked, this, &FinderTabOverview::newTabRequested);
    connect(m_closeTab, &QPushButton::clicked, this, &FinderTabOverview::closeCurrent);
    connect(open, &QPushButton::clicked, this, &FinderTabOverview::activateCurrent);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_cards, &QListWidget::itemClicked, this, [this] { activateCurrent(); });
    connect(m_cards, &QListWidget::itemActivated, this, [this] { activateCurrent(); });
    connect(m_cards->model(), &QAbstractItemModel::rowsMoved, this, [this] { reorder(); });
    connect(tabs->tabBar(), &QTabBar::tabMoved, this, [this] { if (!m_rebuilding) refresh(); });
    connect(tabs, &QTabWidget::currentChanged, this, [this] {
        if (!m_rebuilding) QTimer::singleShot(0, this, &FinderTabOverview::refresh);
    });
    connect(tabs, &QObject::destroyed, this, &QDialog::reject);
    refresh(); m_cards->setFocus();
}

int FinderTabOverview::currentTab() const {
    if (!m_tabs || !m_cards->currentItem()) return -1;
    return m_tabs->indexOf(m_cards->currentItem()->data(Qt::UserRole).value<QPointer<QWidget>>());
}
void FinderTabOverview::activateCurrent() {
    const int index = currentTab();
    if (index >= 0) { m_tabs->setCurrentIndex(index); accept(); }
}
void FinderTabOverview::closeCurrent() {
    const int index = currentTab();
    if (index < 0) return;
    emit closeTabRequested(index);
    if (m_tabs) refresh();
}
void FinderTabOverview::refresh() {
    if (!m_tabs || m_rebuilding) return;
    m_rebuilding = true;
    const auto selected = m_cards->currentItem() ? m_cards->currentItem()->data(Qt::UserRole).value<QPointer<QWidget>>() : QPointer<QWidget>(m_tabs->currentWidget());
    m_cards->clear();
    // Cached once per overview refresh, never a compositor capture or idle loop.
    // Bound the overview to the session's 100-tab limit and thumbnail size.
    for (int index = 0; index < qMin(m_tabs->count(), 100); ++index) {
        auto *page = m_tabs->widget(index);
        QPixmap thumbnail(QSize(200, 112) * devicePixelRatioF());
        thumbnail.setDevicePixelRatio(devicePixelRatioF()); thumbnail.fill(palette().color(QPalette::Base));
        if (page->width() > 0 && page->height() > 0) {
            QPainter painter(&thumbnail);
            const qreal factor = qMin(200.0 / page->width(), 112.0 / page->height());
            painter.translate((200 - page->width() * factor) / 2, (112 - page->height() * factor) / 2);
            painter.scale(factor, factor);
            page->render(&painter, QPoint(), QRegion(), QWidget::DrawWindowBackground | QWidget::DrawChildren);
        }
        auto *card = new QListWidgetItem(QIcon(thumbnail), m_tabs->tabText(index), m_cards);
        card->setData(Qt::UserRole, QVariant::fromValue(QPointer<QWidget>(page)));
        card->setToolTip(m_tabs->tabToolTip(index));
        card->setTextAlignment(Qt::AlignHCenter); card->setSizeHint(QSize(220, 160));
        if (page == selected) m_cards->setCurrentItem(card);
    }
    if (!m_cards->currentItem() && m_cards->count()) m_cards->setCurrentRow(0);
    m_closeTab->setEnabled(currentTab() >= 0);
    m_rebuilding = false;
}
void FinderTabOverview::reorder() {
    if (!m_tabs || m_rebuilding) return;
    m_rebuilding = true;
    for (int index = 0; index < m_cards->count(); ++index) {
        const int source = m_tabs->indexOf(m_cards->item(index)->data(Qt::UserRole).value<QPointer<QWidget>>());
        if (source >= 0 && source != index) m_tabs->tabBar()->moveTab(source, index);
    }
    m_rebuilding = false;
}
bool FinderTabOverview::eventFilter(QObject *object, QEvent *event) {
    if (object == m_cards && event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) { activateCurrent(); return true; }
        if (key->key() == Qt::Key_Backspace || key->key() == Qt::Key_Delete) { closeCurrent(); return true; }
        if (key->modifiers().testFlag(Qt::AltModifier) && (key->key() == Qt::Key_Left || key->key() == Qt::Key_Right)) {
            const int from = m_cards->currentRow();
            const int to = from + (key->key() == Qt::Key_Left ? -1 : 1);
            if (from >= 0 && to >= 0 && to < m_cards->count()) {
                auto *card = m_cards->takeItem(from); m_cards->insertItem(to, card); m_cards->setCurrentItem(card); reorder();
            }
            return true;
        }
    }
    return QDialog::eventFilter(object, event);
}
