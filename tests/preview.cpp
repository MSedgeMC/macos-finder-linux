// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
// Standalone target: preview.cpp/preview.h, tags.cpp, preferences.cpp,
// Qt Widgets/Pdf/Multimedia (optional PrintSupport),
// and existing KIO/FileMetaData dependencies; AUTOMOC must process preview.cpp.
// Run only offscreen with private XDG/runtime directories and session D-Bus.
#include "preview.h"
#include "tags.h"
#include <QAction>
#include <QAbstractScrollArea>
#include <QScrollBar>
#include <QClipboard>
#include <QProcess>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProgressDialog>
#include <KIO/FileUndoManager>
#include <KIO/RenameDialog>
#include <KJob>
#include <QApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QKeyEvent>
#include <QListWidget>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPdfWriter>
#include <QPushButton>
#include <QStackedWidget>
#include <QSlider>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QToolButton>
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
QByteArray fileBytes(const QString &path)
{
    QFile file(path); check(file.open(QIODevice::ReadOnly), "read private media fixture"); return file.readAll();
}
bool runBackend(const QString &program, const QStringList &arguments, QByteArray *output = nullptr)
{
    QProcess process; process.start(program, arguments);
    if (!process.waitForStarted(5000) || !process.waitForFinished(10000)) { process.kill(); process.waitForFinished(5000); return false; }
    if (output) *output = process.readAllStandardOutput();
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}
KIO::RenameDialog *trimConflict()
{
    for (auto *widget : QApplication::topLevelWidgets())
        if (auto *dialog = qobject_cast<KIO::RenameDialog *>(widget); dialog && dialog->isVisible()) return dialog;
    return nullptr;
}
}

