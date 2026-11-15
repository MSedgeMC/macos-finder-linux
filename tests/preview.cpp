// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
// Standalone target: preview.cpp/preview.h, tags.cpp, preferences.cpp,
// Qt Widgets/Pdf/Multimedia (optional PrintSupport),
// and existing KIO/FileMetaData dependencies; AUTOMOC must process preview.cpp.
// Run only offscreen with private XDG/runtime directories and session D-Bus.
#include "preview.h"
#include "tags.h"
#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QKeyEvent>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPdfWriter>
#include <QPushButton>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QLineEdit>
#include <QTextEdit>
#include <QPdfDocument>
#if defined(FINDER_PREVIEW_PRINT_SUPPORT) && FINDER_PREVIEW_PRINT_SUPPORT
#include <QPrinter>
#endif
#include <functional>
#include <iostream>

namespace {
bool waitFor(const std::function<bool()> &condition, int timeout = 10000)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < timeout) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return condition();
}
void check(bool value, const char *message)
{
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTemporaryDir files;
    check(files.isValid(), "temporary files");
    const auto redPath = files.filePath("large.png");
    const auto bluePath = files.filePath("blue.png");
    const auto pdfPath = files.filePath("pages.pdf");
    QImage red(3600, 2800, QImage::Format_RGB32); red.fill(Qt::red);
    QImage blue(160, 100, QImage::Format_RGB32); blue.fill(Qt::blue);
    check(red.save(redPath) && blue.save(bluePath), "image fixtures");
    {
        QPdfWriter writer(pdfPath);
        QPainter painter(&writer);
        painter.fillRect(QRect(100, 100, 300, 300), Qt::black);
        check(writer.newPage(), "second PDF page");
        painter.fillRect(QRect(100, 100, 300, 300), Qt::red);
    }
    const auto redUrl = QUrl::fromLocalFile(redPath), blueUrl = QUrl::fromLocalFile(bluePath);
    PreviewPane pane;
    pane.resize(640, 480);
    pane.show();
    auto *image = pane.findChild<QLabel *>("previewImage");
    auto *content = pane.findChild<QStackedWidget *>("previewContent");
    int ticks = 0;
    QTimer heartbeat; heartbeat.setInterval(1);
    QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++ticks; });
    heartbeat.start();
    QElapsedTimer dispatch; dispatch.start();
    pane.setUrls({redUrl});
    check(dispatch.elapsed() < 200, "large image dispatch returns before decoding");
    for (int i = 0; i < 40; ++i) pane.setUrls({i % 2 ? blueUrl : redUrl});
    pane.setUrls({blueUrl});
    check(waitFor([&] { return content->currentWidget() == image && !image->pixmap().isNull(); }), "latest image loads");
    check(image->pixmap().toImage().pixelColor(image->pixmap().width()/2, image->pixmap().height()/2) == QColor(Qt::blue), "stale red results discarded");
    check(ticks > 0, "GUI heartbeat remains live while worker decodes");
    pane.setUrls({redUrl});
    check(waitFor([&] { return pane.originalImageSize() == red.size(); }), "original resolution retained");
    check(image->pixmap().width() <= 1600, "bounded display raster");
    pane.setUrls({blueUrl});
    check(waitFor([&] { return pane.originalImageSize() == blue.size(); }), "cached geometry changes with file");
    pane.setUrls({redUrl});
    check(pane.originalImageSize() == red.size(), "cache retains original geometry");
    pane.setUrls({QUrl::fromLocalFile(pdfPath)});
    auto *pages = pane.findChild<QLabel *>("previewPageCount");
    check(waitFor([&] { return pages->text() == "Page 1 of 2" && content->currentWidget() == image; }), "worker loads PDF");
    check(image->pixmap().toImage().pixelColor(0,0) == QColor(Qt::white), "PDF paper is white");
    pane.findChild<QPushButton *>("previewNextPage")->click();
    check(waitFor([&] { return pages->text() == "Page 2 of 2" && content->currentWidget() == image; }), "worker renders next PDF page");
    pane.setUrls({redUrl});
    pane.hide();
    pane.setUrls({blueUrl});
    pane.show();
    check(waitFor([&] { return pane.originalImageSize() == blue.size(); }), "hidden work cancels and preview reopens");
    for (int i = 0; i < 8; ++i) {
        auto *closing = new PreviewPane;
        closing->setUrls({redUrl});
        delete closing;
    }
    pane.setUrls({});
    check(waitFor([&] { return content->currentWidget() != image; }), "clear selection resets preview");
    pane.hide();
    int changes = 0, closes = 0;
    QObject::connect(&pane, &PreviewPane::currentUrlChanged, [&] { ++changes; });
    QObject::connect(&pane, &PreviewPane::closeRequested, [&] { ++closes; });
    check(pane.toggleQuickLook({redUrl, blueUrl}, blueUrl), "reusable Quick Look opens");
    check(pane.currentUrl() == blueUrl && changes == 1, "host primary selection emits one change");
    check(waitFor([&] { return pane.originalImageSize() == blue.size(); }), "primary selection renders");
    pane.setZoomFactor(2);
    check(pane.zoomFactor() == 2, "zoom API");
    const QImage beforePan = image->grab().toImage();
    const QPointF centre(image->rect().center());
    QMouseEvent press(QEvent::MouseButtonPress, centre, centre, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, centre + QPointF(90, 20), centre + QPointF(90, 20), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, centre + QPointF(90, 20), centre + QPointF(90, 20), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(image, &press); QApplication::sendEvent(image, &move); QApplication::sendEvent(image, &release);
    check(!beforePan.isNull() && !image->grab().isNull(), "zoom/pan paints without giant pixmap");
    pane.setZoomFactor(100); check(pane.zoomFactor() == 16, "zoom bounded above");
    pane.resetZoom(); check(pane.zoomFactor() == 1, "fit reset");
    pane.setFocus();
    QKeyEvent zoomKey(QEvent::KeyPress, Qt::Key_Plus, Qt::MetaModifier);
    QApplication::sendEvent(&pane, &zoomKey);
    check(pane.zoomFactor() > 1, "Command-plus zoom");
    pane.setContactSheet(true);
    auto *sheet = pane.findChild<QListWidget *>("previewContactSheet");
    check(pane.contactSheet() && sheet->count() == 2 && content->currentWidget() == sheet, "contact sheet selection");
    check(waitFor([&] { return !sheet->item(0)->data(Qt::UserRole + 1).toString().isEmpty()
        && !sheet->item(1)->data(Qt::UserRole + 1).toString().isEmpty(); }), "contact thumbnails load off thread");
    if (argc > 1) check(pane.grab().save(QString::fromLocal8Bit(argv[1])), "isolated screenshot artifact");
    sheet->itemActivated(sheet->item(0));
    check(!pane.contactSheet() && pane.currentUrl() == redUrl, "contact sheet activation opens item");
    pane.setSlideshow(true, 500);
    check(waitFor([&] { return pane.currentUrl() == blueUrl; }, 2500), "slideshow advances");
    check(waitFor([&] { return pane.currentUrl() == redUrl; }, 2500), "slideshow wraps");
    pane.setSlideshow(false); check(!pane.slideshowRunning(), "slideshow stops");
    pane.showMaximized();
    pane.setFullScreen(true); check(pane.isFullScreen(), "fullscreen enters");
    pane.setFullScreen(false); check(pane.isMaximized(), "fullscreen restores maximized state");
    pane.showNormal();
    pane.setCompact(true); pane.setFullScreen(true);
    check(!pane.isFullScreen(), "embedded previews cannot fullscreen host");
    pane.setCompact(false);
    check(!pane.toggleQuickLook({redUrl}), "reusable Quick Look toggles closed");
    check(closes == 1 && !pane.isVisible(), "toggle emits close and hides pane");
    check(pane.toggleQuickLook({blueUrl}), "reusable Quick Look reopens");
    check(waitFor([&] { return pane.originalImageSize() == blue.size(); }), "reopened image");
    QImage green(200, 120, QImage::Format_RGB32); green.fill(Qt::green);
    check(green.save(bluePath), "external replacement fixture");
    check(waitFor([&] { return pane.originalImageSize() == green.size(); }), "file watcher refreshes external edit");
    pane.hide();
    pane.setUrls({blueUrl}); pane.show();
    check(waitFor([&] { return pane.originalImageSize() == green.size(); }), "native action source loads");
    QUrl opened;
    QObject::connect(&pane, &PreviewPane::openRequested, [&](const QUrl &url) { opened = url; });
    pane.openCurrent(); check(opened == blueUrl, "existing open signal retains host action ownership");
    QList<QUrl> shared;
    QObject::connect(&pane, &PreviewPane::shareRequested, [&](const QList<QUrl> &urls) { shared = urls; });
    pane.shareCurrent(); check(shared.isEmpty(), "provider unavailable does not emit fake share");
    pane.setShareProviderAvailable(true); pane.shareCurrent();
    check(shared == QList<QUrl>{blueUrl}, "real provider capability exposes share signal");
#if defined(FINDER_PREVIEW_PRINT_SUPPORT) && FINDER_PREVIEW_PRINT_SUPPORT
    int printed = 0;
    QString printError;
    QObject::connect(&pane, &PreviewPane::printFinished, [&](const QUrl &, const QString &error) { ++printed; printError = error; });
    auto pdfPrinter = [&](const QString &path) {
        auto printer = std::make_shared<QPrinter>();
        printer->setOutputFormat(QPrinter::PdfFormat);
        printer->setOutputFileName(path);
        return printer;
    };
    pane.printToPrinter(pdfPrinter(files.filePath("printed-image.pdf")));
    check(waitFor([&] { return printed == 1; }) && printError.isEmpty(), "original image prints to private PDF");
    pane.setUrls({QUrl::fromLocalFile(pdfPath)});
    check(waitFor([&] { return pages->text() == "Page 1 of 2" && content->currentWidget() == image; }), "print PDF source loads");
    const auto printedPdf = files.filePath("printed-pages.pdf");
    pane.printToPrinter(pdfPrinter(printedPdf));
    check(waitFor([&] { return printed == 2; }) && printError.isEmpty(), "all PDF pages print asynchronously");
    QPdfDocument output;
    check(output.load(printedPdf) == QPdfDocument::Error::None && output.pageCount() == 2, "printed PDF retains both pages");
    output.close();
    const auto rangePdf = files.filePath("printed-range.pdf");
    auto rangePrinter = pdfPrinter(rangePdf);
    rangePrinter->setPrintRange(QPrinter::PageRange); rangePrinter->setFromTo(2, 2);
    pane.printToPrinter(rangePrinter);
    check(waitFor([&] { return printed == 3; }) && printError.isEmpty(), "PDF range prints");
    const auto rangeError = output.load(rangePdf);
    if (rangeError != QPdfDocument::Error::None || output.pageCount() != 1)
        std::cerr << "Range output: error " << int(rangeError) << ", pages " << output.pageCount()
                  << ", requested " << rangePrinter->fromPage() << '-' << rangePrinter->toPage() << '\n';
    check(rangeError == QPdfDocument::Error::None && output.pageCount() == 1, "printed range has one page");
    output.close();
    pane.setUrls({redUrl});
    check(waitFor([&] { return pane.originalImageSize() == red.size(); }), "cancel print source loads");
    pane.printToPrinter(pdfPrinter(files.filePath("cancelled.pdf")));
    pane.hide();
    check(printed == 4 && !printError.isEmpty(), "hide cancels print with source error");
    pane.show();
    std::cout << "PASS: native print original image, complete PDF, page range, cancellation (private PDF outputs only)\n";
#else
    check(!pane.canPrintCurrent() && !pane.findChild<QAction *>("previewPrint")->isEnabled(), "Print disabled without backend");
#endif
    pane.setGallery(true);
    pane.setUrls({blueUrl});
    auto *tagEditor = pane.findChild<QLineEdit *>("previewTagEditor");
    auto *tagStatus = pane.findChild<QLabel *>("previewTagStatus");
    check(waitFor([&] { return tagEditor->isEnabled(); }), "tag read completes");
    QUrl tagUrl;
    QObject::connect(&pane, &PreviewPane::tagChangeRequested, [&](const QUrl &url, const QString &, int) { tagUrl = url; });
    tagEditor->setText("Portable Test Tag");
    tagEditor->returnPressed();
    check(tagUrl == blueUrl && !tagEditor->isEnabled(), "tag mutation delegated without optimistic state");
    pane.setUrls({redUrl});
    pane.tagChangeFinished(blueUrl, "Native tag backend rejected the write.");
    pane.setUrls({blueUrl});
    check(waitFor([&] { return tagEditor->isEnabled(); }), "tag controls recover after failed background write");
    check(tagStatus->text() == "Native tag backend rejected the write.", "tag error survives selection change");
    tagEditor->setText("Portable Test Tag"); tagEditor->returnPressed();
    pane.tagChangeFinished(blueUrl, FileTags::addTag(blueUrl, "Portable Test Tag"));
    check(waitFor([&] { return tagEditor->isEnabled(); }), "successful native tag write refreshes");
    check(tagStatus->text().isEmpty(), "success clears tag error");
    pane.hide();
    std::cout << "PASS: async preview, stale cancellation, original geometry, PDF pages, hide/destruction\n";
    std::cout << "PASS: reusable lifecycle, primary selection, zoom/pan, contact sheet, slideshow, fullscreen, file freshness\n";
    std::cout << "PASS: host open/share capabilities, deferred tag errors and native tag refresh\n";
    return 0;
}
