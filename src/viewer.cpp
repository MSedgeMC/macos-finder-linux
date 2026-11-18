// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#include "viewer.h"
#include "preview.h"
#include "quickactions.h"
#include "appearance.h"
#include "windowcontour.h"
#include "windowcontrols.h"
#include "windowframe.h"
#include "gestures.h"
#include "scroll.h"
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
#include <KIO/PreviewJob>
#include <KFileItem>
#include <KIO/FileUndoManager>

namespace {
class ViewerHeader final : public QWidget {
public: using QWidget::QWidget;
protected:
    void paintEvent(QPaintEvent *) override { QPainter painter(this); painter.fillRect(rect(), FinderAppearance::color(FinderAppearance::Header)); }
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
    auto *header = new ViewerHeader(base); header->setFixedHeight(40); header->setObjectName("previewAppHeader");
    auto *chrome = new QHBoxLayout(header); chrome->setContentsMargins(12, 4, 12, 4); chrome->setSpacing(7);
    layout->addWidget(header);
    auto *splitter = new QSplitter(base); splitter->setObjectName("previewAppSplitter"); layout->addWidget(splitter, 1);
    m_sidebar = new QListWidget(splitter); m_sidebar->setObjectName("previewAppSidebar");
    m_sidebar->setAccessibleName(tr("Open documents")); m_sidebar->setIconSize(QSize(64, 64));
    m_sidebar->setMinimumWidth(100); m_sidebar->setMaximumWidth(260); m_sidebar->setWordWrap(true);
    m_preview = new PreviewPane(splitter); m_preview->setHostWindowControls(true); m_preview->setHostedQuickLook(true);
    for (const auto &kind : {"close", "minimize", "maximize"}) {
        auto *control = new WindowControl(kind, header); chrome->addWidget(control);
        connect(control, &QToolButton::clicked, this, [this, kind] {
            if (QString(kind) == "close") close(); else if (QString(kind) == "minimize") showMinimized(); else m_preview->setFullScreen(!isFullScreen());
        });
    }
    chrome->addWidget(m_preview->quickLookHeaderWidget(), 1);
    // Keep the app menus available on Linux inside the same clipped surface.
    auto *menus = new QMenuBar(base); menus->setNativeMenuBar(false); layout->insertWidget(1, menus);
    FinderWindowFrame::install(this);
    auto *gestures = FinderGestures::Controller::install(this); gestures->setZoomTarget(m_preview);
    connect(gestures, &FinderGestures::Controller::zoomRequested, this, [this](QWidget *, qreal factor, const QPointF &) { m_preview->setZoomFactor(m_preview->zoomFactor() * factor); });
    splitter->setStretchFactor(0, 0); splitter->setStretchFactor(1, 1); splitter->setSizes({166, 794});
    m_edits = new QuickActions(this);
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
            auto *search = m_preview->findChild<QLineEdit *>("previewPdfSearch"); search->setFocus(); search->selectAll();
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
    auto *sidebar = view->addAction(tr("Show Thumbnails")); sidebar->setCheckable(true); sidebar->setChecked(true); sidebar->setObjectName("previewAppThumbnails");
    connect(sidebar, &QAction::toggled, m_sidebar, &QWidget::setVisible);
    auto *previous = view->addAction(tr("Previous Document"), this, [this] { m_preview->advance(-1); }); previous->setObjectName("previewAppPrevious");
    previous->setShortcuts({QKeySequence("Ctrl+Up"), QKeySequence("Meta+Up")});
    auto *next = view->addAction(tr("Next Document"), this, [this] { m_preview->advance(1); }); next->setObjectName("previewAppNext");
    next->setShortcuts({QKeySequence("Ctrl+Down"), QKeySequence("Meta+Down")});
    view->addAction(m_preview->findChild<QAction *>("previewFullScreenAction"));
    m_tools = menus->addMenu(tr("Tools"));
    connect(m_tools, &QMenu::aboutToShow, this, [this] { m_tools->clear(); m_edits->populateMenu(m_tools, {m_preview->currentUrl()}); });
    connect(m_preview, &PreviewPane::quickActionsMenuRequested, this, [this](QMenu *menu, const QList<QUrl> &selection) { menu->clear(); m_edits->populateMenu(menu, selection); });
    connect(m_preview, &PreviewPane::closeRequested, this, &QWidget::close);
    // Space closes transient Quick Look. Standalone documents retain ordinary
    // keyboard scrolling; their only close shortcuts are the File actions.
    m_preview->setProperty("standalonePreview", true);
    // Open remains an explicit native application choice, avoiding recursively
    // relaunching Preview when it is the default image/PDF handler.
    connect(m_preview, &PreviewPane::openRequested, m_preview, &PreviewPane::openWithCurrent);
    connect(m_sidebar, &QListWidget::currentRowChanged, this, [this](int row) {
        const auto list = m_preview->urls(); if (row >= 0 && row < list.size()) m_preview->updateQuickLook(list, list[row]);
    });
    connect(m_preview, &PreviewPane::currentUrlChanged, this, [this](const QUrl &url) {
        const QSignalBlocker blocker(m_sidebar); m_sidebar->setCurrentRow(m_preview->urls().indexOf(url));
        setWindowTitle(url.isEmpty() ? tr("Preview") : url.fileName() + tr(" — Preview")); updateActions();
    });
    connect(m_preview, &PreviewPane::printReadinessChanged, this, [this] { updateActions(); });
    connect(m_preview->findChild<QTextEdit *>("previewText"), &QTextEdit::textChanged, this, [this] { QTimer::singleShot(0, this, [this] { updateActions(); }); });
    connect(m_preview, &PreviewPane::pdfPageChanged, this, [this] { updateActions(); });
    connect(m_preview, &PreviewPane::actionError, this, [this](const QUrl &, const QString &error) { FinderAlert::showError(this, tr("The document couldn’t be opened."), error); });
    connect(m_edits, &QuickActions::busyChanged, this, [this] { updateActions(); });
    connect(m_edits, &QuickActions::statusChanged, this, [this](const QString &text) { statusBar()->showMessage(text); });
    connect(m_edits, &QuickActions::errorOccurred, this, [this](const QString &error) { FinderAlert::showError(this, tr("The export couldn’t be completed."), error); });
    connect(m_edits, &QuickActions::outputCreated, this, [this](const QUrl &url) { statusBar()->showMessage(tr("Created %1").arg(url.fileName()), 10000); });
    auto restyle = [this] {
        setPalette(FinderAppearance::palette());
        setStyleSheet(QStringLiteral("QMainWindow{background:transparent;} QMenuBar{background:palette(window);color:palette(window-text);border:0;padding:2px;} QMenuBar::item{padding:4px 10px;} #previewAppSidebar{background:palette(window);border:0;} #previewAppSidebar::item{padding:9px;border-radius:7px;}"
            "#previewAppSidebar::item:selected{background:palette(highlight);color:palette(highlighted-text);} QMenu{background:palette(window);color:palette(window-text);padding:6px;}"));
    };
    restyle(); connect(FinderAppearance::instance(), &FinderAppearance::changed, this, restyle);
    m_thumbnailTimer.setSingleShot(true); m_thumbnailTimer.setInterval(75);
    connect(&m_thumbnailTimer, &QTimer::timeout, this, [this] { loadThumbnails(); });
    connect(m_sidebar->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { m_thumbnailTimer.start(); });
    connect(m_preview, &PreviewPane::printReadinessChanged, this, [this] { m_thumbnailTimer.start(); });
    reloadSettings(); setUrls(urls);
}
void ViewerWindow::reloadSettings() {
    QSettings settings;
    m_preview->setTextWrapping(settings.value("Viewer/wrapText", true).toBool());
    auto *sidebar = findChild<QAction *>("previewAppThumbnails"); sidebar->setChecked(settings.value("Viewer/showThumbnails", true).toBool());
    m_sidebar->setVisible(sidebar->isChecked());
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
    ++m_thumbnailGeneration;
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
    while (recent.size() > 20) recent.removeLast(); QSettings().setValue("Viewer/recentUrls", recent);
    updateActions(); m_thumbnailTimer.start();
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
    const auto list = urls(); const int row = list.indexOf(m_preview->currentUrl());
    findChild<QAction *>("previewAppPrevious")->setEnabled(row > 0);
    findChild<QAction *>("previewAppNext")->setEnabled(row >= 0 && row + 1 < list.size());
    findChild<QAction *>("previewAppPrint")->setEnabled(m_preview->canPrintCurrent());
    auto *text = m_preview->findChild<QTextEdit *>("previewText");
    findChild<QAction *>("previewAppFind")->setEnabled(m_preview->pdfPageCount() > 0 || (text && text->isVisible() && !text->toPlainText().isEmpty()));
    const auto capabilities = QuickActions::actionsForUrls({m_preview->currentUrl()});
    bool image = false; for (const auto &capability : capabilities) if (capability.action == FinderQuickActions::Action::ConvertImage) image = capability.enabled;
    findChild<QAction *>("previewAppExport")->setEnabled(image && !m_edits->busy());
    bool pdf = false; for (const auto &capability : QuickActions::actionsForUrls(list)) if (capability.action == FinderQuickActions::Action::CreatePdf) pdf = capability.enabled;
    findChild<QAction *>("previewAppCreatePdf")->setEnabled(pdf && !m_edits->busy());
}
void ViewerWindow::dragEnterEvent(QDragEnterEvent *event) { if (event->mimeData()->hasUrls()) event->acceptProposedAction(); }
void ViewerWindow::dropEvent(QDropEvent *event) { if (event->mimeData()->hasUrls()) { setUrls(event->mimeData()->urls()); event->acceptProposedAction(); } }

void ViewerWindow::loadThumbnails() {
    if (!isVisible() || !m_sidebar->isVisible() || m_thumbnailJob) return;
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
