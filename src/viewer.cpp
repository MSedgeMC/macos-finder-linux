// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#include "viewer.h"
#include "preview.h"
#include "quickactions.h"
#include "appearance.h"
#include "appearancecontrols.h"
#include "windowcontour.h"
#include "sidebarsplitter.h"
#include "windowcontrols.h"
#include "windowframe.h"
#include "gestures.h"
#include "scroll.h"
#include "glasscontrols.h"
#include <KPropertiesDialog>
#include <QIconEngine>
#include <QAbstractScrollArea>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QTextDocument>
#include <QWindow>
#include <QMouseEvent>
#include "alerts.h"
#include <QAction>
#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QListWidget>
#include <QMenuBar>
#include <QMimeData>
#include <QMimeDatabase>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QIcon>
#include <QClipboard>
#include <QToolButton>
#include <QScrollBar>
#include <QResizeEvent>
#include <QStyledItemDelegate>
#include <QStyleOptionFocusRect>
#include <QStyle>
#include <KIO/PreviewJob>
#include <KFileItem>
#include <KIO/FileUndoManager>

namespace {
class ThumbnailDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override {
        const auto *view = qobject_cast<const QListWidget *>(option.widget);
        return QSize(view ? qMax(1, view->viewport()->width() - 2 * view->spacing()) : 180, 140);
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QStyleOptionViewItem item(option); initStyleOption(&item, index);
        painter->save(); painter->setClipRect(item.rect);
        const bool selected = item.state & QStyle::State_Selected;
        if (selected) {
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen); painter->setBrush(item.palette.highlight());
            painter->drawRoundedRect(item.rect.adjusted(2, 2, -2, -2), 7, 7);
        }
        const int edge = qMax(40, qMin(144, item.rect.width() - 20));
        const QRect icon(item.rect.center().x() - edge / 2, item.rect.top() + 8, edge, 100);
        item.icon.paint(painter, icon, Qt::AlignCenter,
                        item.state & QStyle::State_Enabled ? QIcon::Normal : QIcon::Disabled);
        const QRect label = item.rect.adjusted(6, 114, -6, -5);
        painter->setFont(item.font);
        painter->setPen(selected ? item.palette.highlightedText().color() : FinderAppearance::color(FinderAppearance::SidebarText));
        painter->drawText(label, Qt::AlignHCenter | Qt::AlignVCenter,
                          item.fontMetrics.elidedText(item.text, Qt::ElideMiddle, label.width()));
        if (item.state & QStyle::State_HasFocus) {
            QStyleOptionFocusRect focus; focus.QStyleOption::operator=(item);
            focus.rect = item.rect.adjusted(3, 3, -3, -3);
            focus.backgroundColor = selected ? item.palette.highlight().color() : item.palette.base().color();
            (item.widget ? item.widget->style() : QApplication::style())->drawPrimitive(QStyle::PE_FrameFocusRect, &focus, painter, item.widget);
        }
        painter->restore();
    }
};
class DocumentSidebar final : public QListWidget {
public:
    explicit DocumentSidebar(QWidget *parent) : QListWidget(parent) {
        setViewMode(QListView::IconMode); setFlow(QListView::TopToBottom);
        setWrapping(false); setMovement(QListView::Static);
        setResizeMode(QListView::Adjust); setSpacing(6);
        setWordWrap(false); setTextElideMode(Qt::ElideMiddle);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setIconSize(QSize(144, 100));
        setItemDelegate(new ThumbnailDelegate(this));
    }
protected:
    void resizeEvent(QResizeEvent *event) override {
        QListWidget::resizeEvent(event);
        setGridSize(QSize(qMax(1, viewport()->width() - 2 * spacing()), 140));
    }
};
// Original vector symbols; foreground follows the shared semantic palette.
class DocumentIcon final : public QIconEngine {
public:
    explicit DocumentIcon(QString name) : name(std::move(name)) {}
    QIconEngine *clone() const override { return new DocumentIcon(name); }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        QPixmap result(size); result.fill(Qt::transparent); QPainter painter(&result);
        paint(&painter, QRect(QPoint(), size), mode, state); return result;
    }
    void paint(QPainter *p, const QRect &rect, QIcon::Mode mode, QIcon::State) override {
        p->save(); p->setRenderHint(QPainter::Antialiasing); p->translate(rect.topLeft());
        p->scale(rect.width() / 24., rect.height() / 24.);
        QColor color = FinderAppearance::color(FinderAppearance::HeaderText);
        if (mode == QIcon::Disabled) color.setAlpha(95);
        p->setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); p->setBrush(Qt::NoBrush);
        if (name == "sidebar") { p->drawRoundedRect(QRectF(3, 5, 18, 14), 2, 2); p->drawLine(9,5,9,19); p->drawLine(5,8,7,8); p->drawLine(5,11,7,11); }
        else if (name.startsWith("zoom") || name == "actual" || name == "find") {
            p->drawEllipse(QRectF(3,3,13,13)); p->drawLine(QPointF(14,14),QPointF(21,21));
            if (name == "zoomOut" || name == "zoomIn") p->drawLine(6,9,13,9);
            if (name == "zoomIn") p->drawLine(9,6,9,13);
            if (name == "actual") { p->drawLine(8,7,10,6); p->drawLine(10,6,10,12); }
        } else if (name == "markup") {
            QPainterPath path; path.moveTo(5,16); path.lineTo(15,5); path.lineTo(19,9); path.lineTo(9,19); path.closeSubpath(); p->drawPath(path);
            p->drawLine(13,7,17,11); p->drawLine(5,16,4,21); p->drawLine(4,21,9,19); p->drawLine(4,22,20,22);
        } else if (name == "rotate") {
            p->drawRoundedRect(QRectF(8,9,12,12),1,1); p->drawArc(QRectF(3,2,15,14),35*16,200*16); p->drawLine(3,9,3,4); p->drawLine(3,4,8,4);
        } else if (name == "share") {
            p->drawLine(12,3,12,15); p->drawLine(12,3,8,7); p->drawLine(12,3,16,7);
            QPainterPath path; path.moveTo(7,10); path.lineTo(4,10); path.lineTo(4,21); path.lineTo(20,21); path.lineTo(20,10); path.lineTo(17,10); p->drawPath(path);
        } else if (name == "info") {
            p->drawEllipse(QRectF(3,3,18,18)); p->drawPoint(12,7); p->drawLine(12,11,12,17);
        } else { p->drawLine(5,7,19,7); p->drawLine(5,12,19,12); p->drawLine(5,17,19,17); }
        p->restore();
    }