int main(int argc, char **argv)
{
    // Safe even when launched directly by CTest: no display connection or
    // writable configuration/data paths belonging to the working desktop.
    QTemporaryDir isolated;
    check(isolated.isValid(), "isolated test environment");
    qunsetenv("DISPLAY"); qunsetenv("WAYLAND_DISPLAY"); qunsetenv("SESSION_MANAGER");
    qputenv("QT_QPA_PLATFORM", "offscreen");
    for (const auto &entry : QList<QPair<QByteArray, QString>>{
        {"XDG_CONFIG_HOME", "config"}, {"XDG_DATA_HOME", "data"},
        {"XDG_CACHE_HOME", "cache"}, {"XDG_RUNTIME_DIR", "runtime"}}) {
        const QString path = isolated.filePath(entry.second);
        check(QDir().mkpath(path), "private XDG directory");
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        qputenv(entry.first.constData(), path.toLocal8Bit());
    }
    QApplication app(argc, argv);
    {
        PreviewPane styled;
        styled.setAttribute(Qt::WA_TranslucentBackground);
        styled.setStyleSheet(QStringLiteral("#finderPreviewPane{background:#18324c;}"));
        styled.resize(480, 320);
        styled.show();
        QApplication::processEvents();
        const QImage painted = styled.grab().toImage();
        check(painted.pixelColor(1, 1) == QColor(QStringLiteral("#18324c")),
            "preview paints its styled background in a translucent host");
    }
    QTemporaryDir files;
    check(files.isValid(), "temporary files");
    const auto redPath = files.filePath("large.png");
    const auto bluePath = files.filePath("blue.png");
    const auto pdfPath = files.filePath("pages.pdf");
    QImage red(3600, 2800, QImage::Format_RGB32); red.fill(Qt::red);
    QImage blue(160, 100, QImage::Format_RGB32); blue.fill(Qt::blue);
    // A visible edge makes panning observable; the centre remains blue for
    // stale-result checks above.
    { QPainter marker(&blue); marker.fillRect(QRect(0, 0, 40, blue.height()), Qt::yellow); }
    check(red.save(redPath) && blue.save(bluePath), "image fixtures");
    {
        QPdfWriter writer(pdfPath);
        writer.setResolution(72);
        QPainter painter(&writer);
        QFont font = painter.font(); font.setPointSize(16); painter.setFont(font);
        painter.drawText(QPointF(60, 80), "Portable PDF needle one");
        painter.fillRect(QRect(100, 100, 300, 300), Qt::black);
        check(writer.newPage(), "second PDF page");
        painter.fillRect(QRect(100, 100, 300, 300), Qt::red);
        painter.drawText(QPointF(60, 80), "Portable PDF needle two");
    }
    const auto redUrl = QUrl::fromLocalFile(redPath), blueUrl = QUrl::fromLocalFile(bluePath);
    {
        QWidget host;
        host.setStyleSheet(QStringLiteral("#finderPreviewPane{background:#18324c;}"));
        auto *hostLayout = new QVBoxLayout(&host);
        hostLayout->setContentsMargins(0, 0, 0, 0);
        hostLayout->setSpacing(0);
        auto *chrome = new QWidget(&host);
        chrome->setFixedHeight(40);
        auto *chromeLayout = new QHBoxLayout(chrome);
        chromeLayout->setContentsMargins(12, 4, 12, 4);
        auto *windowControl = new QPushButton(QStringLiteral("×"), chrome);
        windowControl->setFixedWidth(24);
        chromeLayout->addWidget(windowControl);
        hostLayout->addWidget(chrome);
        auto *hosted = new PreviewPane(&host);
        hosted->setHostedQuickLook(true);
        QWidget *header = hosted->quickLookHeaderWidget();
        chromeLayout->addWidget(header, 1);
        hostLayout->addWidget(hosted, 1);
        host.resize(480, 320);
        host.show();
        hosted->updateQuickLook({redUrl, blueUrl}, blueUrl);
        auto *hostedImage = hosted->findChild<QLabel *>("previewImage");
        check(waitFor([&] { return hosted->originalImageSize() == blue.size(); }), "hosted image loads");
        check(hosted->hostedQuickLook() && hosted->quickLookHeaderWidget() == header
            && header->parentWidget() == chrome, "host reuses the existing header");
        check(header->findChild<QLabel *>("previewName")->text() == blueUrl.fileName()
            && header->findChild<QPushButton *>("previewOpen")->text() == "Open", "one live filename and compact Open in chrome");
        check(hostedImage->mapTo(&host, QPoint()).y() < 60 && header->height() <= 32
            && header->width() >= header->minimumSizeHint().width(), "40px chrome leaves content free of a duplicate header");
        auto *toolbar = hosted->findChild<QWidget *>("previewViewControls");
        auto *metadata = hosted->findChild<QLabel *>("previewMetadata");
        check(toolbar->isHidden() && metadata->isHidden()
            && header->findChild<QPushButton *>("previewClose")->isHidden(), "hosted clutter and embedded close hidden by default");
        auto *actions = header->findChild<QToolButton *>("previewNativeActions");
        check(actions && actions->isVisible() && actions->menu(), "Actions stays accessible in the host header");
        actions->menu()->popup(actions->mapToGlobal(QPoint(0, actions->height())));
        QApplication::processEvents();
        check(actions->menu()->isVisible(), "hosted Actions menu opens offscreen");
        actions->menu()->hide();
        auto *informationAction = actions->findChild<QAction *>("previewInformationAction");
        informationAction->trigger();
        check(metadata->isVisible() && !metadata->text().isEmpty(), "hidden metadata can be revealed");
        informationAction->trigger();
        check(metadata->isHidden(), "metadata toggle hides details again");
        QUrl hostedOpened, hostedInfo;
        QObject::connect(hosted, &PreviewPane::openRequested, [&](const QUrl &url) { hostedOpened = url; });
        QObject::connect(hosted, &PreviewPane::getInfoRequested, [&](const QUrl &url) { hostedInfo = url; });
        header->findChild<QPushButton *>("previewOpen")->click();
        actions->findChild<QAction *>("previewGetInfoAction")->trigger();
        check(hostedOpened == blueUrl && hostedInfo == blueUrl, "reused header retains host-owned native actions");
        actions->findChild<QAction *>("previewZoomInAction")->trigger();
        check(hosted->zoomFactor() > 1, "hidden zoom toolbar remains available through Actions");
        actions->findChild<QAction *>("previewFitAction")->trigger();
        check(hosted->zoomFactor() == 1, "menu Fit restores zoom");
        auto *indexAction = actions->findChild<QAction *>("previewIndexSheetAction");
        indexAction->trigger();
        check(hosted->contactSheet() && indexAction->isChecked(), "hosted Index Sheet menu command");
        indexAction->trigger();
        check(!hosted->contactSheet(), "hosted Index Sheet returns to preview");
        auto *slideshowAction = actions->findChild<QAction *>("previewSlideshowAction");
        slideshowAction->trigger();
        check(hosted->slideshowRunning() && slideshowAction->isChecked(), "hosted slideshow menu command");
        slideshowAction->trigger();
        check(!hosted->slideshowRunning(), "hosted slideshow pauses");
        auto *fullScreenAction = actions->findChild<QAction *>("previewFullScreenAction");
        fullScreenAction->trigger();
        check(hosted->isFullScreen() && fullScreenAction->isChecked(), "hosted fullscreen menu controls the host");
        fullScreenAction->trigger();
        check(!hosted->isFullScreen(), "hosted fullscreen restores host state");
        if (argc > 1) check(host.grab().save(QString::fromLocal8Bit(argv[1]) + QStringLiteral(".hosted.png")), "isolated one-row hosted screenshot");
        hosted->setCompact(true);
        check(header->isHidden() && !indexAction->isEnabled(), "compact preview suppresses hosted window commands");
        hosted->setCompact(false);
        check(header->isVisible() && toolbar->isHidden(), "hosted chrome survives compact transition");
        hosted->setGallery(true);
        check(header->isHidden() && metadata->isVisible()
            && hosted->findChild<QPushButton *>("previewOpen")->isVisible(), "Gallery retains metadata and Open");
        hosted->setGallery(false);
        check(header->findChild<QPushButton *>("previewOpen")->isVisible()
            && actions->parentWidget() == header && metadata->isHidden(), "Gallery transition restores hosted header and defaults");
        int hostedCloses = 0;
        QObject::connect(hosted, &PreviewPane::closeRequested, [&] { ++hostedCloses; });
        header->findChild<QPushButton *>("previewOpen")->setFocus();
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(&host, &escape);
        check(hostedCloses == 1 && !hosted->isVisible(), "header focus retains Quick Look Escape close");
        hosted->show();
        hosted->setHostedQuickLook(false);
        check(header->parentWidget() == hosted && toolbar->isVisible() && metadata->isVisible(), "disabling hosted mode restores embedded presentation");
        hosted->setHostedQuickLook(true);
        chromeLayout->addWidget(header, 1);
        auto *pdfDocument = hosted->findChild<QAbstractScrollArea *>("previewPdf");
        hosted->updateQuickLook({QUrl::fromLocalFile(pdfPath)});
        check(waitFor([&] { return hosted->pdfPageCount() == 2; })
            && hosted->findChild<QLineEdit *>("previewPdfSearch")->isVisible()
            && toolbar->isHidden(), "hosted PDF retains page and search controls without generic toolbar");
        hosted->setPdfPage(1);
        check(hosted->pdfCurrentPage() == 1 && pdfDocument->isVisible(), "hosted PDF page navigation");
        hosted->showNormal();
        const auto mediaPath = files.filePath("hosted.wav");
        { QFile media(mediaPath); check(media.open(QIODevice::WriteOnly), "hosted media fixture"); media.write("RIFF"); }
        hosted->updateQuickLook({QUrl::fromLocalFile(mediaPath)});
        check(hosted->findChild<QPushButton *>("previewPlay")->isVisible()
            && hosted->findChild<QSlider *>("previewSeek")->isVisible()
            && toolbar->isHidden(), "hosted media retains playback controls");
        // Simulate a host that deletes its chrome before the content pane.
        delete chrome;
        check(header->parentWidget() == hosted && actions->menu(), "host chrome teardown reclaims the live header");
        hosted->updateQuickLook({blueUrl});
        check(waitFor([&] { return hosted->originalImageSize() == blue.size(); }), "reclaimed header retains asynchronous delivery safety");
        auto *secondChrome = new QWidget(&host);
        auto *secondLayout = new QHBoxLayout(secondChrome);
        secondLayout->addWidget(header);
        QPointer<QWidget> lifetime(header);
        delete hosted;
        check(lifetime.isNull(), "pane destruction cleans a reparented header");
        std::cout << "PASS: hosted 40px header, hidden clutter, view/info actions, PDF/media controls, embedding transitions and header teardown\n";
    }
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
    auto *pdfView = pane.findChild<QAbstractScrollArea *>("previewPdf");
    check(waitFor([&] { return pages->text() == "Page 1 of 2" && content->currentWidget() == pdfView; }), "worker loads PDF");
    check(pdfView->verticalScrollBar()->maximum() > 0, "multipage PDF has a real scroll range");
    const int fitRange = pdfView->verticalScrollBar()->maximum();
    pane.setZoomFactor(2);
    check(pdfView->verticalScrollBar()->maximum() > fitRange, "PDF zoom changes document geometry");
    pane.resetZoom();
    int searches = 0, matches = -1; bool limited = true;
    QObject::connect(&pane, &PreviewPane::pdfSearchFinished, [&](const QString &, int count, bool bounded) { ++searches; matches = count; limited = bounded; });
    pane.searchPdf("needle");
    check(waitFor([&] { return searches == 1; }) && matches == 2 && !limited, "public QtPdf text search spans both pages");
    pane.advancePdfSearch(1);
    check(pane.pdfCurrentPage() == 1, "next PDF match navigates to second page");
    pane.setPdfPage(0);
    QPdfDocument selectionFixture;
    check(selectionFixture.load(pdfPath) == QPdfDocument::Error::None, "selection fixture opens");
    const auto fixtureSelection = selectionFixture.getAllText(0);
    const auto textBounds = fixtureSelection.boundingRectangle();
    selectionFixture.close();
    pane.selectPdfText(0, textBounds.topLeft(), textBounds.bottomRight());
    check(waitFor([&] { return pane.selectedPdfText().contains("needle"); }), "PDF text selection supplies real text");
    if (argc > 1) check(pane.grab().save(QString::fromLocal8Bit(argv[1]) + QStringLiteral(".pdf.png")), "isolated PDF screenshot artifact");
    QKeyEvent copyPdf(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
    QApplication::sendEvent(pdfView, &copyPdf);
    check(QGuiApplication::clipboard()->text().contains("needle"), "PDF selection copies to clipboard");
    pane.findChild<QPushButton *>("previewNextPage")->click();
    check(waitFor([&] { return pages->text() == "Page 2 of 2" && content->currentWidget() == pdfView; }), "worker renders next PDF page");
    pane.searchPdf("needle");
    pane.setUrls({redUrl});
    check(pane.selectedPdfText().isEmpty(), "navigation clears stale PDF selection immediately");
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
    pane.setHostWindowControls(true);
    pane.setCompact(true); pane.setCompact(false);
    check(pane.findChild<QPushButton *>("previewClose")->isHidden(), "native host close ownership survives embedding changes");
    pane.setHostWindowControls(false);
    const QImage beforePan = image->grab().toImage();
    const QPointF centre(image->rect().center());
    QMouseEvent press(QEvent::MouseButtonPress, centre, centre, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, centre + QPointF(90, 20), centre + QPointF(90, 20), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, centre + QPointF(90, 20), centre + QPointF(90, 20), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(image, &press); QApplication::sendEvent(image, &move); QApplication::sendEvent(image, &release);
    check(!beforePan.isNull() && beforePan != image->grab().toImage(), "drag changes visible zoomed raster");
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
    QUrl rotateSource;
    QObject::connect(&pane, &PreviewPane::rotateCopyRequested, [&](const QUrl &url) { rotateSource = url; });
    pane.findChild<QAction *>("previewRotateCopy")->trigger();
    check(rotateSource == blueUrl && pane.originalImageSize() == green.size(), "Quick Look rotate export keeps original source and geometry");
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
    check(waitFor([&] { return pages->text() == "Page 1 of 2" && content->currentWidget() == pdfView; }), "print PDF source loads");
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
    pane.setGallery(false);
    const auto textPath = files.filePath("preview.txt");
    { QFile text(textPath); check(text.open(QIODevice::WriteOnly), "text fixture"); text.write("Plain text preview\n"); }
    int readiness = 0;
    QObject::connect(&pane, &PreviewPane::printReadinessChanged, [&](bool ready) { if (ready) ++readiness; });
    pane.setUrls({QUrl::fromLocalFile(textPath)});
    check(!pane.canPrintCurrent(), "text Print waits for asynchronous readiness");
    auto *text = pane.findChild<QTextEdit *>("previewText");
    check(waitFor([&] { return content->currentWidget() == text; }), "text preview loads asynchronously");
#if defined(FINDER_PREVIEW_PRINT_SUPPORT) && FINDER_PREVIEW_PRINT_SUPPORT
    check(readiness == 1 && pane.canPrintCurrent(), "text Print readiness notifies host after first load");
    pane.printToPrinter(pdfPrinter(files.filePath("printed-text.pdf")));
    check(printed == 5 && printError.isEmpty(), "first-show text prints after readiness");
#endif
    text->setFocus();
    QKeyEvent focusedSpace(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QApplication::sendEvent(text, &focusedSpace);
    check(pane.isVisible(), "Space in focused text preserves Quick Look");
    const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg"), ffprobe = QStandardPaths::findExecutable("ffprobe");
    if (!ffmpeg.isEmpty() && !ffprobe.isEmpty()) {
        const QString audioPath = files.filePath("audio.wav");
        check(runBackend(ffmpeg, {"-nostdin", "-v", "error", "-f", "lavfi", "-i", "sine=frequency=440:duration=4", "-c:a", "pcm_s16le", audioPath}), "real audio fixture");
        const auto originalAudio = fileBytes(audioPath);
        const QUrl audioUrl = QUrl::fromLocalFile(audioPath), audioCopy = QUrl::fromLocalFile(files.filePath("audio-trim.wav"));
        pane.setUrls({audioUrl});
        const QByteArray backendPath = qgetenv("PATH");
        qputenv("PATH", files.filePath("no-backends").toLocal8Bit());
        check(!pane.canTrimCurrent(), "missing ffmpeg backend disables real trim capability");
        qputenv("PATH", backendPath);
        check(pane.canTrimCurrent() && pane.findChild<QAction *>("previewTrimCopy")->isEnabled(), "installed media trim backend exposed");
        int trimmed = 0, trimProgress = -1; QString trimError; QUrl actual;
        QObject::connect(&pane, &PreviewPane::trimFinished, [&](const QUrl &source, const QUrl &output, const QString &error) {
            check(source == pane.currentUrl(), "trim completion retains source identity");
            ++trimmed; actual = output; trimError = error;
            if (!error.isEmpty()) std::cerr << "Trim error: " << error.toStdString() << "\n";
        });
        QObject::connect(&pane, &PreviewPane::trimProgressChanged, [&](int value) { trimProgress = value; });
        dispatch.restart(); pane.exportTrimCopy(500, 1750, audioCopy);
        check(dispatch.elapsed() < 200 && pane.trimBusy(), "trim dispatch is asynchronous");
        check(waitFor([&] { return trimmed == 1 && !pane.trimBusy(); }) && trimError.isEmpty() && actual == audioCopy, "real audio trim publishes native copy");
        QByteArray duration;
        check(runBackend(ffprobe, {"-v", "error", "-show_entries", "format=duration", "-of", "default=nw=1:nk=1", audioCopy.toLocalFile()}, &duration)
            && std::abs(duration.trimmed().toDouble() - 1.25) < .02, "encoded audio has requested trim duration");
        check(trimProgress == 100 && fileBytes(audioPath) == originalAudio, "trim progress finishes and original bytes survive");
        check(KIO::FileUndoManager::self()->isUndoAvailable(), "trim copy registers native Undo");
        int undos = 0;
        QObject::connect(KIO::FileUndoManager::self(), &KIO::FileUndoManager::undoJobFinished, &app, [&] { ++undos; });
        KIO::FileUndoManager::self()->uiInterface()->setShowProgressInfo(false);
        KIO::FileUndoManager::self()->undo();
        check(waitFor([&] { return undos == 1; }) && !QFile::exists(audioCopy.toLocalFile()) && fileBytes(audioPath) == originalAudio,
            "native Undo removes trimmed copy after staging cleanup");
        check(runBackend(ffmpeg, {"-nostdin", "-v", "error", "-i", audioPath, "-c:a", "pcm_s16le", audioCopy.toLocalFile()}), "existing collision fixture");
        const auto existing = fileBytes(audioCopy.toLocalFile());
        pane.exportTrimCopy(0, 1000, audioCopy);
        check(waitFor([] { return trimConflict() != nullptr; }), "trim presents genuine native conflict");
        check(fileBytes(audioCopy.toLocalFile()) == existing, "trim never silently overwrites collision");
        trimConflict()->suggestNewNamePressed(); trimConflict()->renamePressed();
        check(waitFor([&] { return trimmed == 2; }) && trimError.isEmpty() && actual != audioCopy && QFile::exists(actual.toLocalFile()), "trim reports actual renamed native destination");
        pane.exportTrimCopy(0, 1000, audioCopy);
        check(waitFor([] { return trimConflict() != nullptr; }), "second native trim conflict");
        trimConflict()->cancelPressed();
        const bool conflictCancelled = waitFor([&] { return trimmed == 3; });
        check(conflictCancelled && !trimError.isEmpty() && actual.isEmpty() && fileBytes(audioCopy.toLocalFile()) == existing,
            "native conflict cancellation preserves existing destination");
        pane.exportTrimCopy(0, 1000, audioUrl);
        check(trimmed == 4 && !trimError.isEmpty() && !pane.trimBusy() && fileBytes(audioPath) == originalAudio, "trim refuses source overwrite");
        const QUrl cancelled = QUrl::fromLocalFile(files.filePath("cancelled-trim.wav"));
        pane.exportTrimCopy(0, 3000, cancelled); pane.cancelTrim();
        check(trimmed == 5 && !pane.trimBusy() && !trimError.isEmpty() && !QFile::exists(cancelled.toLocalFile()), "probe cancellation cleans unpublished copy");
        pane.exportTrimCopy(0, 3000, cancelled);
        pane.hide();
        check(trimmed == 6 && !pane.trimBusy() && !trimError.isEmpty() && !QFile::exists(cancelled.toLocalFile()), "hide cancels trim without publishing");
        pane.show();
        const QString videoPath = files.filePath("video.mp4");
        check(runBackend(ffmpeg, {"-nostdin", "-v", "error", "-f", "lavfi", "-i", "color=c=red:s=64x48:r=10:d=3", "-c:v", "libx264", "-pix_fmt", "yuv420p", videoPath}), "real video fixture");
        const auto videoOriginal = fileBytes(videoPath);
        pane.setUrls({QUrl::fromLocalFile(videoPath)});
        const QUrl videoCopy = QUrl::fromLocalFile(files.filePath("video-trim.mp4"));
        pane.exportTrimCopy(500, 1500, videoCopy);
        check(waitFor([&] { return trimmed == 7; }) && trimError.isEmpty() && actual == videoCopy, "real video trim publishes native copy");
        check(runBackend(ffprobe, {"-v", "error", "-show_entries", "format=duration", "-of", "default=nw=1:nk=1", videoCopy.toLocalFile()}, &duration)
            && std::abs(duration.trimmed().toDouble() - 1.) < .15 && fileBytes(videoPath) == videoOriginal, "video output duration and source preservation");
        pane.exportTrimCopy(0, 5000, QUrl::fromLocalFile(files.filePath("beyond-duration.mp4")));
        check(waitFor([&] { return trimmed == 8; }) && !trimError.isEmpty() && !QFile::exists(files.filePath("beyond-duration.mp4")), "range past actual duration is rejected before encoding");
        // A controlled external encoder fixture keeps cancellation deterministic
        // without wasting CPU on a large generated movie. ffprobe remains real.
        const QString backendDirectory = files.filePath("cancel-backend");
        check(QDir().mkpath(backendDirectory), "private cancel backend directory");
        { QFile script(backendDirectory + "/ffmpeg"); check(script.open(QIODevice::WriteOnly), "cancel encoder fixture");
          script.write("#!/bin/sh\nexec /usr/bin/sleep 30\n"); script.close();
          check(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner), "encoder fixture executable"); }
        qputenv("PATH", backendDirectory.toLocal8Bit() + ':' + backendPath);
        const QUrl cancelledEncoder = QUrl::fromLocalFile(files.filePath("cancelled-encoder.mp4"));
        pane.exportTrimCopy(0, 2000, cancelledEncoder);
        check(waitFor([&] {
            for (auto *widget : QApplication::topLevelWidgets()) {
                if (auto *progress = qobject_cast<QProgressDialog *>(widget); progress && progress->objectName() == "previewTrimProgress"
                    && progress->labelText() == "Encoding trimmed copy…") return true;
            }
            return false;
        }), "bounded asynchronous encoder starts");
        const int beforeCancelTicks = ticks;
        dispatch.restart(); pane.cancelTrim();
        check(dispatch.elapsed() < 200 && trimmed == 9 && !pane.trimBusy() && !trimError.isEmpty()
            && !QFile::exists(cancelledEncoder.toLocalFile()), "running encoder cancellation does not block or publish");
        check(waitFor([&] { return ticks > beforeCancelTicks; }), "GUI heartbeat survives encoder cancellation");
        qputenv("PATH", backendPath);
        std::cout << "PASS: real ffmpeg audio/video trim duration, native conflict/renamed result/Undo, progress/cancel and source preservation\n";
    } else {
        check(!pane.canTrimCurrent(), "absent backend cannot expose fake trim");
        std::cout << "SKIP: trim fixtures require installed ffmpeg and ffprobe\n";
    }
    pane.hide();
    std::cout << "PASS: async preview, stale cancellation, original geometry, PDF pages, hide/destruction\n";
    std::cout << "PASS: reusable lifecycle, primary selection, zoom/pan, contact sheet, slideshow, fullscreen, file freshness\n";
    std::cout << "PASS: host open/share capabilities, deferred tag errors and native tag refresh\n";
    std::cout << "PASS: PDF multipage scrolling/zoom/search/selection/copy, native host controls and first-show async text Print readiness\n";
    return 0;
}
