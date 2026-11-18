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
    const QUrl imageUrl = QUrl::fromLocalFile(fixture.filePath("Image.png"));
    const QUrl pdfUrl = QUrl::fromLocalFile(fixture.filePath("Document.pdf"));
    const QUrl textUrl = QUrl::fromLocalFile(fixture.filePath("Readme.txt"));
    QImage image(600, 400, QImage::Format_RGB32); image.fill(Qt::cyan); check(image.save(imageUrl.toLocalFile()), "image fixture");
    { QPdfWriter pdf(pdfUrl.toLocalFile()); QPainter painter(&pdf); painter.drawText(QPoint(100, 100), "Viewer searchable document"); check(pdf.newPage(), "PDF second page"); painter.drawText(QPoint(100, 100), "Second page"); }
    { QFile text(textUrl.toLocalFile()); check(text.open(QIODevice::WriteOnly), "text fixture"); text.write("Readable standalone document\n"); }
    ViewerWindow window({imageUrl, pdfUrl, textUrl}); window.show();
    auto *pane = window.preview(); auto *sidebar = window.findChild<QListWidget *>("previewAppSidebar");
    check(sidebar && sidebar->count() == 3 && waitFor([&] { return pane->originalImageSize() == image.size(); }), "standalone opens an image with ordered documents");
    pane->actualSize(); check(pane->imageScale() == 1, "standalone actual size");
    check(waitFor([&] { return sidebar->item(0)->data(Qt::UserRole + 1).toBool(); }), "native image thumbnail delivered");
    QKeyEvent space(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier); QApplication::sendEvent(pane, &space);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier); QApplication::sendEvent(pane, &escape);
    check(window.isVisible() && pane->isVisible(), "standalone Space and Escape retain the document window");
    window.findChild<QAction *>("previewAppNext")->trigger();
    check(waitFor([&] { return pane->pdfPageCount() == 2; }) && pane->currentUrl() == pdfUrl && sidebar->currentRow() == 1, "Next synchronizes document and sidebar");
    check(window.findChild<QAction *>("previewAppFind")->isEnabled(), "PDF Find uses document capability");
    pane->searchPdf("Viewer");
    window.findChild<QAction *>("previewAppNext")->trigger();
    auto *text = pane->findChild<QTextEdit *>("previewText");
    check(waitFor([&] { return text->toPlainText().contains("Readable standalone document"); }) && sidebar->currentRow() == 2, "text opens asynchronously and advances exact URL");
    check(!window.findChild<QAction *>("previewAppNext")->isEnabled() && window.findChild<QAction *>("previewAppFind")->isEnabled(), "last item and text Find capabilities");
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
    for (int i = 0; i < 8; ++i) { auto *closing = new ViewerWindow({imageUrl}); closing->show(); delete closing; }
    QApplication::processEvents();
    std::cout << "PASS standalone Preview image/PDF/text, thumbnails, selection, capabilities, keyboard, MIME, recent files and lifecycle\n";
}