private: QString name;
};
class DocumentTitle final : public QLabel {
public:
    using QLabel::QLabel;
    void setTitle(const QString &value) { title = value; setToolTip(value); setAccessibleName(value); refresh(); }
protected:
    void resizeEvent(QResizeEvent *event) override { QLabel::resizeEvent(event); refresh(); }
private:
    void refresh() { setText(fontMetrics().elidedText(title, Qt::ElideMiddle, width())); }
    QString title;
};
class ViewerSplitter final : public SidebarSplitter {
public:
    using SidebarSplitter::SidebarSplitter;
    bool blur = false;
protected:
    void paintEvent(QPaintEvent *event) override {
        QPainter painter(this); QColor tint = FinderAppearance::materialTint();
        if (!blur) tint.setAlpha(255);
        if (count() && widget(0)->isVisible()) painter.fillRect(widget(0)->geometry(), tint);
        painter.end(); SidebarSplitter::paintEvent(event);
    }
};
class ViewerHeader final : public QWidget {
public:
    using QWidget::QWidget;
    bool blur = false;
    int sidebarWidth = 0;
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this); painter.fillRect(QRect(sidebarWidth,0,width()-sidebarWidth,height()),FinderAppearance::color(FinderAppearance::Header));
        QColor tint = FinderAppearance::materialTint(); if (!blur) tint.setAlpha(255);
        painter.fillRect(QRect(0,0,sidebarWidth,height()),tint);
    }
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && !window()->isFullScreen() && window()->windowHandle()) window()->windowHandle()->startSystemMove();
    }
    void mouseDoubleClickEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && !window()->isFullScreen()) { if (window()->isMaximized()) window()->showNormal(); else window()->showMaximized(); }
    }
};
}
ViewerWindow::ViewerWindow(const QList<QUrl> &urls, QWidget *parent) : QMainWindow(parent) {
    setObjectName("finderPreviewApp"); setWindowTitle(tr("Preview")); setWindowIcon(QIcon::fromTheme("image-x-generic"));
    resize(960, 700); setMinimumSize(480, 320); setAcceptDrops(true);
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint); setAttribute(Qt::WA_TranslucentBackground);
    auto *base = new QWidget(this); base->setGraphicsEffect(new ContourEffect(base)); setCentralWidget(base);
    auto *layout = new QVBoxLayout(base); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    auto *header = new ViewerHeader(base); header->setFixedHeight(56); header->setObjectName("previewAppHeader");
    auto *chrome = new QHBoxLayout(header); chrome->setContentsMargins(14, 8, 14, 8); chrome->setSpacing(10);
    layout->addWidget(header);
    auto *splitter = new ViewerSplitter(base); splitter->setObjectName("previewAppSplitter"); layout->addWidget(splitter, 1);
    m_sidebar = new DocumentSidebar(splitter); m_sidebar->setObjectName("previewAppSidebar");
    m_sidebar->setAccessibleName(tr("Open documents"));
    m_sidebar->setAutoFillBackground(false); m_sidebar->viewport()->setAutoFillBackground(false);
    m_sidebar->setMinimumWidth(100); m_sidebar->setMaximumWidth(260);
    m_preview = new PreviewPane(splitter); m_preview->setHostWindowControls(true); m_preview->setDocumentPresentation(true);
    // Export application menus to Plasma; a persistent toolbar menu gives the
    // same commands when the session has no global menu applet.
    auto *menus = new QMenuBar(this); menus->setNativeMenuBar(true); setMenuBar(menus); menus->hide();
    FinderWindowFrame::install(this);
    auto *gestures = FinderGestures::Controller::install(this); gestures->setZoomTarget(m_preview);
    connect(gestures, &FinderGestures::Controller::zoomRequested, this, [this](QWidget *, qreal factor, const QPointF &) { m_preview->setZoomFactor(m_preview->zoomFactor() * factor); });
    splitter->setStretchFactor(0, 0); splitter->setStretchFactor(1, 1); splitter->setSizes({210, 750});
    m_edits = new QuickActions(this);
    m_status = new QStatusBar(base); m_status->setSizeGripEnabled(false); m_status->hide(); layout->addWidget(m_status);
    auto *application = menus->addMenu(tr("Preview"));
    auto *preferences = application->addAction(tr("Settings…"), this, [this] { settingsDialog(); });
    preferences->setObjectName("previewAppSettings"); preferences->setShortcuts({QKeySequence("Ctrl+,"), QKeySequence("Meta+,")});
    application->addAction(tr("Quit Preview"), this, &QWidget::close);
    auto *file = menus->addMenu(tr("File"));
    auto *open = file->addAction(tr("Open…"), this, [this] { openFiles(); }); open->setObjectName("previewAppOpen");
    open->setShortcuts({QKeySequence("Ctrl+O"), QKeySequence("Meta+O")});
    m_recent = file->addMenu(tr("Open Recent")); connect(m_recent, &QMenu::aboutToShow, this, [this] { populateRecent(); });
    auto *exportImage = file->addAction(tr("Export Image…"), this, [this] { m_edits->trigger(FinderQuickActions::Action::ConvertImage, {m_preview->currentUrl()}); });
    exportImage->setObjectName("previewAppExport");
    auto *pdf = file->addAction(tr("Create PDF…"), this, [this] { m_edits->trigger(FinderQuickActions::Action::CreatePdf, m_preview->urls()); }); pdf->setObjectName("previewAppCreatePdf");
    auto *print = file->addAction(tr("Print…"), this, [this] { m_preview->printCurrent(); }); print->setObjectName("previewAppPrint");
    print->setShortcuts({QKeySequence("Ctrl+P"), QKeySequence("Meta+P")}); file->addSeparator();
    auto *close = file->addAction(tr("Close"), this, &QWidget::close); close->setShortcuts({QKeySequence("Ctrl+W"), QKeySequence("Meta+W")});
    auto *edit = menus->addMenu(tr("Edit"));
    auto *undo = edit->addAction(tr("Undo"), this, [this] { auto *manager = KIO::FileUndoManager::self(); manager->uiInterface()->setParentWidget(this); if (manager->isUndoAvailable()) manager->undo(); });
    undo->setObjectName("previewAppUndo"); undo->setShortcuts({QKeySequence("Ctrl+Z"), QKeySequence("Meta+Z")});
    connect(KIO::FileUndoManager::self(), &KIO::FileUndoManager::undoAvailable, undo, &QAction::setEnabled);
    undo->setEnabled(KIO::FileUndoManager::self()->isUndoAvailable());
    auto *copy = edit->addAction(tr("Copy"), this, [this] {
        if (auto *text = qobject_cast<QTextEdit *>(QApplication::focusWidget())) text->copy();
        else if (auto *line = qobject_cast<QLineEdit *>(QApplication::focusWidget())) line->copy();
        else QGuiApplication::clipboard()->setText(m_preview->selectedPdfText());
    }); copy->setShortcuts({QKeySequence("Ctrl+C"), QKeySequence("Meta+C")});
    auto *all = edit->addAction(tr("Select All"), this, [] {
        if (auto *text = qobject_cast<QTextEdit *>(QApplication::focusWidget())) text->selectAll();
        else if (auto *line = qobject_cast<QLineEdit *>(QApplication::focusWidget())) line->selectAll();
    }); all->setShortcuts({QKeySequence("Ctrl+A"), QKeySequence("Meta+A")});
    auto *view = menus->addMenu(tr("View"));
    for (const auto &name : {"previewZoomInAction", "previewZoomOutAction", "previewFitAction", "previewActualSizeAction", "previewWrapTextAction"})
        if (auto *action = m_preview->findChild<QAction *>(name)) view->addAction(action);
    auto *find = view->addAction(tr("Find…"), this, [this] {
        if (m_preview->pdfPageCount() > 0) {
            m_preview->showPdfSearch();
        } else if (auto *text = m_preview->findChild<QTextEdit *>("previewText"); text && text->isVisible()) {
            if (auto *existing = findChild<QDialog *>("previewAppTextFind")) { existing->show(); existing->raise(); return; }
            auto *dialog = new QDialog(this); dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setObjectName("previewAppTextFind"); dialog->setWindowTitle(tr("Find"));
            auto *layout = new QVBoxLayout(dialog); auto *query = new QLineEdit(dialog); query->setObjectName("previewAppTextQuery"); query->setPlaceholderText(tr("Find in document")); layout->addWidget(query);
            auto *message = new QLabel(dialog); layout->addWidget(message);
            auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog); auto *next = buttons->addButton(tr("Next"), QDialogButtonBox::ActionRole); layout->addWidget(buttons);
            QPointer<QTextEdit> target(text); const auto source = m_preview->currentUrl();
            auto search = [this, target, source, query, message] {
                if (!target || m_preview->currentUrl() != source) return;
                if (query->text().isEmpty()) return;
                bool found = target->find(query->text());
                if (!found) { auto cursor = target->textCursor(); cursor.movePosition(QTextCursor::Start); target->setTextCursor(cursor); found = target->find(query->text()); }
                message->setText(found ? QString() : tr("No matches"));
            };
            connect(next, &QPushButton::clicked, dialog, search); connect(query, &QLineEdit::returnPressed, dialog, search);
            connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
            connect(m_preview, &PreviewPane::currentUrlChanged, dialog, [dialog] { dialog->close(); });
            dialog->show(); query->setFocus();
        }
    }); find->setObjectName("previewAppFind"); find->setShortcuts({QKeySequence("Ctrl+F"), QKeySequence("Meta+F")});
    view->addSeparator();
    auto *sidebar = view->addAction(tr("Show Thumbnails")); sidebar->setCheckable(true); sidebar->setObjectName("previewAppThumbnails");
    connect(sidebar, &QAction::toggled, this, [this](bool enabled) {
        m_sidebarOverride = true; m_sidebarEnabled = enabled;
        QSettings().setValue("Viewer/showThumbnails", enabled); updateSidebarVisibility();
    });
    auto *previous = view->addAction(tr("Previous Document"), this, [this] { m_preview->advance(-1); }); previous->setObjectName("previewAppPrevious");
    previous->setShortcuts({QKeySequence("Ctrl+Up"), QKeySequence("Meta+Up")});
    auto *next = view->addAction(tr("Next Document"), this, [this] { m_preview->advance(1); }); next->setObjectName("previewAppNext");
    next->setShortcuts({QKeySequence("Ctrl+Down"), QKeySequence("Meta+Down")});
    view->addAction(m_preview->findChild<QAction *>("previewFullScreenAction"));
    auto *previousPage = view->addAction(tr("Previous Page"), this, [this] { m_preview->setPdfPage(m_preview->pdfCurrentPage() - 1); }); previousPage->setObjectName("previewAppPreviousPage");
    auto *nextPage = view->addAction(tr("Next Page"), this, [this] { m_preview->setPdfPage(m_preview->pdfCurrentPage() + 1); }); nextPage->setObjectName("previewAppNextPage");
    m_tools = menus->addMenu(tr("Tools"));
    auto *markup = m_tools->addAction(tr("Markup Image…"), this, [this] { m_edits->trigger(FinderQuickActions::Action::Markup, {m_preview->currentUrl()}); }); markup->setObjectName("previewAppMarkup");
    auto *rotate = m_tools->addAction(tr("Rotate Left as Copy…"), this, [this] { m_edits->trigger(FinderQuickActions::Action::RotateLeft, {m_preview->currentUrl()}); }); rotate->setObjectName("previewAppRotate");
    connect(m_preview, &PreviewPane::rotateCopyRequested, this, [this](const QUrl &url) { m_edits->trigger(FinderQuickActions::Action::RotateLeft, {url}); });
    connect(m_preview, &PreviewPane::getInfoRequested, this, [this](const QUrl &url) {
        if (url.isEmpty()) return;
        auto *dialog = new KPropertiesDialog(url, this); dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->show();
    });
    file->addAction(m_preview->findChild<QAction *>("previewGetInfoAction"));
    auto *share = file->addMenu(m_preview->shareMenu()); share->setObjectName("previewAppShare");
    auto *localMenu = new QMenu(this); localMenu->setObjectName("previewAppMenu");
    for (auto *action : menus->actions()) localMenu->addAction(action);
    auto tool = [this](QWidget *parent, QAction *action, const QString &symbol, const QString &name) {
        action->setIcon(QIcon(new DocumentIcon(symbol)));
        auto *button = new GlassToolButton(parent); button->setObjectName(name);
        // QToolButton::setDefaultAction(menuAction) followed by setMenu(the
        // same menu) leaves Qt's menu() null. Menu controls own their popup;
        // ordinary commands continue to use native QAction forwarding.
        if (action->menu()) {
            button->setMenu(action->menu()); button->setText(action->text()); button->setToolTip(action->text());
            button->setEnabled(action->isEnabled());
            connect(action,&QAction::changed,button,[button,action] { button->setEnabled(action->isEnabled()); });
        } else button->setDefaultAction(action);
        button->setProperty("documentSymbol", symbol);
        button->setIcon(QIcon(new DocumentIcon(symbol))); button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setIconSize(QSize(22,22)); button->setFixedSize(36,32); button->setFocusPolicy(Qt::StrongFocus);
        button->setAccessibleName(action->text().remove('&')); return button;
    };
    auto *sidebarButton = tool(header, sidebar, "sidebar", "previewAppSidebarButton"); sidebarButton->setProperty("finderGlassSurface", true); chrome->addWidget(sidebarButton);
    auto *titles = new QWidget(header); auto *titleLayout = new QVBoxLayout(titles); titleLayout->setContentsMargins(0,0,0,0); titleLayout->setSpacing(0);
    auto *title = new DocumentTitle(titles); title->setObjectName("previewAppDocumentTitle"); title->setTextFormat(Qt::PlainText); title->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    QFont titleFont = title->font(); titleFont.setWeight(QFont::DemiBold); title->setFont(titleFont); titleLayout->addWidget(title);
    auto *subtitle = new QLabel(titles); subtitle->setObjectName("previewAppDocumentSubtitle"); subtitle->setTextFormat(Qt::PlainText); subtitle->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred); titleLayout->addWidget(subtitle);
    titles->setMinimumWidth(50); chrome->addWidget(titles,1);
    auto *zoom = new GlassCapsule(header); zoom->setObjectName("previewAppZoomGroup"); auto *zoomLayout = new QHBoxLayout(zoom); zoomLayout->setContentsMargins(3,0,3,0); zoomLayout->setSpacing(0);
    zoomLayout->addWidget(tool(zoom,m_preview->findChild<QAction *>("previewZoomOutAction"),"zoomOut","previewAppZoomOutButton"));
    auto *actual = tool(zoom,m_preview->findChild<QAction *>("previewActualSizeAction"),"actual","previewAppActualSizeButton"); zoomLayout->addWidget(actual);
    zoomLayout->addWidget(tool(zoom,m_preview->findChild<QAction *>("previewZoomInAction"),"zoomIn","previewAppZoomInButton")); chrome->addWidget(zoom);
    auto *documentTools = new GlassCapsule(header); documentTools->setObjectName("previewAppDocumentTools"); auto *toolsLayout = new QHBoxLayout(documentTools); toolsLayout->setContentsMargins(3,0,3,0); toolsLayout->setSpacing(0);
    toolsLayout->addWidget(tool(documentTools,markup,"markup","previewAppMarkupButton"));
    toolsLayout->addWidget(tool(documentTools,rotate,"rotate","previewAppRotateButton"));
    auto *shareButton = tool(documentTools,share,"share","previewAppShareButton");
    shareButton->setMenu(m_preview->shareMenu()); shareButton->setPopupMode(QToolButton::InstantPopup); toolsLayout->addWidget(shareButton);
    toolsLayout->addWidget(tool(documentTools,find,"find","previewAppFindButton")); chrome->addWidget(documentTools);
    auto *menuButton = new GlassToolButton(header); menuButton->setObjectName("previewAppMenuButton"); menuButton->setProperty("documentSymbol", "menu"); menuButton->setIcon(QIcon(new DocumentIcon("menu"))); menuButton->setIconSize(QSize(22,22)); menuButton->setFixedSize(36,32);
    menuButton->setAccessibleName(tr("Preview menus")); menuButton->setToolTip(tr("Preview menus")); menuButton->setMenu(localMenu); menuButton->setPopupMode(QToolButton::InstantPopup); menuButton->setProperty("finderGlassSurface",true); chrome->addWidget(menuButton);
    auto *menuShortcut = new QAction(tr("Preview menus"),this); menuShortcut->setShortcut(QKeySequence(Qt::Key_F10)); addAction(menuShortcut);
    connect(menuShortcut, &QAction::triggered,this,[menuButton] { menuButton->showMenu(); });
    auto *windowControls = new WindowControls(header); chrome->addWidget(windowControls);
    auto placeControls = [chrome, windowControls] {
        chrome->removeWidget(windowControls);
        chrome->insertWidget(FinderAppearance::leftWindowControls() ? 0 : chrome->count(), windowControls);
    };
    placeControls(); connect(FinderAppearance::instance(), &FinderAppearance::changed, this, placeControls);
    connect(splitter, &QSplitter::splitterMoved, this, [this] { updateMaterial(); });
    for (const auto &kind : {"minimize", "maximize", "close"}) {
        auto *control = windowControls->findChild<QToolButton *>("window-" + QString(kind));
        connect(control,&QToolButton::clicked,this,[this,kind] {
            if (QString(kind)=="close") this->close(); else if (QString(kind)=="minimize") showMinimized(); else m_preview->setFullScreen(!isFullScreen());
        });
    }
    for (auto *action : findChildren<QAction *>()) addAction(action);
    connect(m_preview,&PreviewPane::viewStateChanged,this,[this] { updateActions(); });
    connect(m_preview, &PreviewPane::quickActionsMenuRequested, this, [this](QMenu *menu, const QList<QUrl> &selection) { menu->clear(); m_edits->populateMenu(menu, selection); });
    connect(m_preview, &PreviewPane::closeRequested, this, &QWidget::close);
    // Space closes transient Quick Look. Standalone documents retain ordinary
    // keyboard scrolling; their only close shortcuts are the File actions.
    m_preview->setProperty("standalonePreview", true);
    // Open remains an explicit native application choice, avoiding recursively
    // relaunching Preview when it is the default image/PDF handler.
    connect(m_preview, &PreviewPane::openRequested, m_preview, &PreviewPane::openWithCurrent);
    connect(m_sidebar, &QListWidget::currentRowChanged, this, [this](int row) {
        const auto list = m_preview->urls();
        if (!m_sidebarPdf.isEmpty()) { if (row >= 0) m_preview->setPdfPage(row); }
        else if (row >= 0 && row < list.size()) m_preview->updateQuickLook(list, list[row]);
    });
    connect(m_preview, &PreviewPane::currentUrlChanged, this, [this](const QUrl &url) {
        rebuildSidebar();
        const QSignalBlocker blocker(m_sidebar); if (m_sidebarPdf.isEmpty()) m_sidebar->setCurrentRow(m_preview->urls().indexOf(url));
        setWindowTitle(url.isEmpty() ? tr("Preview") : url.fileName() + tr(" — Preview")); updateActions();
    });
    connect(m_preview, &PreviewPane::printReadinessChanged, this, [this] { updateActions(); });
    connect(m_preview->findChild<QTextEdit *>("previewText"), &QTextEdit::textChanged, this, [this] { QTimer::singleShot(0, this, [this] { updateActions(); }); });
    connect(m_preview, &PreviewPane::pdfPageChanged, this, [this](int page, int count) {
        if (count > 0) { rebuildSidebar(); const QSignalBlocker blocker(m_sidebar); m_sidebar->setCurrentRow(page); }
        updateActions(); m_thumbnailTimer.start();
    });
    connect(m_preview, &PreviewPane::pdfThumbnailsReady, this, [this](const QUrl &source, const QList<int> &pages, const QList<QImage> &images) {
        if (source != m_sidebarPdf) return;
        m_pageThumbnailsPending = false;
        for (int i = 0; i < pages.size() && i < images.size(); ++i) {
            auto *item = m_sidebar->item(pages[i]); if (!item) continue;
            item->setData(Qt::UserRole + 1, true); if (!images[i].isNull()) item->setIcon(QIcon(QPixmap::fromImage(images[i])));
        }
        const QRect nearby = m_sidebar->viewport()->rect().adjusted(0, -160, 0, 160);
        for (int row = 0; row < m_sidebar->count(); ++row) {
            auto *item = m_sidebar->item(row);
            if (!m_sidebar->visualItemRect(item).intersects(nearby) && item->data(Qt::UserRole + 1).toBool()) {
                item->setIcon(QIcon::fromTheme("application-pdf")); item->setData(Qt::UserRole + 1, false);
            }
        }
        m_thumbnailTimer.start();
    });
    connect(m_preview, &PreviewPane::actionError, this, [this](const QUrl &, const QString &error) { FinderAlert::showError(this, tr("The document couldn’t be opened."), error); });
    connect(m_edits, &QuickActions::busyChanged, this, [this] { updateActions(); });
    connect(m_edits, &QuickActions::statusChanged, this, [this](const QString &text) { m_status->show(); m_status->showMessage(text); });
    connect(m_edits, &QuickActions::errorOccurred, this, [this](const QString &error) { FinderAlert::showError(this, tr("The export couldn’t be completed."), error); });
    connect(m_edits, &QuickActions::outputCreated, this, [this](const QUrl &url) { m_status->show(); m_status->showMessage(tr("Created %1").arg(url.fileName()), 10000); });
    auto restyle = [this] {
        setPalette(FinderAppearance::palette());
        setStyleSheet(QStringLiteral("QMainWindow{background:transparent;} #previewAppSidebar{background:transparent;border:0;} #previewAppSidebar::item{padding:9px;border-radius:7px;}"
            "#previewAppSidebar::item:selected{background:palette(highlight);color:palette(highlighted-text);}"));
    };
    restyle(); connect(FinderAppearance::instance(), &FinderAppearance::changed, this, [this, restyle] { restyle(); for (auto *button : findChildren<QToolButton *>()) { if (button->property("documentSymbol").isValid()) { const QIcon icon(new DocumentIcon(button->property("documentSymbol").toString())); if (button->defaultAction()) button->defaultAction()->setIcon(icon); else button->setIcon(icon); } button->update(); } updateActions(); updateMaterial(); });
    m_thumbnailTimer.setSingleShot(true); m_thumbnailTimer.setInterval(75);
    connect(&m_thumbnailTimer, &QTimer::timeout, this, [this] { loadThumbnails(); });
    connect(m_sidebar->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { m_thumbnailTimer.start(); });
    connect(m_preview, &PreviewPane::printReadinessChanged, this, [this] { m_thumbnailTimer.start(); });
    reloadSettings(); setUrls(urls);
}
void ViewerWindow::reloadSettings() {
    QSettings settings;
    m_preview->setTextWrapping(settings.value("Viewer/wrapText", true).toBool());
    m_sidebarOverride = settings.contains("Viewer/showThumbnails");
    m_sidebarEnabled = settings.value("Viewer/showThumbnails", true).toBool();
    updateSidebarVisibility();
    for (auto *area : findChildren<QAbstractScrollArea *>()) FinderScroll::install(area, FinderScroll::optionsFromPreferences());
}
void ViewerWindow::settingsDialog() {
    if (auto *existing = findChild<QDialog *>("previewAppPreferences")) { existing->raise(); existing->activateWindow(); return; }
    auto *dialog = new QDialog(this); dialog->setObjectName("previewAppPreferences"); dialog->setWindowTitle(tr("Preview Settings")); dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto *form = new QFormLayout(dialog); form->setContentsMargins(24, 20, 24, 20); form->setSpacing(14);
    auto *scale = new QComboBox(dialog); scale->setObjectName("previewAppDefaultScale"); scale->addItem(tr("Fit to Window"), "fit"); scale->addItem(tr("Actual Size"), "actual");
    scale->setCurrentIndex(QSettings().value("Viewer/imageScale", "fit").toString() == "actual" ? 1 : 0); form->addRow(tr("When opening an image:"), scale);
    connect(scale, &QComboBox::currentIndexChanged, dialog, [scale] { QSettings().setValue("Viewer/imageScale", scale->currentData()); });
    for (const auto &entry : QList<QPair<QString, QString>>{{"showThumbnails", tr("Show document thumbnails")}, {"wrapText", tr("Wrap plain text documents")}}) {
        auto *box = new QCheckBox(entry.second, dialog); box->setObjectName("previewAppSetting_" + entry.first); box->setChecked(QSettings().value("Viewer/" + entry.first, true).toBool()); form->addRow(box);
        connect(box, &QCheckBox::toggled, dialog, [this, key = entry.first](bool enabled) { QSettings().setValue("Viewer/" + key, enabled); reloadSettings(); });
    }
    FinderAppearanceControls::addRows(form, dialog);
    auto *note = new QLabel(tr("Image edits and exports create a new file. The original document is preserved."), dialog); note->setWordWrap(true); form->addRow(note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog); form->addRow(buttons); connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close); dialog->open();
}
ViewerWindow::~ViewerWindow() {
    ++m_thumbnailGeneration; m_thumbnailTimer.stop();
    if (m_thumbnailJob) m_thumbnailJob->kill();
    m_edits->cancel();
}
void ViewerWindow::setUrls(const QList<QUrl> &urls) {
    QList<QUrl> accepted; for (const auto &url : urls) if (url.isValid() && !url.isEmpty() && !url.isRelative() && url.password().isEmpty() && !accepted.contains(url) && accepted.size() < 4096) accepted.append(url);
    ++m_thumbnailGeneration; m_sidebarPdf = QUrl(); m_pageThumbnailsPending = false;
    if (m_thumbnailJob) { m_thumbnailJob->kill(); m_thumbnailJob = nullptr; }
    m_thumbnailLoaded.clear();
    const QSignalBlocker blocker(m_sidebar); m_sidebar->clear();
    for (const auto &url : accepted) {
        auto *item = new QListWidgetItem(QIcon::fromTheme(QMimeDatabase().mimeTypeForUrl(url).iconName()), url.fileName(), m_sidebar);
        item->setToolTip(url.toDisplayString(QUrl::RemovePassword)); item->setData(Qt::UserRole, url);
    }
    m_preview->setUrls(accepted); m_sidebar->setCurrentRow(accepted.indexOf(m_preview->currentUrl()));
    QStringList recent = QSettings().value("Viewer/recentUrls").toStringList();
    for (const auto &url : accepted) { const auto key = url.toString(QUrl::FullyEncoded); recent.removeAll(key); recent.prepend(key); }
    while (recent.size() > 20) recent.removeLast();
    QSettings().setValue("Viewer/recentUrls", recent);
    rebuildSidebar(); updateSidebarVisibility(); updateActions(); m_thumbnailTimer.start();
}
QList<QUrl> ViewerWindow::urls() const { return m_preview->urls(); }
PreviewPane *ViewerWindow::preview() const { return m_preview; }
void ViewerWindow::openFiles() {
    auto *dialog = new QFileDialog(this, tr("Open Documents")); dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setFileMode(QFileDialog::ExistingFiles); dialog->setDirectoryUrl(m_preview->currentUrl().adjusted(QUrl::RemoveFilename));
    connect(dialog, &QFileDialog::urlsSelected, this, [this](const QList<QUrl> &urls) { setUrls(urls); }); dialog->open();
}
void ViewerWindow::populateRecent() {
    m_recent->clear();
    for (const auto &value : QSettings().value("Viewer/recentUrls").toStringList()) {
        const QUrl url(value); if (!url.isValid() || url.isRelative() || !url.password().isEmpty()) continue;
        m_recent->addAction(url.fileName(), this, [this, url] { setUrls({url}); });
    }
    m_recent->addSeparator(); m_recent->addAction(tr("Clear Menu"), this, [] { QSettings().remove("Viewer/recentUrls"); });
}
void ViewerWindow::updateActions() {
    const auto list = urls(); const auto current = m_preview->currentUrl(); const int row = list.indexOf(current);
    if (auto *title = dynamic_cast<DocumentTitle *>(findChild<QLabel *>("previewAppDocumentTitle"))) title->setTitle(current.isEmpty() ? tr("Preview") : current.fileName());
    auto *subtitle = findChild<QLabel *>("previewAppDocumentSubtitle");
    if (m_preview->pdfPageCount() > 0) subtitle->setText(tr("Page %1 of %2").arg(m_preview->pdfCurrentPage()+1).arg(m_preview->pdfPageCount()));
    else if (m_preview->imageScale() > 0) subtitle->setText(tr("%1%").arg(qRound(m_preview->imageScale()*100)));
    else subtitle->setText(list.size()>1 ? tr("Document %1 of %2").arg(row+1).arg(list.size()) : QString());
    QPalette subtitlePalette = palette(); subtitlePalette.setColor(QPalette::WindowText,FinderAppearance::color(FinderAppearance::SecondaryText)); subtitle->setPalette(subtitlePalette);
    findChild<QAction *>("previewAppPreviousPage")->setEnabled(m_preview->pdfCurrentPage()>0);
    findChild<QAction *>("previewAppNextPage")->setEnabled(m_preview->pdfPageCount()>0 && m_preview->pdfCurrentPage()+1<m_preview->pdfPageCount());
    findChild<QAction *>("previewAppShare")->setEnabled(!current.isEmpty());
    auto *actualButton = findChild<QToolButton *>("previewAppActualSizeButton");
    actualButton->setDefaultAction(m_preview->findChild<QAction *>(m_preview->pdfPageCount()>0 ? "previewFitAction" : "previewActualSizeAction"));
    actualButton->setIcon(QIcon(new DocumentIcon("actual")));
    actualButton->setAccessibleName(actualButton->defaultAction()->text());
    updateSidebarVisibility();
    findChild<QAction *>("previewAppPrevious")->setEnabled(row > 0);
    findChild<QAction *>("previewAppNext")->setEnabled(row >= 0 && row + 1 < list.size());
    findChild<QAction *>("previewAppPrint")->setEnabled(m_preview->canPrintCurrent());
    auto *text = m_preview->findChild<QTextEdit *>("previewText");
    findChild<QAction *>("previewAppFind")->setEnabled(m_preview->pdfPageCount() > 0 || (text && text->isVisible() && !text->toPlainText().isEmpty()));
    // PDF scroll/page callbacks update UI frequently. Cache capability checks
    // by source/list, so they never rescan every document during scrolling.
    if (m_capabilitySource != m_preview->currentUrl()) {
        m_capabilitySource = m_preview->currentUrl(); m_canExportImage = false;
        for (const auto &capability : QuickActions::actionsForUrls({m_capabilitySource}))
            if (capability.action == FinderQuickActions::Action::ConvertImage) m_canExportImage = capability.enabled;
    }
    if (m_capabilitySelection != list) {
        m_capabilitySelection = list; m_canCreatePdf = false;
        for (const auto &capability : QuickActions::actionsForUrls(list))
            if (capability.action == FinderQuickActions::Action::CreatePdf) m_canCreatePdf = capability.enabled;
    }
    findChild<QAction *>("previewAppMarkup")->setEnabled(m_canExportImage && !m_edits->busy());
    findChild<QAction *>("previewAppRotate")->setEnabled(m_canExportImage && !m_edits->busy());
    findChild<QAction *>("previewAppExport")->setEnabled(m_canExportImage && !m_edits->busy());
    findChild<QAction *>("previewAppCreatePdf")->setEnabled(m_canCreatePdf && !m_edits->busy());
}
void ViewerWindow::dragEnterEvent(QDragEnterEvent *event) { if (event->mimeData()->hasUrls()) event->acceptProposedAction(); }
void ViewerWindow::dropEvent(QDropEvent *event) { if (event->mimeData()->hasUrls()) { setUrls(event->mimeData()->urls()); event->acceptProposedAction(); } }

