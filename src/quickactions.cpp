// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#include "alerts.h"
#include "quickactions.h"

#include <KIO/CopyJob>
#include <KIO/FileUndoManager>
#include <KIO/JobUiDelegateFactory>
#include <KJobUiDelegate>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QColorSpace>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QImageReader>
#include <QImageWriter>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPageSize>
#include <QPainter>
#include <QPdfDocument>
#include <QPdfWriter>
#include <QProgressBar>
#include <QPushButton>
#include <QRunnable>
#include <QSpinBox>
#include <QSet>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QToolButton>
#include <QVBoxLayout>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <functional>
#include <sys/stat.h>

using namespace FinderQuickActions;

struct QuickActionsState {
    bool busy = false;
    quint64 generation = 0;
    std::shared_ptr<std::atomic_bool> cancelled;
    std::shared_ptr<QTemporaryDir> temporary;
    QStringList staged;
    QList<QUrl> destinations, published;
    int next = 0;
    QPointer<KIO::CopyJob> job;
};

class QuickActionTask : public QObject {
    Q_OBJECT
signals:
    void done();
};

namespace {
constexpr qint64 inputLimit = 256LL * 1024 * 1024;
constexpr qint64 imagePixelLimit = 32LL * 1000 * 1000;
constexpr qint64 pdfPixelLimit = 16LL * 1000 * 1000;
constexpr qint64 totalPdfPixels = 256LL * 1000 * 1000;
constexpr qint64 outputLimit = 512LL * 1024 * 1024;
constexpr int pageLimit = 200;
constexpr int inputCountLimit = 64;
const QSize previewLimit(1600, 1200);

struct WorkContext {
    std::shared_ptr<std::atomic_bool> cancelled;
    QElapsedTimer elapsed;
    QString error;
    explicit WorkContext(std::shared_ptr<std::atomic_bool> flag) : cancelled(std::move(flag)) { elapsed.start(); }
    bool proceed() {
        if (cancelled->load()) { error = QStringLiteral("Cancelled."); return false; }
        if (elapsed.elapsed() > 120000) { error = QStringLiteral("The action exceeded its processing time limit."); return false; }
        return true;
    }
};

QThreadPool &actionPool()
{
    static QThreadPool pool;
    static const bool configured = [&] {
        pool.setMaxThreadCount(2);
        pool.setExpiryTimeout(10000);
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, qApp, [&] { pool.waitForDone(); });
        return true;
    }();
    Q_UNUSED(configured);
    return pool;
}

template<typename Work, typename Done>
void submit(QObject *receiver, Work work, Done done)
{
    auto task = std::shared_ptr<QuickActionTask>(new QuickActionTask,
        [](QuickActionTask *p) { p->deleteLater(); });
    using Result = decltype(work());
    auto result = std::make_shared<Result>();
    QObject::connect(task.get(), &QuickActionTask::done, receiver,
        [task, result, done = std::move(done)] { done(*result); },
        Qt::ConnectionType(Qt::QueuedConnection | Qt::SingleShotConnection));
    actionPool().start(QRunnable::create([task, result, work = std::move(work)] {
        *result = work();
        emit task->done();
    }));
}

// Bound writes as well as decode dimensions. Partial data stays in staging.
class BoundedFile : public QIODevice {
public:
    BoundedFile(const QString &path, WorkContext &context) : file(path), work(context) {
        if (file.open(QIODevice::ReadWrite | QIODevice::Truncate)) open(QIODevice::ReadWrite);
        else setErrorString(file.errorString());
    }
    bool seek(qint64 position) override { return file.seek(position) && QIODevice::seek(position); }
    qint64 size() const override { return file.size(); }
    bool good() { return file.flush() && file.error() == QFileDevice::NoError && !failed; }
protected:
    qint64 readData(char *data, qint64 length) override { return file.read(data, length); }
    qint64 writeData(const char *data, qint64 length) override {
        if (!work.proceed() || length > outputLimit - file.pos()) {
            failed = true;
            setErrorString(work.error.isEmpty() ? QStringLiteral("The output exceeds 512 MiB.") : work.error);
            return -1;
        }
        const auto written = file.write(data, length);
        if (written != length) { failed = true; setErrorString(file.errorString()); }
        return written;
    }
private:
    QFile file;
    WorkContext &work;
    bool failed = false;
};

QString localSource(const QUrl &url, WorkContext &work)
{
    if (!work.proceed()) return {};
    const QFileInfo info(url.toLocalFile());
    if (!url.isLocalFile() || url.hasQuery() || url.hasFragment() || !info.isFile() || info.isSymLink()) {
        work.error = QStringLiteral("Select local regular image or PDF files. Links and remote sources are not supported.");
        return {};
    }
    if (info.size() <= 0 || info.size() > inputLimit) {
        work.error = QStringLiteral("Each source must be nonempty and no larger than 256 MiB."); return {};
    }
    return info.absoluteFilePath();
}

bool validPixels(const QSize &size, qint64 limit)
{
    return size.width() > 0 && size.height() > 0 && qint64(size.width()) * size.height() <= limit;
}

QByteArray formatFor(const QUrl &url)
{
    // Extension-only lookup avoids disk sniffing/network work on menu opening.
    const auto suffix = QFileInfo(url.path()).suffix().toLower().toLatin1();
    if (suffix == "jpg" || suffix == "jpe") return "jpeg";
    if (suffix == "tif") return "tiff";
    return suffix;
}

bool imageUrl(const QUrl &url)
{
    return url.isLocalFile() && QImageReader::supportedImageFormats().contains(formatFor(url));
}
bool pdfUrl(const QUrl &url) { return url.isLocalFile() && formatFor(url) == "pdf"; }

