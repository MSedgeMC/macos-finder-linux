// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#include "viewer.h"
#include "preview.h"
#include "quickactions.h"
#include "appearance.h"
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

ViewerWindow::ViewerWindow(const QList<QUrl> &urls, QWidget *parent) : QMainWindow(parent) {
    setObjectName("finderPreviewApp"); setWindowTitle(tr("Preview")); setWindowIcon(QIcon::fromTheme("image-x-generic"));
    resize(960, 700); setMinimumSize(480, 320); setAcceptDrops(true);
    auto *splitter = new QSplitter(this); splitter->setObjectName("previewAppSplitter"); setCentralWidget(splitter);
    m_sidebar = new QListWidget(splitter); m_sidebar->setObjectName("previewAppSidebar");
    m_sidebar->setAccessibleName(tr("Open documents")); m_sidebar->setIconSize(QSize(64, 64));
    m_sidebar->setMinimumWidth(100); m_sidebar->setMaximumWidth(260); m_sidebar->setWordWrap(true);
    m_preview = new PreviewPane(splitter); m_preview->setHostWindowControls(true); m_preview->setHostedQuickLook(true);
    splitter->setStretchFactor(0, 0); splitter->setStretchFactor(1, 1); splitter->setSizes({166, 794});
    m_edits = new QuickActions(this);
    auto *file = menuBar()->addMenu(tr("File"));
    auto *open = file->addAction(tr("Open…"), this, [this] { openFiles(); }); open->setObjectName("previewAppOpen");
    open->setShortcuts({QKeySequence("Ctrl+O"), QKeySequence("Meta+O")});
    m_recent = file->addMenu(tr("Open Recent")); connect(m_recent, &QMenu::aboutToShow, this, [this] { populateRecent(); });
    auto *exportImage = file->addAction(tr("Export Image…"), this, [this] { m_edits->trigger(FinderQuickActions::Action::ConvertImage, {m_preview->currentUrl()}); });
    exportImage->setObjectName("previewAppExport");
    auto *pdf = file->addAction(tr("Create PDF…"), this, [this] { m_edits->trigger(FinderQuickActions::Action::CreatePdf, m_preview->urls()); }); pdf->setObjectName("previewAppCreatePdf");
    auto *print = file->addAction(tr("Print…"), this, [this] { m_preview->printCurrent(); }); print->setObjectName("previewAppPrint");
    print->setShortcuts({QKeySequence("Ctrl+P"), QKeySequence("Meta+P")}); file->addSeparator();
    auto *close = file->addAction(tr("Close"), this, &QWidget::close); close->setShortcuts({QKeySequence("Ctrl+W"), QKeySequence("Meta+W")});
    auto *edit = menuBar()->addMenu(tr("Edit"));
    auto *undo = edit->addAction(tr("Undo"), this, [this] { KIO::FileUndoManager::self()->undo(this); });
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
    auto *view = menuBar()->addMenu(tr("View"));
    for (const auto &name : {"previewZoomInAction", "previewZoomOutAction", "previewFitAction", "previewActualSizeAction", "previewWrapTextAction"})
        if (auto *action = m_preview->findChild<QAction *>(name)) view->addAction(action);
    auto *find = view->addAction(tr("Find…"), this, [this] {
        if (m_preview->pdfPageCount() > 0) {
            auto *search = m_preview->findChild<QLineEdit *>("previewPdfSearch"); search->setFocus(); search->selectAll();
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
    m_tools = menuBar()->addMenu(tr("Tools"));
    connect(m_tools, &QMenu::aboutToShow, this, [this] { m_tools->clear(); m_edits->populateMenu(m_tools, {m_preview->currentUrl()}); });
    connect(m_preview, &PreviewPane::quickActionsMenuRequested, this, [this](QMenu *menu, const QList<QUrl> &selection) { menu->clear(); m_edits->populateMenu(menu, selection); });
    connect(m_preview, &PreviewPane::closeRequested, this, &QWidget::close);
    // Space closes transient Quick Look. Standalone documents retain ordinary
    // keyboard scrolling; their only close shortcuts are the File actions.
    m_preview->setProperty("standalonePreview", true);
    connect(m_sidebar, &QListWidget::currentRowChanged, this, [this](int row) {
        const auto list = m_preview->urls(); if (row >= 0 && row < list.size()) m_preview->updateQuickLook(list, list[row]);
    });
    connect(m_preview, &PreviewPane::currentUrlChanged, this, [this](const QUrl &url) {
        const QSignalBlocker blocker(m_sidebar); m_sidebar->setCurrentRow(m_preview->urls().indexOf(url));
        setWindowTitle(url.isEmpty() ? tr("Preview") : url.fileName() + tr(" — Preview")); updateActions();
    });
    connect(m_preview, &PreviewPane::printReadinessChanged, this, [this] { updateActions(); });
    connect(m_preview, &PreviewPane::pdfPageChanged, this, [this] { updateActions(); });
    connect(m_preview, &PreviewPane::actionError, this, [this](const QUrl &, const QString &error) { FinderAlert::showError(this, tr("The document couldn’t be opened."), error); });
    connect(m_edits, &QuickActions::busyChanged, this, [this] { updateActions(); });
    connect(m_edits, &QuickActions::statusChanged, this, [this](const QString &text) { statusBar()->showMessage(text); });
    connect(m_edits, &QuickActions::errorOccurred, this, [this](const QString &error) { FinderAlert::showError(this, tr("The export couldn’t be completed."), error); });
    connect(m_edits, &QuickActions::outputCreated, this, [this](const QUrl &url) { statusBar()->showMessage(tr("Created %1").arg(url.fileName()), 10000); });
    auto restyle = [this] {
        setPalette(FinderAppearance::palette());
        setStyleSheet(QStringLiteral("#previewAppSidebar{background:palette(window);border:0;} #previewAppSidebar::item{padding:9px;border-radius:7px;}"
            "#previewAppSidebar::item:selected{background:palette(highlight);color:palette(highlighted-text);} QMenu{background:palette(window);color:palette(window-text);padding:6px;}"));
    };
    restyle(); connect(FinderAppearance::instance(), &FinderAppearance::changed, this, restyle);
    m_thumbnailTimer.setSingleShot(true); m_thumbnailTimer.setInterval(75);
    connect(&m_thumbnailTimer, &QTimer::timeout, this, [this] { loadThumbnails(); });
    connect(m_sidebar->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { m_thumbnailTimer.start(); });
    connect(m_preview, &PreviewPane::printReadinessChanged, this, [this] { m_thumbnailTimer.start(); });
    setUrls(urls);
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
    findChild<QAction *>("previewAppFind")->setEnabled(m_preview->pdfPageCount() > 0);
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
