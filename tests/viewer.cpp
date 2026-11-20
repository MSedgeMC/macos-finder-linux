// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#include "viewer.h"
#include "preview.h"
#include "appearance.h"
#include <QApplication>
#include <QAction>
#include <QListWidget>
#include <QTemporaryDir>
#include <QImage>
#include <QPainter>
#include <QPdfWriter>
#include <QFile>
#include <QTextEdit>
#include <QKeyEvent>
#include <QElapsedTimer>
#include <QThread>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QSettings>
#include <QDialog>
#include <QLabel>
#include <QMenuBar>
#include <QToolButton>
#include <QLineEdit>
#include <QSplitter>
#include <QScrollBar>
#include <iostream>
#include <functional>
namespace {
void check(bool condition, const char *message) { if (!condition) { std::cerr << "FAIL " << message << '\n'; std::exit(1); } }
bool waitFor(const std::function<bool()> &condition) {
    QElapsedTimer time; time.start(); while (!condition() && time.elapsed() < 10000) { QApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(1); } return condition();
}
}
int main(int argc, char **argv) {
    QApplication app(argc, argv); app.setOrganizationName("FinderViewerTests"); app.setApplicationName("Finder"); FinderAppearance::apply(&app);
    QTemporaryDir fixture; check(fixture.isValid(), "private fixtures");
    const QUrl imageUrl = QUrl::fromLocalFile(fixture.filePath("A very long image filename that needs an accessible elided label.png"));
    const QUrl pdfUrl = QUrl::fromLocalFile(fixture.filePath("Document.pdf"));
    const QUrl textUrl = QUrl::fromLocalFile(fixture.filePath("Readme.txt"));
    QImage image(600, 400, QImage::Format_RGB32); image.fill(Qt::cyan); check(image.save(imageUrl.toLocalFile()), "image fixture");
    { QPdfWriter pdf(pdfUrl.toLocalFile()); QPainter painter(&pdf); painter.drawText(QPoint(100, 100), "Viewer searchable document"); check(pdf.newPage(), "PDF second page"); painter.drawText(QPoint(100, 100), "Second page"); }
    { QFile text(textUrl.toLocalFile()); check(text.open(QIODevice::WriteOnly), "text fixture"); text.write("Readable standalone document\n"); }
    ViewerWindow window({imageUrl, pdfUrl, textUrl}); window.show();
    auto *pane = window.preview(); auto *sidebar = window.findChild<QListWidget *>("previewAppSidebar");
    check(sidebar && sidebar->count() == 3 && waitFor([&] { return pane->originalImageSize() == image.size(); }), "standalone opens an image with ordered documents");
    check(pane->documentPresentation() && !pane->hostedQuickLook() && pane->quickLookHeaderWidget()->isHidden(), "document host keeps Quick Look chrome hidden");
    check(window.menuBar()->isHidden() && window.findChild<QToolButton *>("previewAppMenuButton")->menu(), "global menus with persistent local fallback and no duplicate menu row");
    auto *title = window.findChild<QLabel *>("previewAppDocumentTitle");
    check(window.findChild<QToolButton *>("previewAppShareButton")->menu() == pane->shareMenu(), "Share button opens actual copy/email menu without requiring an uninstalled provider");
    check(title && title->toolTip() == imageUrl.fileName(), "leading document title preserves full accessible name");
    window.findChild<QToolButton *>("previewAppZoomInButton")->click(); check(pane->zoomFactor()>1, "grouped zoom button invokes real image zoom");
    pane->actualSize(); check(pane->imageScale() == 1, "standalone actual size");
    pane->setZoomFactor(16); check(!window.findChild<QToolButton *>("previewAppZoomInButton")->icon().isNull(), "action capability changes preserve vector toolbar symbols");
    pane->actualSize();
    check(waitFor([&] { return sidebar->item(0)->data(Qt::UserRole + 1).toBool(); }), "native image thumbnail delivered");
    auto *splitter = window.findChild<QSplitter *>("previewAppSplitter");
    splitter->setSizes({100, 860}); QApplication::processEvents();
    check(sidebar->horizontalScrollBar()->maximum() == 0 && sidebar->visualItemRect(sidebar->item(0)).right() < sidebar->viewport()->width(), "narrow sidebar retains thumbnail and long filename without horizontal scrolling");
    splitter->setSizes({220, 740}); QApplication::processEvents();
    check(sidebar->visualItemRect(sidebar->item(0)).width() > 150, "thumbnail layout follows native sidebar resizing");
    QKeyEvent space(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier); QApplication::sendEvent(pane, &space);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier); QApplication::sendEvent(pane, &escape);
    check(window.isVisible() && pane->isVisible(), "standalone Space and Escape retain the document window");
    window.findChild<QAction *>("previewAppNext")->trigger();
    check(waitFor([&] { return pane->pdfPageCount() == 2; }) && pane->currentUrl() == pdfUrl && sidebar->count() == 2 && sidebar->currentRow() == 0, "Next opens PDF page thumbnails");
    check(waitFor([&] { return sidebar->item(1)->data(Qt::UserRole + 1).toBool(); }), "PDF page thumbnails decode asynchronously");
    sidebar->setCurrentRow(1); check(pane->pdfCurrentPage() == 1, "PDF thumbnail activates its real page");
    check(window.findChild<QAction *>("previewAppFind")->isEnabled(), "PDF Find uses document capability");
    check(window.findChild<QLabel *>("previewAppDocumentSubtitle")->text()=="Page 2 of 2", "document title follows PDF page selection");
    check(pane->findChild<QWidget *>("previewPdfControls")->isHidden(), "PDF begins without permanent bottom search/navigation chrome");
    window.findChild<QAction *>("previewAppFind")->trigger();
    check(pane->findChild<QLineEdit *>("previewPdfSearch")->isVisible(), "PDF Find reveals functional search on demand");
    pane->showPdfSearch(false);
    pane->searchPdf("Viewer");
    window.findChild<QAction *>("previewAppNext")->trigger();
    auto *text = pane->findChild<QTextEdit *>("previewText");
    check(waitFor([&] { return text->toPlainText().contains("Readable standalone document"); }) && sidebar->currentRow() == 2, "text opens asynchronously and advances exact URL");
    check(waitFor([&] { return !window.findChild<QAction *>("previewAppNext")->isEnabled() && window.findChild<QAction *>("previewAppFind")->isEnabled(); }), "last item and text Find capabilities");
    window.findChild<QAction *>("previewAppFind")->trigger();
    auto *find = window.findChild<QDialog *>("previewAppTextFind"); check(find, "text Find opens native panel"); find->close();
    window.findChild<QAction *>("previewAppSettings")->trigger();
    auto *preferences = window.findChild<QDialog *>("previewAppPreferences"); check(preferences, "standalone Settings panel"); preferences->close();
    window.findChild<QAction *>("previewAppThumbnails")->trigger(); check(sidebar->isHidden(), "thumbnail sidebar toggles");
    sidebar->setCurrentRow(0); check(waitFor([&] { return pane->originalImageSize() == image.size(); }), "sidebar primary selection follows image");
    QMimeData mime; mime.setUrls({pdfUrl}); QDragEnterEvent enter(QPoint(50, 50), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &enter); check(enter.isAccepted(), "native file drag accepted");
    QDropEvent drop(QPointF(50, 50), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(&window, &drop);
    check(drop.isAccepted() && waitFor([&] { return pane->pdfPageCount() == 2; }) && window.urls() == QList<QUrl>{pdfUrl}, "drop replaces list and rejects old results");
    check(QSettings().value("Viewer/recentUrls").toStringList().contains(pdfUrl.toString(QUrl::FullyEncoded)), "recent documents persisted");
    if (argc > 1) check(window.grab().save(QString::fromLocal8Bit(argv[1])), "standalone screenshot");
    window.setUrls({QUrl("relative.png"), imageUrl, imageUrl}); check(window.urls() == QList<QUrl>{imageUrl}, "exact URL identity and invalid inputs");
    QSettings().remove("Viewer/showThumbnails");
    { ViewerWindow single({imageUrl}); single.show();
      auto *thumbnails=single.findChild<QListWidget *>("previewAppSidebar");
      check(thumbnails->isHidden(), "single image opens without automatic sidebar");
      single.findChild<QToolButton *>("previewAppSidebarButton")->click(); check(thumbnails->isVisible(), "leading sidebar toggle works and persists explicit preference");
      single.resize(480,320); QApplication::processEvents();
      check(single.findChild<QWidget *>("previewAppDocumentTools")->isHidden() && single.findChild<QToolButton *>("previewAppMenuButton")->isVisible(), "narrow toolbar collapses tools into persistent menu");
      check(single.findChild<QWidget *>("previewAppZoomGroup")->geometry().right() < single.findChild<QToolButton *>("previewAppMenuButton")->geometry().left(), "narrow toolbar has no overlapping action groups");
      check(single.findChild<QToolButton *>("window-close")->x()>single.findChild<QToolButton *>("previewAppMenuButton")->x(), "explicit right-side controls preserved");
    }
    { ViewerWindow remembered({imageUrl}); check(!remembered.findChild<QListWidget *>("previewAppSidebar")->isHidden(), "explicit thumbnail preference survives another window"); }
    QSettings().remove("Viewer/showThumbnails");
    for (int i = 0; i < 8; ++i) { auto *closing = new ViewerWindow({imageUrl}); closing->show(); delete closing; }
    QApplication::processEvents();
    std::cout << "PASS standalone Preview image/PDF/text, thumbnails, selection, capabilities, keyboard, MIME, recent files and lifecycle\n";
}