QImage readImage(const QUrl &url, WorkContext &work, bool preview = false, QSize *original = nullptr)
{
    const auto path = localSource(url, work);
    if (path.isEmpty()) return {};
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (!validPixels(size, imagePixelLimit)) {
        work.error = QStringLiteral("The image dimensions are unknown or exceed 32 million pixels."); return {};
    }
    if (reader.supportsAnimation() || reader.imageCount() > 1) {
        work.error = QStringLiteral("Animated and multi-frame images are not flattened silently. Select a single-frame image."); return {};
    }
    if (original) {
        *original = size;
        if (reader.transformation() & QImageIOHandler::TransformationRotate90) original->transpose();
    }
    if (preview && (size.width() > previewLimit.width() || size.height() > previewLimit.height()))
        reader.setScaledSize(size.scaled(previewLimit, Qt::KeepAspectRatio));
    QImage image = reader.read();
    if (image.isNull()) { work.error = reader.errorString(); return {}; }
    if (!work.proceed()) return {};
    if (!preview && !validPixels(image.size(), imagePixelLimit)) {
        work.error = QStringLiteral("The decoded image exceeds the pixel limit."); return {};
    }
    if (preview && (image.width() > previewLimit.width() || image.height() > previewLimit.height()))
        image = image.scaled(previewLimit, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}

void copyMetadata(const QImage &source, QImage &target, bool transpose = false, qreal scale = 1)
{
    target.setColorSpace(source.colorSpace());
    target.setDotsPerMeterX(qRound((transpose ? source.dotsPerMeterY() : source.dotsPerMeterX()) * scale));
    target.setDotsPerMeterY(qRound((transpose ? source.dotsPerMeterX() : source.dotsPerMeterY()) * scale));
    for (const auto &key : source.textKeys()) target.setText(key, source.text(key));
}

void paintAnnotation(QPainter &painter, const Annotation &annotation)
{
    painter.setPen(QPen(annotation.color, annotation.width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    switch (annotation.kind) {
    case Annotation::Kind::Draw: painter.drawPath(annotation.path); break;
    case Annotation::Kind::Rectangle: painter.drawRect(annotation.rect); break;
    case Annotation::Kind::Ellipse: painter.drawEllipse(annotation.rect); break;
    case Annotation::Kind::Text: {
        auto font = painter.font(); font.setPixelSize(qRound(annotation.textSize)); painter.setFont(font);
        painter.drawText(annotation.rect, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, annotation.text);
        break;
    }
    }
}

bool validEdits(const ImageEdits &edits, const QSize &size, WorkContext &work)
{
    if ((!edits.crop.isNull() && (edits.crop.isEmpty() || !QRect(QPoint(), size).contains(edits.crop)))
        || edits.annotations.size() > 512) {
        work.error = QStringLiteral("The crop is outside the image or there are too many annotations."); return false;
    }
    qsizetype elements = 0;
    for (const auto &a : edits.annotations) {
        elements += a.path.elementCount();
        if (elements > 100000 || !a.color.isValid() || !std::isfinite(a.width) || a.width <= 0 || a.width > 4096
            || !std::isfinite(a.textSize) || a.textSize < 1 || a.textSize > 4096 || a.text.size() > 4096
            || !std::isfinite(a.rect.x()) || !std::isfinite(a.rect.y())
            || !std::isfinite(a.rect.width()) || !std::isfinite(a.rect.height())) {
            work.error = QStringLiteral("An annotation exceeds the editing limits."); return false;
        }
        for (int i = 0; i < a.path.elementCount(); ++i) {
            const auto element = a.path.elementAt(i);
            if (!std::isfinite(element.x) || !std::isfinite(element.y)) {
                work.error = QStringLiteral("An annotation contains invalid coordinates."); return false;
            }
        }
    }
    return true;
}

QImage applyEdits(QImage image, const ImageEdits &edits, const ImageOptions &options, WorkContext &work)
{
    if (!validEdits(edits, image.size(), work)) return {};
    if (!edits.annotations.isEmpty()) {
        // QPainter does not support indexed/CMYK targets; retain 16-bit channels
        // for high-depth images instead of converting them all to RGB32.
        const auto source = image;
        image = image.convertToFormat(image.depth() > 32 ? QImage::Format_RGBA64 : QImage::Format_ARGB32);
        copyMetadata(source, image);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        for (const auto &a : edits.annotations) {
            if (!work.proceed()) return {};
            paintAnnotation(painter, a);
        }
    }
    if (!edits.crop.isNull()) {
        auto cropped = image.copy(edits.crop); copyMetadata(image, cropped); image = std::move(cropped);
    }
    const int turns = ((edits.quarterTurns % 4) + 4) % 4;
    if (turns) {
        auto rotated = image.transformed(QTransform().rotate(turns * 90), Qt::FastTransformation);
        copyMetadata(image, rotated, turns % 2); image = std::move(rotated);
    }
    if (options.maximumEdge > 0 && qMax(image.width(), image.height()) > options.maximumEdge) {
        auto scaled = image.scaled(options.maximumEdge, options.maximumEdge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        copyMetadata(image, scaled, false, qreal(scaled.width()) / image.width()); image = std::move(scaled);
    }
    if ((options.format == "jpeg" || options.format == "bmp") && image.hasAlphaChannel()) {
        QImage flattened(image.size(), image.depth() > 32 ? QImage::Format_RGBX64 : QImage::Format_RGB32);
        flattened.fill(options.alphaBackground);
        { QPainter painter(&flattened); painter.drawImage(QPoint(), image); }
        copyMetadata(image, flattened); image = std::move(flattened);
    }
    if (!work.proceed()) return {};
    return image;
}

QString destinationError(const QList<QUrl> &sources, const QList<QUrl> &destinations)
{
    QSet<QUrl> seen;
    for (const auto &target : destinations) {
        const auto normalized = target.adjusted(QUrl::NormalizePathSegments);
        if (!target.isValid() || target.isEmpty() || target.isRelative() || target.fileName().isEmpty()
            || target.hasQuery() || target.hasFragment() || seen.contains(normalized))
            return QStringLiteral("Choose distinct file destinations with valid absolute URLs.");
        seen.insert(normalized);
        for (const auto &source : sources) {
            if (normalized == source.adjusted(QUrl::NormalizePathSegments))
                return QStringLiteral("Quick Actions save copies. Choose a destination different from the source.");
            if (source.isLocalFile() && target.isLocalFile()) {
                struct stat sourceStat{}, targetStat{};
                const auto sourcePath = QFile::encodeName(source.toLocalFile()), targetPath = QFile::encodeName(target.toLocalFile());
                if (::stat(sourcePath.constData(), &sourceStat) == 0 && ::stat(targetPath.constData(), &targetStat) == 0
                    && sourceStat.st_dev == targetStat.st_dev && sourceStat.st_ino == targetStat.st_ino)
                    return QStringLiteral("The destination refers to a selected source. Choose a new copy name.");
            }
        }
    }
    return {};
}

struct ExportResult {
    QString error;
    QStringList staged;
    std::shared_ptr<QTemporaryDir> temporary;
};

ExportResult exportImageWork(const QList<QUrl> &sources, const QList<QUrl> &targets,
                             const ImageOptions &options, const ImageEdits &edits,
                             const std::shared_ptr<std::atomic_bool> &cancelled)
{
    WorkContext work(cancelled);
    ExportResult result;
    result.error = destinationError(sources, targets);
    if (!result.error.isEmpty()) return result;
    result.temporary = std::make_shared<QTemporaryDir>();
    if (!result.temporary->isValid()) { result.error = QStringLiteral("Could not create private output staging."); return result; }
    for (int i = 0; i < sources.size(); ++i) {
        auto image = readImage(sources[i], work);
        if (image.isNull()) break;
        image = applyEdits(std::move(image), edits, options, work);
        if (image.isNull()) break;
        const auto path = result.temporary->filePath(QString::number(i) + '.' + QString::fromLatin1(options.format));
        BoundedFile file(path, work);
        QImageWriter writer(&file, options.format);
        writer.setQuality(options.quality);
        if (!file.isOpen() || !writer.write(image) || !file.good()) {
            work.error = !file.errorString().isEmpty() && file.errorString() != "Unknown error"
                ? file.errorString() : writer.errorString(); break;
        }
        if (!work.proceed()) break;
        result.staged.append(path);
    }
    result.error = work.error;
    return result;
}

ExportResult exportPdfWork(const QList<QUrl> &sources, const QUrl &target, const PdfOptions &options,
                           const std::shared_ptr<std::atomic_bool> &cancelled)
{
    WorkContext work(cancelled);
    ExportResult result;
    result.error = destinationError(sources, {target});
    if (!result.error.isEmpty()) return result;
    result.temporary = std::make_shared<QTemporaryDir>();
    if (!result.temporary->isValid()) { result.error = QStringLiteral("Could not create private PDF staging."); return result; }
    const auto path = result.temporary->filePath("output.pdf");
    BoundedFile file(path, work);
    if (!file.isOpen()) { result.error = file.errorString(); return result; }
    int written = 0;
    qint64 pixels = 0;
    {
        QPdfWriter writer(&file);
        writer.setResolution(options.rasterDpi);
        writer.setCreator(QStringLiteral("Finder for Linux Quick Actions"));
        writer.setTitle(options.title);
        QPainter painter;
        auto page = [&](const QImage &image, const QSizeF &points) {
            if (!work.proceed()) return false;
            if (++written > pageLimit || (pixels += qint64(image.width()) * image.height()) > totalPdfPixels) {
                work.error = QStringLiteral("PDF creation is limited to 200 pages and 256 million total raster pixels."); return false;
            }
            if (!points.isValid() || points.width() > 14400 || points.height() > 14400
                || !std::isfinite(points.width()) || !std::isfinite(points.height())) {
                work.error = QStringLiteral("A PDF page has unsupported dimensions."); return false;
            }
            if (!writer.setPageSize(QPageSize(points, QPageSize::Point, QString(), QPageSize::ExactMatch))
                || !writer.setPageMargins(QMarginsF(0, 0, 0, 0), QPageLayout::Point)
                || (written == 1 ? !painter.begin(&writer) : !writer.newPage())) {
                work.error = QStringLiteral("Could not create the PDF page."); return false;
            }
            // White paper composites transparent PDF/image pixels deliberately.
            painter.fillRect(QRect(0, 0, writer.width(), writer.height()), Qt::white);
            painter.drawImage(QRect(0, 0, writer.width(), writer.height()), image);
            return work.proceed();
        };
        for (const auto &source : sources) {
            if (!work.proceed()) break;
            if (!pdfUrl(source)) {
                const QImage image = readImage(source, work);
                if (image.isNull()) break;
                const qreal dx = image.dotsPerMeterX() > 0 ? image.dotsPerMeterX() * 0.0254 : 72;
                const qreal dy = image.dotsPerMeterY() > 0 ? image.dotsPerMeterY() * 0.0254 : 72;
                if (!page(image, QSizeF(image.width() * 72.0 / dx, image.height() * 72.0 / dy))) break;
            } else {
                const auto sourcePath = localSource(source, work);
                if (sourcePath.isEmpty()) break;
                QPdfDocument pdf;
                const auto error = pdf.load(sourcePath);
                if (error != QPdfDocument::Error::None || pdf.status() != QPdfDocument::Status::Ready) {
                    work.error = error == QPdfDocument::Error::IncorrectPassword
                        ? QStringLiteral("This PDF requires a password. Export an unlocked copy first.")
                        : QStringLiteral("The PDF reader could not open this document."); break;
                }
                if (pdf.pageCount() <= 0 || pdf.pageCount() > pageLimit - written) {
                    work.error = QStringLiteral("The combined PDF exceeds the 200-page limit."); break;
                }
                QPdfDocumentRenderOptions renderOptions;
                renderOptions.setRenderFlags(QPdfDocumentRenderOptions::RenderFlag::Annotations);
                for (int p = 0; p < pdf.pageCount(); ++p) {
                    if (!work.proceed()) break;
                    const auto points = pdf.pagePointSize(p);
                    const qreal w = std::ceil(points.width() * options.rasterDpi / 72.0);
                    const qreal h = std::ceil(points.height() * options.rasterDpi / 72.0);
                    if (!std::isfinite(w) || !std::isfinite(h) || w < 1 || h < 1 || w * h > pdfPixelLimit) {
                        work.error = QStringLiteral("A PDF page exceeds 16 million raster pixels. Use a lower PDF resolution."); break;
                    }
                    const auto raster = pdf.render(p, QSize(int(w), int(h)), renderOptions);
                    if (raster.isNull()) { work.error = QStringLiteral("The PDF reader could not render a page."); break; }
                    if (!page(raster, points)) break;
                }
                if (!work.error.isEmpty()) break;
            }
        }
        if (painter.isActive() && !painter.end() && work.error.isEmpty()) work.error = QStringLiteral("Could not finalize the PDF.");
    }
    if ((!file.good() || written == 0) && work.error.isEmpty()) work.error = QStringLiteral("Could not write the staged PDF.");
    if (work.error.isEmpty() && work.proceed()) result.staged.append(path);
    result.error = work.error;
    return result;
}

QUrl proposedUrl(const QUrl &source, const QString &suffix, const QByteArray &format)
{
    QUrl target = source.adjusted(QUrl::RemoveFilename | QUrl::StripTrailingSlash);
    auto path = target.path();
    if (!path.endsWith('/')) path += '/';
    target.setPath(path + QFileInfo(source.path()).completeBaseName() + suffix + '.' + QString::fromLatin1(format));
    return target;
}

QDialog *makeDialog(QWidget *parent, const QString &title)
{
    auto *dialog = new QDialog(parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(title);
    dialog->setWindowModality(Qt::WindowModal);
    return dialog;
}
}

QuickActions::QuickActions(QWidget *parent) : QObject(parent), m_parent(parent), m_state(std::make_shared<QuickActionsState>()) {}
QuickActions::~QuickActions()
{
    if (m_state->cancelled) m_state->cancelled->store(true);
    if (m_state->job) m_state->job->kill(KJob::Quietly);
    if (m_dialog) delete m_dialog;
    if (m_progress) delete m_progress;
}

QList<QByteArray> QuickActions::writableImageFormats()
{
    QList<QByteArray> formats;
    const auto supported = QImageWriter::supportedImageFormats();
    for (const auto &format : {QByteArray("png"), QByteArray("jpeg"), QByteArray("webp"), QByteArray("tiff"), QByteArray("bmp")})
        if (supported.contains(format)) formats.append(format);
    return formats;
}

QList<ActionInfo> QuickActions::actionsForUrls(const QList<QUrl> &urls)
{
    bool images = !urls.isEmpty() && urls.size() <= inputCountLimit;
    bool pdfs = images;
    for (const auto &url : urls) { images &= imageUrl(url); pdfs &= imageUrl(url) || pdfUrl(url); }
    const bool writable = !writableImageFormats().isEmpty();
    const auto reason = QStringLiteral("Select up to 64 local single-frame images. Sources are validated before export.");
    return {
        {Action::ConvertImage, tr("Convert Image…"), images && writable, reason},
        {Action::CreatePdf, urls.size() > 1 ? tr("Combine into PDF…") : tr("Create PDF…"), pdfs,
            tr("Images and PDFs in selection order. Existing PDFs are rasterized with Qt's public reader.")},
        {Action::Markup, tr("Markup / Crop…"), urls.size() == 1 && images && writable, reason},
        {Action::RotateLeft, tr("Rotate Left as Copy…"), images && writable, reason},
        {Action::RotateRight, tr("Rotate Right as Copy…"), images && writable, reason}
    };
}

void QuickActions::populateMenu(QMenu *menu, const QList<QUrl> &urls)
{
    if (!menu) return;
    for (const auto &info : actionsForUrls(urls)) {
        auto *action = menu->addAction(info.text);
        action->setObjectName(QStringLiteral("quickAction%1").arg(int(info.action)));
        action->setEnabled(info.enabled && !busy());
        action->setToolTip(info.explanation);
        connect(this, &QuickActions::busyChanged, action, [action, allowed = info.enabled](bool active) { action->setEnabled(allowed && !active); });
        connect(action, &QAction::triggered, this, [this, urls, id = info.action] { trigger(id, urls); });
    }
}
bool QuickActions::busy() const { return m_state->busy; }
void QuickActions::setBusy(bool busy)
{
    if (m_state->busy == busy) return;
    m_state->busy = busy;
    emit busyChanged(busy);
}

void QuickActions::exportImages(const QList<QUrl> &sources, const QList<QUrl> &destinations,
                               const ImageOptions &options, const ImageEdits &edits)
{
    if (busy()) { emit errorOccurred(tr("A Quick Action is already running.")); return; }
    if (sources.isEmpty() || sources.size() > inputCountLimit || sources.size() != destinations.size()
        || !writableImageFormats().contains(options.format) || options.maximumEdge < 0 || options.maximumEdge > 32768
        || options.quality < 0 || options.quality > 100 || !options.alphaBackground.isValid()) {
        emit errorOccurred(tr("The image selection, destination count, format or size is invalid.")); return;
    }
    m_state->cancelled = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = m_state->cancelled;
    const auto generation = ++m_state->generation;
    m_state->destinations = destinations;
    m_state->published.clear(); m_state->next = 0;
    setBusy(true);
    showProgress();
    emit statusChanged(tr("Preparing full-resolution image copies…"));
    emit progressChanged(0, sources.size());
    submit(this, [sources, destinations, options, edits, cancelled] {
        return exportImageWork(sources, destinations, options, edits, cancelled);
    }, [this, generation](const ExportResult &result) {
        if (generation != m_state->generation) return;
        if (m_state->cancelled->load()) { complete(true); return; }
        if (!result.error.isEmpty()) { complete(false, result.error); return; }
        m_state->temporary = result.temporary; m_state->staged = result.staged;
        publishNext();
    });
}

void QuickActions::createPdf(const QList<QUrl> &sources, const QUrl &destination, const PdfOptions &options)
{
    if (busy()) { emit errorOccurred(tr("A Quick Action is already running.")); return; }
    if (sources.isEmpty() || sources.size() > inputCountLimit || options.rasterDpi < 72 || options.rasterDpi > 300) {
        emit errorOccurred(tr("Select 1–64 files and a PDF resolution from 72 to 300 DPI.")); return;
    }
    m_state->cancelled = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = m_state->cancelled;
    const auto generation = ++m_state->generation;
    m_state->destinations = {destination};
    m_state->published.clear(); m_state->next = 0;
    setBusy(true);
    showProgress();
    emit statusChanged(tr("Creating PDF pages in selection order…"));
    emit progressChanged(0, 1);
    submit(this, [sources, destination, options, cancelled] { return exportPdfWork(sources, destination, options, cancelled); },
        [this, generation](const ExportResult &result) {
            if (generation != m_state->generation) return;
            if (m_state->cancelled->load()) { complete(true); return; }
            if (!result.error.isEmpty()) { complete(false, result.error); return; }
            m_state->temporary = result.temporary; m_state->staged = result.staged;
            publishNext();
        });
}

void QuickActions::publishNext()
{
    if (m_state->cancelled->load()) { complete(true); return; }
    if (m_state->next >= m_state->staged.size()) { complete(false); return; }
    const int index = m_state->next;
    emit statusChanged(tr("Saving copy %1 of %2…").arg(index + 1).arg(m_state->staged.size()));
    // No Overwrite flag. KIO handles collisions, rename, skip and cancellation.
    auto *job = KIO::copyAs(QUrl::fromLocalFile(m_state->staged[index]), m_state->destinations[index]);
    m_state->job = job;
    job->setUiDelegate(new FinderJobUiDelegate(KJobUiDelegate::AutoHandlingDisabled, m_parent));
    KIO::FileUndoManager::self()->recordCopyJob(job);
    auto actual = std::make_shared<QUrl>();
    connect(job, &KIO::CopyJob::copyingDone, this,
        [actual](KIO::Job *, const QUrl &, const QUrl &to, const QDateTime &, bool, bool) { *actual = to; });
    // Retain staging until KIO has closed its source even if the controller dies.
    connect(job, &KJob::result, job, [temporary = m_state->temporary] { Q_UNUSED(temporary); });
    connect(job, &KJob::result, this, [this, actual](KJob *finished) {
        m_state->job = nullptr;
        if (!actual->isEmpty()) {
            m_state->published.append(*actual); emit outputCreated(*actual);
        }
        if (m_state->cancelled->load() || finished->error() == KJob::KilledJobError) { complete(true); return; }
        if (finished->error()) { complete(false, finished->errorText()); return; }
        ++m_state->next;
        emit progressChanged(m_state->next, m_state->staged.size());
        publishNext();
    });
}

void QuickActions::complete(bool cancelled, const QString &error)
{
    if (!busy()) return;
    const auto outputs = m_state->published;
    m_state->temporary.reset(); m_state->staged.clear(); m_state->job = nullptr;
    if (m_progress) { m_progress->accept(); m_progress = nullptr; }
    setBusy(false);
    if (!error.isEmpty()) emit errorOccurred(error);
    emit statusChanged(cancelled ? tr("Quick Action cancelled. %1 copies saved.").arg(outputs.size())
        : !error.isEmpty() ? error : tr("%1 copies saved.").arg(outputs.size()));
    if (!outputs.isEmpty()) emit outputsCreated(outputs);
    emit finished(cancelled);
}

void QuickActions::cancel()
{
    if (!busy()) return;
    if (m_state->cancelled) m_state->cancelled->store(true);
    emit statusChanged(tr("Cancelling Quick Action…"));
    // kill(EmitResult) can complete synchronously; publish cancellation status
    // before it so the final status cannot be replaced with "Cancelling".
    if (m_state->job) m_state->job->kill(KJob::EmitResult);
}

void QuickActions::showProgress()
{
    auto *dialog = makeDialog(m_parent, tr("Quick Action")); m_progress = dialog;
    dialog->setObjectName(QStringLiteral("quickActionProgressDialog"));
    auto *layout = new QVBoxLayout(dialog);
    auto *label = new QLabel(dialog); label->setWordWrap(true); layout->addWidget(label);
    auto *progress = new QProgressBar(dialog); progress->setRange(0, 0); layout->addWidget(progress);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, dialog); layout->addWidget(buttons);
    connect(this, &QuickActions::statusChanged, label, &QLabel::setText);
    connect(this, &QuickActions::progressChanged, progress, [progress](int completed, int total) {
        // Export is indeterminate until complete images/pages are staged.
        if (completed > 0) { progress->setRange(0, total); progress->setValue(completed); }
    });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(dialog, &QDialog::rejected, this, &QuickActions::cancel);
    dialog->resize(420, 150); dialog->open();
}

ImageCanvas::ImageCanvas(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("quickActionCanvas"));
    setMinimumSize(320, 240);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(tr("Image editing canvas"));
    setToolTip(tr("Draw or drag shapes and crops. Select and drag an annotation to move it; Delete removes it. Ctrl+Z undoes."));
}
void ImageCanvas::setImage(const QImage &preview, const QSize &originalSize)
{
    m_preview = preview; m_original = originalSize; m_edits = {}; m_history.clear(); m_selected = -1;
    update(); emit editsChanged();
}
void ImageCanvas::setEdits(const ImageEdits &edits)
{
    m_edits = edits; m_history.clear(); m_selected = -1; update(); emit editsChanged();
}
ImageEdits ImageCanvas::edits() const { return m_edits; }
QSize ImageCanvas::originalSize() const { return m_original; }
void ImageCanvas::setTool(Tool tool) { m_tool = tool; m_dragging = false; update(); }
void ImageCanvas::setColor(const QColor &color) { if (color.isValid()) m_color = color; }
void ImageCanvas::setStrokeWidth(qreal width) { m_width = qBound(qreal(1), width, qreal(256)); }
void ImageCanvas::setText(const QString &text) { m_text = text.left(4096); }
QRect ImageCanvas::cropRect() const { return m_edits.crop.isNull() ? QRect(QPoint(), m_original) : m_edits.crop; }
void ImageCanvas::remember()
{
    m_history.append(m_edits);
    if (m_history.size() > 32) m_history.removeFirst();
}
void ImageCanvas::undo()
{
    if (m_history.isEmpty()) return;
    m_edits = m_history.takeLast(); m_selected = -1; m_dragging = false; update(); emit editsChanged();
}
void ImageCanvas::rotate(int turns)
{
    remember(); m_edits.quarterTurns = (m_edits.quarterTurns + turns) % 4;
    update(); emit editsChanged();
}
void ImageCanvas::resetCrop()
{
    if (m_edits.crop.isNull()) return;
    remember(); m_edits.crop = {}; update(); emit editsChanged();
}
QTransform ImageCanvas::imageToWidget() const
{
    const auto crop = cropRect();
    if (crop.isEmpty()) return {};
    const int turns = ((m_edits.quarterTurns % 4) + 4) % 4;
    const QSize rotated = turns % 2 ? crop.size().transposed() : crop.size();
    const qreal scale = qMin(qreal(qMax(1, width() - 24)) / rotated.width(), qreal(qMax(1, height() - 24)) / rotated.height());
    QTransform transform;
    transform.translate((width() - rotated.width() * scale) / 2, (height() - rotated.height() * scale) / 2);
    transform.scale(scale, scale);
    if (turns == 1) transform.translate(crop.height(), 0);
    else if (turns == 2) transform.translate(crop.width(), crop.height());
    else if (turns == 3) transform.translate(0, crop.width());
    transform.rotate(turns * 90);
    transform.translate(-crop.x(), -crop.y());
    return transform;
}
QPointF ImageCanvas::sourcePoint(const QPointF &point) const
{
    const auto p = imageToWidget().inverted().map(point);
    const auto crop = cropRect();
    return QPointF(qBound(qreal(crop.x()), p.x(), qreal(crop.x() + crop.width())),
                   qBound(qreal(crop.y()), p.y(), qreal(crop.y() + crop.height())));
}
void ImageCanvas::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), palette().brush(QPalette::Base));
    if (m_preview.isNull() || m_original.isEmpty()) {
        painter.drawText(rect(), Qt::AlignCenter, tr("Loading image…")); return;
    }
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    painter.setTransform(imageToWidget());
    painter.setClipRect(cropRect());
    painter.fillRect(cropRect(), Qt::white);
    painter.drawImage(QRectF(QPointF(), QSizeF(m_original)), m_preview);
    for (const auto &a : m_edits.annotations) paintAnnotation(painter, a);
    if (m_selected >= 0 && m_selected < m_edits.annotations.size()) {
        const auto &a = m_edits.annotations[m_selected];
        painter.setPen(QPen(palette().color(QPalette::Highlight), 0, Qt::DashLine));
        painter.drawRect(a.kind == Annotation::Kind::Draw ? a.path.boundingRect() : a.rect);
    }
    if (m_tool == Tool::Crop && m_dragging) {
        painter.setPen(QPen(palette().color(QPalette::Highlight), 0, Qt::DashLine));
        painter.fillRect(m_pendingCrop, QColor(80, 140, 220, 45)); painter.drawRect(m_pendingCrop);
    }
}
void ImageCanvas::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || m_preview.isNull()
        || !imageToWidget().mapRect(QRectF(cropRect())).contains(event->position())) {
        QWidget::mousePressEvent(event); return;
    }
    setFocus();
    m_start = m_last = sourcePoint(event->position());
    m_selected = -1;
    if (m_tool == Tool::Select) {
        const qreal tolerance = 8 / qMax(qreal(0.001), std::hypot(imageToWidget().m11(), imageToWidget().m12()));
        for (int i = m_edits.annotations.size() - 1; i >= 0; --i) {
            const auto &a = m_edits.annotations[i];
            const auto box = a.kind == Annotation::Kind::Draw ? a.path.boundingRect() : a.rect;
            if (box.adjusted(-tolerance, -tolerance, tolerance, tolerance).contains(m_start)) {
                m_selected = i; remember(); m_dragging = true; break;
            }
        }
    } else if (m_tool == Tool::Crop) {
        m_pendingCrop = QRectF(m_start, m_start); m_dragging = true; remember();
    } else if (m_edits.annotations.size() < 512 && (m_tool != Tool::Text || !m_text.isEmpty())) {
        remember();
        Annotation a;
        a.color = m_color; a.width = m_width;
        a.kind = m_tool == Tool::Draw ? Annotation::Kind::Draw : m_tool == Tool::Rectangle
            ? Annotation::Kind::Rectangle : m_tool == Tool::Ellipse ? Annotation::Kind::Ellipse : Annotation::Kind::Text;
        a.path.moveTo(m_start);
        a.rect = QRectF(m_start, m_start);
        a.text = m_text; a.textSize = qMax(qreal(12), m_width * 6);
        if (m_tool == Tool::Text) {
            a.rect = QRectF(m_start, QSizeF(qMax(qreal(120), a.textSize * 12), a.textSize * 4));
        }
        m_edits.annotations.append(a); m_selected = m_edits.annotations.size() - 1;
        m_dragging = m_tool != Tool::Text;
    }
    update(); emit editsChanged(); event->accept();
}
void ImageCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging) { QWidget::mouseMoveEvent(event); return; }
    const auto point = sourcePoint(event->position());
    if (m_tool == Tool::Crop) m_pendingCrop = QRectF(m_start, point).normalized();
    else if (m_selected >= 0 && m_selected < m_edits.annotations.size()) {
        auto &a = m_edits.annotations[m_selected];
        if (m_tool == Tool::Select) { a.path.translate(point - m_last); a.rect.translate(point - m_last); }
        else if (m_tool == Tool::Draw) {
            if (a.path.elementCount() < 4096) a.path.lineTo(point);
        } else a.rect = QRectF(m_start, point).normalized();
    }
    m_last = point; update(); event->accept();
}
void ImageCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_dragging || event->button() != Qt::LeftButton) { QWidget::mouseReleaseEvent(event); return; }
    mouseMoveEvent(event);
    if (m_tool == Tool::Crop) {
        // Round user-chosen pixel boundaries; floor/ceil can grow a crop by a
        // pixel because the inverse rotated preview transform has roundoff.
        const QRect rounded(qRound(m_pendingCrop.x()), qRound(m_pendingCrop.y()),
                            qRound(m_pendingCrop.width()), qRound(m_pendingCrop.height()));
        const auto crop = rounded.intersected(cropRect());
        if (crop.width() > 1 && crop.height() > 1) m_edits.crop = crop;
    }
    m_dragging = false; update(); emit editsChanged(); event->accept();
}
void ImageCanvas::keyPressEvent(QKeyEvent *event)
{
    if (event->matches(QKeySequence::Undo)) { undo(); event->accept(); return; }
    if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)
        && m_selected >= 0 && m_selected < m_edits.annotations.size()) {
        remember(); m_edits.annotations.removeAt(m_selected); m_selected = -1; update(); emit editsChanged(); event->accept(); return;
    }
    if (event->key() == Qt::Key_Escape && m_dragging) { undo(); event->accept(); return; }
    QWidget::keyPressEvent(event);
}

