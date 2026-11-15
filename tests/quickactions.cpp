// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
// Standalone target: quickactions.cpp + src/quickactions.cpp/.h; AUTOMOC.
// Link Qt6::Widgets Qt6::Pdf KF6::KIOCore KF6::KIOGui KF6::KIOWidgets.
// Run offscreen in PRIVATE XDG directories/runtime/session D-Bus; never use
// the user's desktop. Fixtures and all native copy/Undo writes are temporary.
#include "quickactions.h"

#include <KIO/FileUndoManager>
#include <KIO/RenameDialog>
#include <QApplication>
#include <QColorSpace>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFile>
#include <QImageReader>
#include <QImageWriter>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPageSize>
#include <QPainter>
#include <QPdfDocument>
#include <QPdfWriter>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <functional>
#include <iostream>
#include <unistd.h>

using namespace FinderQuickActions;
namespace {
void check(bool success, const char *message)
{
    if (!success) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
bool waitFor(const std::function<bool()> &condition, int timeout = 15000)
{
    QElapsedTimer elapsed; elapsed.start();
    while (!condition() && elapsed.elapsed() < timeout) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return condition();
}
QByteArray bytes(const QString &path)
{
    QFile file(path); check(file.open(QIODevice::ReadOnly), "read fixture"); return file.readAll();
}
struct Observer {
    int completions = 0;
    bool cancelled = false;
    QString error;
    QList<QUrl> outputs;
    explicit Observer(QuickActions &actions) {
        QObject::connect(&actions, &QuickActions::errorOccurred, &actions, [this](const QString &message) { error = message; });
        QObject::connect(&actions, &QuickActions::outputCreated, &actions, [this](const QUrl &url) { outputs.append(url); });
        QObject::connect(&actions, &QuickActions::finished, &actions, [this](bool value) { cancelled = value; ++completions; });
    }
    void reset() { error.clear(); outputs.clear(); cancelled = false; }
    void finish(QuickActions &actions, int expected) {
        check(waitFor([&] { return completions == expected && !actions.busy(); }), "bounded async completion");
        if (!error.isEmpty()) std::cerr << "Action error: " << error.toStdString() << '\n';
    }
};
void drag(ImageCanvas &canvas, const QPointF &start, const QPointF &end)
{
    QMouseEvent down(QEvent::MouseButtonPress, start, start, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, end, end, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent up(QEvent::MouseButtonRelease, end, end, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &down); QApplication::sendEvent(&canvas, &move); QApplication::sendEvent(&canvas, &up);
}
KIO::RenameDialog *conflict()
{
    for (auto *widget : QApplication::topLevelWidgets())
        if (auto *dialog = qobject_cast<KIO::RenameDialog *>(widget); dialog && dialog->isVisible()) return dialog;
    return nullptr;
}
}

int main(int argc, char **argv)
{
    check(qEnvironmentVariable("QT_QPA_PLATFORM") == "offscreen", "offscreen test required");
    check(qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"), "working display disconnected");
    check(!qEnvironmentVariableIsEmpty("XDG_RUNTIME_DIR") && !qEnvironmentVariableIsEmpty("XDG_CONFIG_HOME")
        && !qEnvironmentVariableIsEmpty("XDG_DATA_HOME"), "private XDG environment required");
    QApplication app(argc, argv);
    QTemporaryDir files; check(files.isValid(), "temporary fixtures");
    auto url = [&](const QString &name) { return QUrl::fromLocalFile(files.filePath(name)); };
    QImage source(640, 480, QImage::Format_RGBA64);
    source.fill(QColor(24, 90, 180, 210));
    { QPainter painter(&source); painter.fillRect(QRect(40, 50, 100, 80), Qt::red); }
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    source.setDotsPerMeterX(4000); source.setDotsPerMeterY(6000); source.setText("Description", "Quick Action full-resolution fixture");
    check(source.save(url("source.png").toLocalFile()), "16-bit source fixture");
    const auto originalBytes = bytes(url("source.png").toLocalFile());
    QImage blue(120, 80, QImage::Format_RGB32); blue.fill(Qt::blue);
    check(blue.save(url("blue.png").toLocalFile()), "blue source fixture");

    const auto supported = QuickActions::actionsForUrls({url("source.png")});
    check(supported.size() == 5, "portable action inventory");
    for (const auto &a : supported) check(a.enabled, "single image actions enabled");
    for (const auto &a : QuickActions::actionsForUrls({QUrl("https://example.invalid/a.png")})) check(!a.enabled, "remote source disabled");
    for (const auto &a : QuickActions::actionsForUrls({})) check(!a.enabled, "empty selection disabled");
    check(!QuickActions::actionsForUrls({url("source.png"), url("blue.png")})[2].enabled, "multi-image markup disabled");
    QuickActions actions;
    Observer observed(actions);
    QMenu menu; actions.populateMenu(&menu, {url("source.png")}); check(menu.actions().size() == 5, "host menu population");
    int ticks = 0;
    QTimer heartbeat; heartbeat.setInterval(1); QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++ticks; }); heartbeat.start();
    ImageOptions imageOptions;
    QElapsedTimer dispatch; dispatch.start();
    actions.exportImages({url("source.png")}, {url("original-copy.png")}, imageOptions);
    check(dispatch.elapsed() < 200 && actions.busy(), "dispatch does not decode on GUI");
    check(!menu.actions().first()->isEnabled(), "busy state disables menus");
    observed.finish(actions, 1);
    check(observed.error.isEmpty() && observed.outputs == QList<QUrl>{url("original-copy.png")}, "real native copy output");
    QImage unchanged(url("original-copy.png").toLocalFile());
    QImage decoded(url("source.png").toLocalFile());
    check(unchanged == decoded && unchanged.depth() == 64, "original pixels alpha and 16-bit depth preserved");
    check(unchanged.colorSpace() == decoded.colorSpace() && unchanged.dotsPerMeterX() == decoded.dotsPerMeterX()
        && unchanged.dotsPerMeterY() == decoded.dotsPerMeterY() && unchanged.text("Description") == decoded.text("Description"), "supported color profile resolution and text metadata");
    check(KIO::FileUndoManager::self()->isUndoAvailable(), "output registered in native Undo");
    check(ticks > 0 && menu.actions().first()->isEnabled(), "GUI heartbeat and menus recover");
    int undoFinished = 0;
    QObject::connect(KIO::FileUndoManager::self(), &KIO::FileUndoManager::undoJobFinished, &app, [&] { ++undoFinished; });
    KIO::FileUndoManager::self()->uiInterface()->setShowProgressInfo(false);
    KIO::FileUndoManager::self()->undo();
    check(waitFor([&] { return undoFinished == 1; }), "native Undo completes after staging cleanup");
    check(!QFile::exists(url("original-copy.png").toLocalFile()) && bytes(url("source.png").toLocalFile()) == originalBytes, "Undo removes copy and preserves source");

    observed.reset();
    imageOptions.maximumEdge = 200;
    actions.exportImages({url("source.png"), url("blue.png")}, {url("small.png"), url("small-blue.png")}, imageOptions);
    observed.finish(actions, 2);
    check(observed.error.isEmpty() && observed.outputs.size() == 2, "batch conversion outputs");
    check(QImage(url("small.png").toLocalFile()).size() == QSize(200, 150), "resize preserves aspect");
    check(QImage(url("small-blue.png").toLocalFile()).size() == blue.size(), "small image not upscaled");

    ImageEdits edits; edits.crop = QRect(20, 30, 300, 220); edits.quarterTurns = 1;
    observed.reset(); imageOptions.maximumEdge = 0;
    actions.exportImages({url("source.png")}, {url("edited.png")}, imageOptions, edits);
    observed.finish(actions, 3);
    QImage edited(url("edited.png").toLocalFile());
    QImage expected = decoded.copy(edits.crop).transformed(QTransform().rotate(90));
    check(observed.error.isEmpty() && edited == expected && edited.size() == QSize(220, 300), "full-resolution crop and clockwise pixel rotation");
    check(edited.dotsPerMeterX() == decoded.dotsPerMeterY() && edited.dotsPerMeterY() == decoded.dotsPerMeterX(), "rotation transposes anisotropic resolution");

    observed.reset(); edits = {};
    Annotation a; a.kind = Annotation::Kind::Rectangle; a.rect = QRectF(200, 160, 120, 80); a.width = 12; a.color = Qt::green;
    edits.annotations.append(a);
    a.kind = Annotation::Kind::Draw; a.path.moveTo(400, 40); a.path.lineTo(500, 120); a.color = Qt::yellow; edits.annotations.append(a);
    a.kind = Annotation::Kind::Text; a.rect = QRectF(20, 260, 300, 100); a.text = "Real markup"; a.color = Qt::black; a.textSize = 40; edits.annotations.append(a);
    actions.exportImages({url("source.png")}, {url("markup.png")}, imageOptions, edits); observed.finish(actions, 4);
    QImage marked(url("markup.png").toLocalFile());
    check(observed.error.isEmpty() && marked.size() == decoded.size() && marked.depth() == 64, "markup retains source resolution and depth");
    check(marked.pixelColor(200, 200) == QColor(Qt::green) && marked != decoded, "annotation canvas content reaches export");

    observed.reset(); imageOptions.format = "jpeg";
    actions.exportImages({url("source.png")}, {url("opaque.jpeg")}, imageOptions); observed.finish(actions, 5);
    QImage jpeg(url("opaque.jpeg").toLocalFile());
    check(observed.error.isEmpty() && !jpeg.hasAlphaChannel() && jpeg.size() == decoded.size(), "installed JPEG writer creates opaque output");
    imageOptions = {};

    // KIO owns the actual conflict dialog and renamed output identity.
    observed.reset(); const auto existingBytes = bytes(url("small.png").toLocalFile());
    actions.exportImages({url("blue.png")}, {url("small.png")}, imageOptions);
    check(waitFor([] { return conflict() != nullptr; }), "native conflict presented");
    check(bytes(url("small.png").toLocalFile()) == existingBytes && actions.busy(), "collision does not silently overwrite");
    conflict()->suggestNewNamePressed(); conflict()->renamePressed();
    observed.finish(actions, 6);
    check(observed.error.isEmpty() && observed.outputs.size() == 1 && observed.outputs.first() != url("small.png"), "renamed actual output reported");
    check(QImage(observed.outputs.first().toLocalFile()) == blue && bytes(url("small.png").toLocalFile()) == existingBytes, "renamed copy contains real pixels");

    observed.reset();
    actions.exportImages({url("blue.png")}, {url("small.png")}, imageOptions);
    check(waitFor([] { return conflict() != nullptr; }), "second native conflict"); conflict()->cancelPressed(); observed.finish(actions, 7);
    check(observed.cancelled && observed.outputs.isEmpty() && bytes(url("small.png").toLocalFile()) == existingBytes, "conflict cancellation preserves target");

    observed.reset(); actions.exportImages({url("source.png")}, {url("source.png")}, imageOptions); observed.finish(actions, 8);
    check(!observed.error.isEmpty() && observed.outputs.isEmpty() && bytes(url("source.png").toLocalFile()) == originalBytes, "source overwrite refused");
    const auto hardlink = url("hardlink.png");
    check(::link(QFile::encodeName(url("source.png").toLocalFile()).constData(), QFile::encodeName(hardlink.toLocalFile()).constData()) == 0, "hardlink fixture");
    observed.reset(); actions.exportImages({url("source.png")}, {hardlink}, imageOptions); observed.finish(actions, 9);
    check(!observed.error.isEmpty() && bytes(hardlink.toLocalFile()) == originalBytes, "source hardlink overwrite refused");
    QFile damaged(url("damaged.png").toLocalFile()); check(damaged.open(QIODevice::WriteOnly), "invalid fixture"); damaged.write("not an image"); damaged.close();
    observed.reset(); actions.exportImages({url("blue.png"), url("damaged.png")}, {url("not-published.png"), url("bad-output.png")}, imageOptions); observed.finish(actions, 10);
    check(!observed.error.isEmpty() && observed.outputs.isEmpty() && !QFile::exists(url("not-published.png").toLocalFile()), "batch staging failure publishes no partial exports");

    const auto pages = url("pages.pdf");
    {
        QPdfWriter writer(pages.toLocalFile()); writer.setResolution(72);
        check(writer.setPageSize(QPageSize(QSizeF(120, 80), QPageSize::Point, QString(), QPageSize::ExactMatch)), "PDF fixture page dimensions");
        writer.setPageMargins(QMarginsF(0, 0, 0, 0), QPageLayout::Point);
        QPainter painter(&writer); painter.fillRect(QRect(0, 0, writer.width(), writer.height()), Qt::red);
        check(writer.newPage(), "PDF fixture second page"); painter.fillRect(QRect(0, 0, writer.width(), writer.height()), Qt::green);
    }
    check(QuickActions::actionsForUrls({pages})[1].enabled && !QuickActions::actionsForUrls({pages})[2].enabled, "reader-backed PDF capability with no fake PDF editor");
    observed.reset();
    actions.createPdf({url("blue.png"), pages, url("source.png")}, url("combined.pdf")); observed.finish(actions, 11);
    check(observed.error.isEmpty() && observed.outputs == QList<QUrl>{url("combined.pdf")}, "mixed image PDF output");
    QPdfDocument pdf; check(pdf.load(url("combined.pdf").toLocalFile()) == QPdfDocument::Error::None && pdf.pageCount() == 4, "combined PDF retains all pages");
    const QList<QColor> colors{Qt::blue, Qt::red, Qt::green};
    for (int p = 0; p < colors.size(); ++p) {
        const auto raster = pdf.render(p, QSize(120, 80)); check(!raster.isNull(), "real PDF raster");
        const auto actual = raster.pixelColor(60, 40);
        check(qAbs(actual.red() - colors[p].red()) < 8 && qAbs(actual.green() - colors[p].green()) < 8
            && qAbs(actual.blue() - colors[p].blue()) < 8, "selection and source PDF page order");
    }
    check(qAbs(pdf.pagePointSize(1).width() - 120) < 1 && qAbs(pdf.pagePointSize(1).height() - 80) < 1, "source PDF physical page size retained");
    pdf.close();
    observed.reset(); actions.createPdf({pages}, url("raster-copy.pdf"), PdfOptions{72, "Copy"}); observed.finish(actions, 12);
    check(observed.error.isEmpty() && pdf.load(url("raster-copy.pdf").toLocalFile()) == QPdfDocument::Error::None && pdf.pageCount() == 2, "single PDF makes complete raster copy"); pdf.close();

    const auto huge = url("huge.pdf");
    {
        QPdfWriter writer(huge.toLocalFile()); writer.setResolution(72);
        writer.setPageSize(QPageSize(QSizeF(10000, 10000), QPageSize::Point, QString(), QPageSize::ExactMatch));
        QPainter painter(&writer); painter.drawLine(0, 0, 100, 100);
    }
    observed.reset(); actions.createPdf({huge}, url("rejected-large.pdf")); observed.finish(actions, 13);
    check(!observed.error.isEmpty() && !QFile::exists(url("rejected-large.pdf").toLocalFile()), "oversized PDF raster rejected before allocation");

    observed.reset();
    actions.exportImages({url("source.png")}, {url("cancelled.png")}, imageOptions); actions.cancel(); observed.finish(actions, 14);
    check(observed.cancelled && observed.outputs.isEmpty() && !QFile::exists(url("cancelled.png").toLocalFile()), "generation cancellation prevents publishing");
    observed.reset(); actions.exportImages({url("blue.png")}, {url("after-cancel.png")}, imageOptions); observed.finish(actions, 15);
    check(observed.error.isEmpty() && QImage(url("after-cancel.png").toLocalFile()) == blue, "new generation works after cancellation");

    // A user can cancel after KIO has saved one member of a batch. That member
    // must remain reported and undoable while the second copy never starts.
    observed.reset();
    const auto cancelAfterCopy = QObject::connect(&actions, &QuickActions::outputCreated, &actions,
        [&](const QUrl &output) { if (output == url("partial-first.png")) actions.cancel(); });
    actions.exportImages({url("blue.png"), url("source.png")}, {url("partial-first.png"), url("partial-second.png")}, imageOptions);
    observed.finish(actions, 16); QObject::disconnect(cancelAfterCopy);
    check(observed.cancelled && observed.outputs == QList<QUrl>{url("partial-first.png")}
        && QFile::exists(url("partial-first.png").toLocalFile()) && !QFile::exists(url("partial-second.png").toLocalFile()), "saved outputs reported on cancellation after native publication");
    KIO::FileUndoManager::self()->undo();
    check(waitFor([&] { return undoFinished == 2; }) && !QFile::exists(url("partial-first.png").toLocalFile()), "partial saved copy remains undoable");

    ImageCanvas canvas; canvas.resize(800, 500); canvas.setImage(decoded.scaled(200, 150), decoded.size()); canvas.show();
    canvas.setTool(ImageCanvas::Tool::Rectangle);
    auto map = canvas.imageToWidget(); drag(canvas, map.map(QPointF(150, 120)), map.map(QPointF(350, 240)));
    check(canvas.edits().annotations.size() == 1 && canvas.edits().annotations.first().rect.toRect() == QRect(150, 120, 200, 120), "canvas uses original pixel coordinates");
    canvas.setTool(ImageCanvas::Tool::Select); drag(canvas, map.map(QPointF(200, 170)), map.map(QPointF(220, 180)));
    check(canvas.edits().annotations.first().rect.topLeft().toPoint() == QPoint(170, 130), "editable annotation drag");
    QKeyEvent deleteKey(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier); QApplication::sendEvent(&canvas, &deleteKey);
    check(canvas.edits().annotations.isEmpty(), "canvas keyboard delete"); canvas.undo(); check(canvas.edits().annotations.size() == 1, "canvas undo restores vector annotation");
    canvas.rotate(1); map = canvas.imageToWidget(); canvas.setTool(ImageCanvas::Tool::Crop);
    drag(canvas, map.map(QPointF(40, 60)), map.map(QPointF(300, 280)));
    check(canvas.edits().crop == QRect(40, 60, 260, 220), "crop coordinates preserved after rotation");
    if (argc > 1) check(canvas.grab().save(QString::fromLocal8Bit(argv[1])), "isolated canvas screenshot artifact");
    canvas.resetCrop(); check(canvas.edits().crop.isNull(), "reset crop"); canvas.hide();

    actions.trigger(Action::Markup, {url("source.png")});
    check(waitFor([&] { return !actions.busy(); }), "native editor preview loads async");
    auto *editor = app.findChild<QDialog *>("quickActionEditorDialog");
    if (!editor) for (auto *widget : app.topLevelWidgets()) if (widget->objectName() == "quickActionEditorDialog") editor = qobject_cast<QDialog *>(widget);
    check(editor && editor->findChild<ImageCanvas *>()->originalSize() == decoded.size(), "native editor original geometry");
    check(editor->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->isEnabled(), "editor save enabled only on successful read"); editor->reject(); QApplication::processEvents();
    for (int i = 0; i < 4; ++i) {
        auto *closing = new QuickActions;
        closing->exportImages({url("source.png")}, {url(QStringLiteral("destroyed-%1.png").arg(i))}, imageOptions); delete closing;
    }
    check(bytes(url("source.png").toLocalFile()) == originalBytes, "all workflows preserve source");
    std::cout << "PASS: real image pixels/depth/profile/metadata; resize/crop/rotation/markup/JPEG; native copy/conflict/Undo\n";
    std::cout << "PASS: mixed image/PDF page order and physical geometry; raster limits; async/cancel/destruction; editable canvas\n";
    return 0;
}