void ViewerWindow::rebuildSidebar() {
    const QUrl pdf = m_preview->pdfPageCount() > 0 ? m_preview->currentUrl() : QUrl();
    if (pdf == m_sidebarPdf) return;
    ++m_thumbnailGeneration; if (m_thumbnailJob) { m_thumbnailJob->kill(); m_thumbnailJob = nullptr; }
    m_sidebarPdf = pdf; m_pageThumbnailsPending = false; m_thumbnailLoaded.clear();
    const QSignalBlocker blocker(m_sidebar); m_sidebar->clear();
    if (!pdf.isEmpty()) {
        m_sidebar->setAccessibleName(tr("PDF page thumbnails"));
        for (int page = 0; page < m_preview->pdfPageCount(); ++page) {
            auto *item = new QListWidgetItem(QIcon::fromTheme("application-pdf"), tr("Page %1").arg(page + 1), m_sidebar);
            item->setData(Qt::UserRole, pdf); item->setData(Qt::UserRole + 2, page);
        }
        m_sidebar->setCurrentRow(m_preview->pdfCurrentPage());
    } else {
        m_sidebar->setAccessibleName(tr("Open documents"));
    m_sidebar->setAutoFillBackground(false); m_sidebar->viewport()->setAutoFillBackground(false);
        for (const auto &url : urls()) {
            auto *item = new QListWidgetItem(QIcon::fromTheme(QMimeDatabase().mimeTypeForUrl(url).iconName()), url.fileName(), m_sidebar);
            item->setData(Qt::UserRole, url); item->setToolTip(url.toDisplayString(QUrl::RemovePassword));
        }
        m_sidebar->setCurrentRow(urls().indexOf(m_preview->currentUrl()));
    }
    m_thumbnailTimer.start();
}
void ViewerWindow::loadThumbnails() {
    if (!isVisible() || !m_sidebar->isVisible() || m_thumbnailJob) return;
    if (!m_sidebarPdf.isEmpty()) {
        if (m_pageThumbnailsPending) return;
        QList<int> pages; const QRect visible = m_sidebar->viewport()->rect().adjusted(0, -80, 0, 80);
        for (int row = 0; row < m_sidebar->count() && pages.size() < 8; ++row)
            if (!m_sidebar->item(row)->data(Qt::UserRole + 1).toBool() && m_sidebar->visualItemRect(m_sidebar->item(row)).intersects(visible)) pages.append(row);
        if (!pages.isEmpty()) { m_pageThumbnailsPending = true; m_preview->requestPdfThumbnails(pages); }
        return;
    }
    KFileItemList files; const QRect visible = m_sidebar->viewport()->rect().adjusted(0, -80, 0, 80);
    for (int row = 0; row < m_sidebar->count() && files.size() < 8; ++row) {
        auto *item = m_sidebar->item(row); const QUrl url = item->data(Qt::UserRole).toUrl();
        if (!url.isLocalFile() || m_thumbnailLoaded.contains(url) || !m_sidebar->visualItemRect(item).intersects(visible)) continue;
        files.append(KFileItem(url)); m_thumbnailLoaded.insert(url);
    }
    if (files.isEmpty()) return;
    const auto generation = m_thumbnailGeneration;
    const auto plugins = KIO::PreviewJob::availablePlugins();
    auto *job = KIO::filePreview(files, QSize(128, 128), &plugins); m_thumbnailJob = job;
    job->setIgnoreMaximumSize(false);
    connect(job, &KIO::PreviewJob::gotPreview, this, [this, generation](const KFileItem &file, const QPixmap &pixmap) {
        if (generation != m_thumbnailGeneration) return;
        for (int row = 0; row < m_sidebar->count(); ++row) {
            auto *item = m_sidebar->item(row);
            if (item->data(Qt::UserRole).toUrl() == file.url()) { item->setIcon(QIcon(pixmap)); item->setData(Qt::UserRole + 1, true); break; }
        }
        // Retain decoded thumbnails only near the viewport, bounded to 64.
        if (m_thumbnailLoaded.size() > 64) {
            const QRect visible = m_sidebar->viewport()->rect().adjusted(0, -160, 0, 160);
            for (int row = 0; row < m_sidebar->count(); ++row) {
                auto *item = m_sidebar->item(row); const QUrl url = item->data(Qt::UserRole).toUrl();
                if (!m_sidebar->visualItemRect(item).intersects(visible) && item->data(Qt::UserRole + 1).toBool()) {
                    item->setIcon(QIcon::fromTheme(QMimeDatabase().mimeTypeForUrl(url).iconName())); item->setData(Qt::UserRole + 1, false); m_thumbnailLoaded.remove(url);
                }
            }
        }
    });
    connect(job, &KJob::result, this, [this, job, generation] {
        if (generation != m_thumbnailGeneration || m_thumbnailJob != job) return;
        m_thumbnailJob = nullptr; m_thumbnailTimer.start();
    });
}