void QuickActions::trigger(Action action, const QList<QUrl> &urls)
{
    if (busy() || m_dialog) { emit errorOccurred(tr("Finish or close the current Quick Action first.")); return; }
    const auto actions = actionsForUrls(urls);
    const auto found = std::find_if(actions.cbegin(), actions.cend(), [action](const ActionInfo &info) { return info.action == action; });
    if (found == actions.cend() || !found->enabled) { emit errorOccurred(tr("This Quick Action does not support the selection.")); return; }
    switch (action) {
    case Action::ConvertImage: showConversion(urls); break;
    case Action::CreatePdf: showPdf(urls); break;
    case Action::Markup: showEditor(urls.first()); break;
    case Action::RotateLeft:
    case Action::RotateRight: {
        ImageOptions options;
        const auto formats = writableImageFormats();
        // A decoded JPEG rotated into JPEG would add a lossy generation.
        // Save decoded source pixels in a lossless container by default.
        options.format = formatFor(urls.first()) == "tiff" && formats.contains("tiff") ? QByteArray("tiff")
            : formats.contains("png") ? QByteArray("png") : formats.first();
        ImageEdits edits; edits.quarterTurns = action == Action::RotateLeft ? -1 : 1;
        chooseImageDestination(urls, options, edits, tr(" rotated")); break;
    }
    }
}

