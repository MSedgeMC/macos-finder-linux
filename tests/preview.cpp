// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
// Standalone target: preview.cpp/preview.h, tags.cpp, Qt Widgets/Pdf/Multimedia
// and existing KIO/FileMetaData dependencies; AUTOMOC must process preview.cpp.
// Run only offscreen with private XDG/runtime directories and session D-Bus.
#include "preview.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPdfWriter>
#include <QPushButton>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
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
    std::cout << "PASS: async preview, stale cancellation, original geometry, PDF pages, hide/destruction\n";
    return 0;
}