void ViewerWindow::updateSidebarVisibility() {
    const bool visible = m_sidebarOverride ? m_sidebarEnabled : (urls().size()>1 || m_preview->pdfPageCount()>0);
    const QSignalBlocker blocker(findChild<QAction *>("previewAppThumbnails"));
    findChild<QAction *>("previewAppThumbnails")->setChecked(visible);
    const bool wasHidden = m_sidebar->isHidden();
    m_sidebar->setVisible(visible);
    if (wasHidden != !visible) QTimer::singleShot(0, this, [this] { updateMaterial(); });
    if (visible) m_thumbnailTimer.start();
}
void ViewerWindow::resizeEvent(QResizeEvent *event) {
    QMainWindow::resizeEvent(event);
    if (auto *tools = findChild<QWidget *>("previewAppDocumentTools")) tools->setVisible(width()>=760);
    updateMaterial();
}

void ViewerWindow::updateMaterial() {
    auto *header = static_cast<ViewerHeader *>(findChild<QWidget *>("previewAppHeader"));
    auto *splitter = static_cast<ViewerSplitter *>(findChild<QSplitter *>("previewAppSplitter"));
    if (!header || !splitter || !m_sidebar) return;
    const bool available = SidebarMaterial::supported(this);
    header->blur = splitter->blur = available;
    header->sidebarWidth = m_sidebar->isVisible() ? m_sidebar->width() : 0;
    header->update(); splitter->update();
    QRegion region;
    if (m_sidebar->isVisible()) {
        region += QRegion(m_sidebar->rect().translated(m_sidebar->mapTo(this,QPoint())));
        region += QRegion(QRect(header->mapTo(this,QPoint()),QSize(header->sidebarWidth,header->height())));
    }
    m_material.update(this,region,available);
}
bool ViewerWindow::event(QEvent *event) {
    const bool result = QMainWindow::event(event);
    if (event->type()==QEvent::Show || event->type()==QEvent::WindowStateChange || event->type()==QEvent::WinIdChange)
        QTimer::singleShot(0,this,[this] {updateMaterial();});
    return result;
}