void QuickActions::showConversion(const QList<QUrl> &sources)
{
    auto *dialog = makeDialog(m_parent, tr("Convert Image")); m_dialog = dialog;
    dialog->setObjectName(QStringLiteral("quickActionConvertDialog"));
    auto *layout = new QVBoxLayout(dialog);
    auto *form = new QFormLayout;
    auto *format = new QComboBox(dialog); format->setObjectName(QStringLiteral("quickActionFormat"));
    for (const auto &f : writableImageFormats()) format->addItem(QString::fromLatin1(f).toUpper(), f);
    auto *size = new QComboBox(dialog); size->setObjectName(QStringLiteral("quickActionSize"));
    size->addItem(tr("Original size"), 0); size->addItem(tr("Small — 640 px"), 640);
    size->addItem(tr("Medium — 1280 px"), 1280); size->addItem(tr("Large — 2560 px"), 2560); size->addItem(tr("Custom maximum edge"), -1);
    auto *edge = new QSpinBox(dialog); edge->setRange(1, 32768); edge->setValue(1920); edge->setEnabled(false);
    auto *quality = new QSpinBox(dialog); quality->setRange(0, 100); quality->setValue(95);
    form->addRow(tr("Format:"), format); form->addRow(tr("Size:"), size); form->addRow(tr("Maximum edge (pixels):"), edge);
    form->addRow(tr("Quality (lossy formats):"), quality); layout->addLayout(form);
    auto *note = new QLabel(tr("Creates copies. Original size keeps source pixels. PNG/TIFF can retain high-depth color when supported by the installed codec. JPEG is lossy and uses white for transparency. Color profiles, resolution and text metadata are passed to codecs; EXIF, XMP and other metadata may be lost."), dialog);
    note->setWordWrap(true); layout->addWidget(note);
    connect(size, &QComboBox::currentIndexChanged, edge, [size, edge] { edge->setEnabled(size->currentData().toInt() < 0); });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, dialog); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this, dialog, sources, format, size, edge, quality] {
        ImageOptions options;
        options.format = format->currentData().toByteArray(); options.quality = quality->value();
        options.maximumEdge = size->currentData().toInt() < 0 ? edge->value() : size->currentData().toInt();
        m_dialog = nullptr; dialog->accept(); chooseImageDestination(sources, options, {}, tr(" converted"));
    });
    dialog->open();
}

void QuickActions::chooseImageDestination(const QList<QUrl> &sources, const ImageOptions &options,
                                         const ImageEdits &edits, const QString &suffix)
{
    auto *picker = new QFileDialog(m_parent, tr("Save Image Copies")); m_dialog = picker;
    picker->setAttribute(Qt::WA_DeleteOnClose);
    picker->setWindowModality(Qt::WindowModal);
    if (sources.size() == 1) {
        picker->setAcceptMode(QFileDialog::AcceptSave); picker->setFileMode(QFileDialog::AnyFile);
        picker->setNameFilter(QStringLiteral("%1 (*.%2)").arg(QString::fromLatin1(options.format).toUpper(), QString::fromLatin1(options.format)));
        picker->setDefaultSuffix(QString::fromLatin1(options.format));
        picker->selectUrl(proposedUrl(sources.first(), suffix, options.format));
    } else {
        picker->setFileMode(QFileDialog::Directory);
        picker->setDirectoryUrl(sources.first().adjusted(QUrl::RemoveFilename));
    }
    connect(picker, &QDialog::accepted, this, [this, picker, sources, options, edits, suffix] {
        const auto selected = picker->selectedUrls(); m_dialog = nullptr;
        if (selected.isEmpty()) return;
        QList<QUrl> targets;
        if (sources.size() == 1) targets = {selected.first()};
        else {
            QSet<QString> names;
            for (const auto &source : sources) {
                const auto stem = QFileInfo(source.path()).completeBaseName() + suffix;
                auto name = stem + '.' + QString::fromLatin1(options.format);
                int number = 2;
                while (names.contains(name)) name = stem + ' ' + QString::number(number++) + '.' + QString::fromLatin1(options.format);
                names.insert(name);
                auto target = selected.first();
                auto path = target.path(); if (!path.endsWith('/')) path += '/'; target.setPath(path + name); targets.append(target);
            }
        }
        exportImages(sources, targets, options, edits);
    });
    picker->open();
}

void QuickActions::showPdf(const QList<QUrl> &sources)
{
    auto *dialog = makeDialog(m_parent, tr("Create / Combine PDF")); m_dialog = dialog;
    dialog->setObjectName(QStringLiteral("quickActionPdfDialog"));
    auto *layout = new QVBoxLayout(dialog);
    auto *note = new QLabel(tr("One image or all pages of each PDF, in the order below. Drag items to reorder. Existing PDFs are rasterized: text search, vectors, links, forms, signatures and editable annotations are not retained. Page dimensions are preserved; images keep their source resolution. Encrypted PDFs require an unlocked copy. Limits: 64 files, 200 pages, 16 million pixels per PDF page."), dialog);
    note->setWordWrap(true); layout->addWidget(note);
    auto *order = new QListWidget(dialog); order->setObjectName(QStringLiteral("quickActionPdfOrder"));
    order->setDragDropMode(QAbstractItemView::InternalMove); order->setSelectionMode(QAbstractItemView::SingleSelection);
    for (const auto &source : sources) { auto *item = new QListWidgetItem(source.fileName(), order); item->setData(Qt::UserRole, source); item->setToolTip(source.toDisplayString()); }
    layout->addWidget(order);
    auto *form = new QFormLayout;
    auto *title = new QLineEdit(dialog); title->setText(sources.size() == 1 ? QFileInfo(sources.first().path()).completeBaseName() : tr("Combined PDF"));
    auto *dpi = new QComboBox(dialog); dpi->addItem(tr("72 DPI"), 72); dpi->addItem(tr("150 DPI"), 150); dpi->addItem(tr("300 DPI"), 300); dpi->setCurrentIndex(1);
    form->addRow(tr("Title:"), title); form->addRow(tr("Existing PDF resolution:"), dpi); layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, dialog); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this, dialog, sources, title, dpi, order] {
        PdfOptions options; options.title = title->text().left(4096); options.rasterDpi = dpi->currentData().toInt();
        QList<QUrl> sorted;
        for (int i = 0; i < order->count(); ++i) sorted.append(order->item(i)->data(Qt::UserRole).toUrl());
        m_dialog = nullptr; dialog->accept();
        auto *picker = new QFileDialog(m_parent, tr("Save PDF Copy")); m_dialog = picker;
        picker->setAttribute(Qt::WA_DeleteOnClose); picker->setWindowModality(Qt::WindowModal);
        picker->setAcceptMode(QFileDialog::AcceptSave); picker->setFileMode(QFileDialog::AnyFile);
        picker->setNameFilter(tr("PDF documents (*.pdf)")); picker->setDefaultSuffix(QStringLiteral("pdf"));
        picker->selectUrl(proposedUrl(sources.first(), sources.size() > 1 ? tr(" combined") : tr(" copy"), "pdf"));
        connect(picker, &QDialog::accepted, this, [this, picker, sorted, options] {
            const auto urls = picker->selectedUrls(); m_dialog = nullptr;
            if (!urls.isEmpty()) createPdf(sorted, urls.first(), options);
        });
        picker->open();
    });
    dialog->resize(540, 500); dialog->open();
}

void QuickActions::showEditor(const QUrl &source)
{
    auto *dialog = makeDialog(m_parent, tr("Markup / Crop Image")); m_dialog = dialog;
    dialog->setObjectName(QStringLiteral("quickActionEditorDialog"));
    auto *layout = new QVBoxLayout(dialog);
    auto *toolbar = new QHBoxLayout;
    auto *tool = new QComboBox(dialog); tool->setObjectName(QStringLiteral("quickActionTool"));
    tool->addItems({tr("Select / Move"), tr("Draw / Signature"), tr("Rectangle"), tr("Ellipse"), tr("Text"), tr("Crop")}); tool->setCurrentIndex(1);
    auto *color = new QPushButton(tr("Color…"), dialog);
    auto *width = new QSpinBox(dialog); width->setRange(1, 256); width->setValue(4); width->setSuffix(tr(" px"));
    auto *left = new QPushButton(tr("Rotate Left"), dialog), *right = new QPushButton(tr("Rotate Right"), dialog);
    auto *undo = new QPushButton(tr("Undo Edit"), dialog), *crop = new QPushButton(tr("Reset Crop"), dialog);
    for (QWidget *widget : QList<QWidget *>{tool, color, width, left, right, undo, crop}) toolbar->addWidget(widget);
    layout->addLayout(toolbar);
    auto *text = new QLineEdit(dialog); text->setPlaceholderText(tr("Text to place on the canvas (select Text, then click)")); text->setMaxLength(4096); layout->addWidget(text);
    auto *canvas = new ImageCanvas(dialog); canvas->setEnabled(false); layout->addWidget(canvas, 1);
    auto *status = new QLabel(tr("Loading a bounded preview. Saved edits use the original full-resolution pixels."), dialog); status->setWordWrap(true); layout->addWidget(status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, dialog); layout->addWidget(buttons);
    buttons->button(QDialogButtonBox::Save)->setEnabled(false);
    connect(tool, &QComboBox::currentIndexChanged, canvas, [tool, canvas] { canvas->setTool(ImageCanvas::Tool(tool->currentIndex())); });
    connect(width, &QSpinBox::valueChanged, canvas, [canvas](int value) { canvas->setStrokeWidth(value); });
    connect(text, &QLineEdit::textChanged, canvas, &ImageCanvas::setText);
    connect(left, &QPushButton::clicked, canvas, [canvas] { canvas->rotate(-1); });
    connect(right, &QPushButton::clicked, canvas, [canvas] { canvas->rotate(1); });
    connect(undo, &QPushButton::clicked, canvas, &ImageCanvas::undo);
    connect(crop, &QPushButton::clicked, canvas, &ImageCanvas::resetCrop);
    connect(color, &QPushButton::clicked, dialog, [canvas, dialog] {
        auto *picker = new QColorDialog(dialog); picker->setAttribute(Qt::WA_DeleteOnClose);
        picker->setOption(QColorDialog::ShowAlphaChannel); picker->setWindowModality(Qt::WindowModal);
        QObject::connect(picker, &QColorDialog::colorSelected, canvas, &ImageCanvas::setColor); picker->open();
    });
    auto cancelled = m_state->cancelled = std::make_shared<std::atomic_bool>(false);
    const auto generation = ++m_state->generation;
    setBusy(true); emit statusChanged(tr("Loading image editor…"));
    struct Preview { QImage image; QSize original; QString error; };
    QPointer<QDialog> guard(dialog);
    connect(dialog, &QDialog::rejected, this, [cancelled] { cancelled->store(true); });
    submit(this, [source, cancelled] {
        WorkContext work(cancelled); Preview result;
        result.image = readImage(source, work, true, &result.original); result.error = work.error;
        return result;
    }, [this, generation, guard, canvas, status, buttons](const Preview &result) {
        if (generation != m_state->generation) return;
        setBusy(false);
        if (m_state->cancelled->load()) { emit finished(true); return; }
        if (!guard) { emit finished(true); return; }
        if (!result.error.isEmpty()) { status->setText(result.error); emit errorOccurred(result.error); emit finished(false); return; }
        canvas->setImage(result.image, result.original); canvas->setEnabled(true);
        buttons->button(QDialogButtonBox::Save)->setEnabled(true);
        status->setText(tr("%1 × %2 pixels. Edits save a new image copy; codec metadata support varies.").arg(result.original.width()).arg(result.original.height()));
        emit statusChanged(tr("Image editor ready.")); emit finished(false);
    });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this, dialog, source, canvas] {
        ImageOptions options;
        // Lossless output for annotations/crop; high-depth pixels kept by PNG
        // when the installed writer supports them. Never write the source.
        options.format = writableImageFormats().contains("png") ? QByteArray("png") : writableImageFormats().first();
        const auto edits = canvas->edits(); m_dialog = nullptr; dialog->accept();
        chooseImageDestination({source}, options, edits, tr(" edited"));
    });
    dialog->resize(1000, 700); dialog->open();
}

#include "quickactions.moc"
