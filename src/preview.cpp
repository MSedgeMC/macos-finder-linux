// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#include "preview.h"
#include "tags.h"

#include <KFileItem>
#include <KIO/PreviewJob>
#include <KIO/ApplicationLauncherJob>
#include <KIO/JobUiDelegateFactory>
#include <KIO/OpenUrlJob>
#include <KApplicationTrader>
#include <KEMailClientLauncherJob>
#include <KService>
#include <KJobUiDelegate>
#include <QAction>
#include <QApplication>
#include <QAudioOutput>
#include <QClipboard>
#include <QColorSpace>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QImageReader>
#include <QFileSystemWatcher>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMediaPlayer>
#include <QMetaMethod>
#include <QMouseEvent>
#include <QMenu>
#include <QMimeDatabase>
#include <QPainter>
#include <QPdfDocument>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTextEdit>
#include <QTextDocument>
#include <QTimer>
#include <QThreadPool>
#include <QRunnable>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVideoWidget>
#include <QWheelEvent>
#if defined(FINDER_PREVIEW_PRINT_SUPPORT) && FINDER_PREVIEW_PRINT_SUPPORT
#include <QPrintDialog>
#include <QPrinter>
#include <QProgressDialog>
#endif
#include <QGuiApplication>
#include <QtEndian>
#include <algorithm>
#include <memory>
#include <atomic>
#include <functional>
#include <cmath>

struct PreviewRaster {
    QImage image;
    QSize originalSize;
    QSizeF resolution;
    QSizeF pagePoints;
    int pageCount = 0;
    bool rotatable = false;
    QString error;
};

struct PreviewPrintState {
#if defined(FINDER_PREVIEW_PRINT_SUPPORT) && FINDER_PREVIEW_PRINT_SUPPORT
    QUrl url;
    QString mime;
    int page = 0;
    int lastPage = -1;
    bool started = false;
    std::shared_ptr<QPrinter> printer;
    std::unique_ptr<QPainter> painter;
    QPointer<QProgressDialog> progress;
#endif
};

// Each pane/domain has at most one submitted job and one replaceable pending
// request. Across all panes, expensive preview/tag reads use only two workers.
// Decoders/PDFium cannot be interrupted mid-call; checkpoints discard their
// output and skip later phases. Destruction never waits on the GUI thread.
struct PreviewWorkQueue {
    bool running = false; // GUI thread only.
    std::shared_ptr<std::atomic_bool> cancelled;
    std::function<void()> pending;
    void cancel() {
        if (cancelled) cancelled->store(true);
        pending = {};
    }
};

class PreviewTask : public QObject {
    Q_OBJECT
signals:
    void finished();
};

// Paint only the visible source through a transform. Zoom/pan does not allocate
// huge scaled pixmaps or synchronously re-decode the full-resolution original.
class PreviewImageLabel : public QLabel {
public:
    using QLabel::QLabel;
    QImage raster;
    qreal zoom = 1;
    QPointF pan;
    std::function<void(qreal)> zoomRequested;
    void constrainPan() {
        const QSizeF size = fittedSize();
        const qreal x = std::max<qreal>(0, (size.width() - width()) / 2);
        const qreal y = std::max<qreal>(0, (size.height() - height()) / 2);
        pan.setX(std::clamp(pan.x(), -x, x));
        pan.setY(std::clamp(pan.y(), -y, y));
    }
protected:
    void paintEvent(QPaintEvent *) override {
        if (raster.isNull()) return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        const QSizeF size = fittedSize();
        painter.drawImage(QRectF(QPointF((width() - size.width()) / 2, (height() - size.height()) / 2) + pan, size), raster);
    }
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && zoom > 1) {
            dragging = true; last = event->position(); setCursor(Qt::ClosedHandCursor); event->accept();
        } else QLabel::mousePressEvent(event);
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        if (dragging) {
            pan += event->position() - last; last = event->position(); constrainPan(); update(); event->accept();
        } else QLabel::mouseMoveEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (dragging && event->button() == Qt::LeftButton) {
            dragging = false; unsetCursor(); event->accept();
        } else QLabel::mouseReleaseEvent(event);
    }
    void mouseDoubleClickEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && zoomRequested) { zoomRequested(zoom > 1 ? 1 : 2); event->accept(); }
        else QLabel::mouseDoubleClickEvent(event);
    }
    void wheelEvent(QWheelEvent *event) override {
        if ((event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)) && zoomRequested) {
            const qreal delta = event->pixelDelta().isNull() ? event->angleDelta().y() / 120.0 : event->pixelDelta().y() / 40.0;
            zoomRequested(zoom * std::pow(1.2, delta)); event->accept();
        } else QLabel::wheelEvent(event);
    }
private:
    QSizeF fittedSize() const { return QSizeF(raster.size()).scaled(QSizeF(size()), Qt::KeepAspectRatio) * zoom; }
    bool dragging = false;
    QPointF last;
};

namespace {
constexpr qsizetype textLimit = 512 * 1024;
constexpr qint64 maxDecodedPixels = 16 * 1024 * 1024;
const QSize renderLimit(1600, 1200);
constexpr qint64 nativePreviewLimit = 64 * 1024 * 1024;

QThreadPool &previewPool()
{
    static QThreadPool pool;
    static const bool configured = [&] {
        pool.setMaxThreadCount(2);
        pool.setExpiryTimeout(30000);
        // Join before QApplication teardown, never when a pane is closed.
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, qApp, [&] { pool.waitForDone(); });
        return true;
    }();
    Q_UNUSED(configured);
    return pool;
}

template<typename Work, typename Done>
void submitPreview(const std::shared_ptr<PreviewWorkQueue> &queue, QObject *context, Work work, Done done)
{
    if (queue->cancelled) queue->cancelled->store(true);
    queue->pending = [queue, context, work = std::move(work), done = std::move(done)]() mutable {
        queue->running = true;
        auto cancelled = queue->cancelled = std::make_shared<std::atomic_bool>(false);
        auto task = std::shared_ptr<PreviewTask>(new PreviewTask, [](PreviewTask *object) { object->deleteLater(); });
        using Result = decltype(work(cancelled));
        auto result = std::make_shared<Result>();
        QObject::connect(task.get(), &PreviewTask::finished, context, [queue, cancelled, task, result, done] {
            queue->running = false;
            if (!cancelled->load()) done(*result);
            // done may itself submit the next request (e.g. fallback).
            if (!queue->running && queue->pending) {
                auto next = std::move(queue->pending);
                queue->pending = {};
                next();
            }
        }, Qt::ConnectionType(Qt::QueuedConnection | Qt::SingleShotConnection));
        previewPool().start(QRunnable::create([task, cancelled, result, work = std::move(work)] {
            if (!cancelled->load()) *result = work(cancelled);
            emit task->finished();
        }));
    };
    if (!queue->running) {
        auto next = std::move(queue->pending);
        queue->pending = {};
        next();
    }
}

QSizeF tiffResolution(const QByteArray &bytes)
{
    if (bytes.size() < 8) return {};
    const bool little = bytes.startsWith("II");
    if (!little && !bytes.startsWith("MM")) return {};
    auto word = [&](qsizetype offset) -> quint16 {
        if (offset < 0 || offset > bytes.size() - 2) return 0;
        const auto *p = reinterpret_cast<const uchar *>(bytes.constData() + offset);
        return little ? qFromLittleEndian<quint16>(p) : qFromBigEndian<quint16>(p);
    };
    auto integer = [&](qsizetype offset) -> quint32 {
        if (offset < 0 || offset > bytes.size() - 4) return 0;
        const auto *p = reinterpret_cast<const uchar *>(bytes.constData() + offset);
        return little ? qFromLittleEndian<quint32>(p) : qFromBigEndian<quint32>(p);
    };
    if (word(2) != 42) return {};
    const qsizetype directory = integer(4);
    if (directory < 8 || directory > bytes.size() - 2) return {};
    const int entries = std::min<int>(word(directory), 128);
    qreal x = 0, y = 0;
    int unit = 2; // TIFF's specified default is inches.
    for (int i = 0; i < entries; ++i) {
        const qsizetype entry = directory + 2 + i * 12;
        if (entry > bytes.size() - 12) break;
        const quint16 tag = word(entry), type = word(entry + 2);
        if (integer(entry + 4) != 1) continue;
        if (tag == 296 && type == 3) unit = word(entry + 8);
        if ((tag == 282 || tag == 283) && type == 5) {
            const qsizetype value = integer(entry + 8);
            if (value > bytes.size() - 8) continue;
            const quint32 denominator = integer(value + 4);
            const qreal density = denominator ? qreal(integer(value)) / denominator : 0;
            if (tag == 282) x = density;
            else y = density;
        }
    }
    if ((unit != 2 && unit != 3) || x <= 0 || y <= 0) return {};
    return QSizeF(x, y) * (unit == 3 ? 2.54 : 1.0);
}

QSizeF explicitImageResolution(const QString &path)
{
    // QImage has a platform-default density even when the file omits it.
    // Read bounded, explicit PNG/JPEG/TIFF metadata instead of inventing DPI.
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QByteArray bytes = file.read(256 * 1024);
    auto big = [&](qsizetype offset) -> quint32 {
        if (offset < 0 || offset > bytes.size() - 4) return 0;
        return qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData() + offset));
    };
    if (bytes.startsWith(QByteArray::fromHex("89504e470d0a1a0a"))) {
        qsizetype offset = 8;
        for (int i = 0; i < 64 && offset <= bytes.size() - 12; ++i) {
            const quint32 length = big(offset);
            const QByteArray type = bytes.mid(offset + 4, 4);
            if (type == "IDAT" || length > quint64(bytes.size() - offset - 12)) break;
            if (type == "pHYs" && length == 9 && bytes[offset + 16] == '\1') {
                const quint32 x = big(offset + 8), y = big(offset + 12);
                if (x && y) return QSizeF(x * .0254, y * .0254);
            }
            offset += length + 12;
        }
        return {};
    }
    if (bytes.startsWith(QByteArray::fromHex("ffd8"))) {
        qsizetype offset = 2;
        QSizeF jfif;
        for (int i = 0; i < 128 && offset < bytes.size(); ++i) {
            if (static_cast<uchar>(bytes[offset++]) != 0xff) break;
            while (offset < bytes.size() && static_cast<uchar>(bytes[offset]) == 0xff) ++offset;
            if (offset >= bytes.size()) break;
            const uchar marker = static_cast<uchar>(bytes[offset++]);
            if (marker == 0xda || marker == 0xd9) break;
            if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
            if (offset > bytes.size() - 2) break;
            const auto *p = reinterpret_cast<const uchar *>(bytes.constData() + offset);
            const quint16 length = qFromBigEndian<quint16>(p);
            if (length < 2 || length > bytes.size() - offset) break;
            const QByteArray data = bytes.mid(offset + 2, length - 2);
            if (marker == 0xe1 && data.startsWith(QByteArray("Exif\0\0", 6))) {
                const QSizeF resolution = tiffResolution(data.mid(6));
                if (!resolution.isEmpty()) return resolution;
            }
            if (marker == 0xe0 && data.startsWith(QByteArray("JFIF\0", 5)) && data.size() >= 12) {
                const auto *density = reinterpret_cast<const uchar *>(data.constData());
                const int unit = density[7];
                const quint16 x = qFromBigEndian<quint16>(density + 8), y = qFromBigEndian<quint16>(density + 10);
                if ((unit == 1 || unit == 2) && x && y) jfif = QSizeF(x, y) * (unit == 2 ? 2.54 : 1.0);
            }
            offset += length;
        }
        return jfif;
    }
    return tiffResolution(bytes);
}

QString playbackTime(qint64 milliseconds)
{
    const qint64 seconds = std::max<qint64>(0, milliseconds / 1000);
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

PreviewRaster readRaster(const QString &path, bool pdf, int page, const QSize &limit,
                         const std::shared_ptr<std::atomic_bool> &cancelled)
{
    PreviewRaster result;
    if (cancelled->load()) return result;
    if (pdf) {
        // The document is created, used, closed and destroyed on this worker.
        // No QPdfDocument instance crosses thread affinity boundaries.
        QPdfDocument document;
        const auto error = document.load(path);
        if (cancelled->load()) return result;
        result.pageCount = document.pageCount();
        if (error != QPdfDocument::Error::None || result.pageCount < 1) {
            result.error = error == QPdfDocument::Error::IncorrectPassword
                ? PreviewPane::tr("This PDF is password protected.\nOpen it with its default application.")
                : PreviewPane::tr("The PDF could not be opened.\nOpen it with its default application.");
            return result;
        }
        page = std::clamp(page, 0, result.pageCount - 1);
        result.pagePoints = document.pagePointSize(page);
        QSize size = result.pagePoints.toSize();
        if (size.isValid()) {
            size.scale(limit, Qt::KeepAspectRatio);
            const QImage raster = document.render(page, size);
            if (cancelled->load()) return result;
            if (!raster.isNull()) {
                result.image = QImage(raster.size(), QImage::Format_ARGB32_Premultiplied);
                result.image.fill(Qt::white);
                QPainter painter(&result.image);
                painter.drawImage(0, 0, raster);
            }
        }
        if (result.image.isNull()) result.error = PreviewPane::tr("This PDF page could not be rendered.");
        return result;
    }
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize original = reader.size();
    result.originalSize = original;
    const bool rotated = reader.transformation().testFlag(QImageIOHandler::TransformationRotate90);
    if (rotated) result.originalSize.transpose();
    result.resolution = explicitImageResolution(path);
    if (rotated) result.resolution.transpose();
    const qint64 pixels = qint64(original.width()) * original.height();
    if (!original.isValid() || pixels <= 0 ||
        (pixels > maxDecodedPixels && !reader.supportsOption(QImageIOHandler::ScaledSize))) {
        result.error = PreviewPane::tr("This image is too large to preview safely.\nOpen it with its default application.");
        return result;
    }
    if (cancelled->load()) return result;
    if (original.width() > limit.width() || original.height() > limit.height())
        reader.setScaledSize(original.scaled(limit, Qt::KeepAspectRatio));
    QImage image = reader.read();
    if (cancelled->load()) return result;
    if (image.isNull()) {
        result.error = PreviewPane::tr("The image could not be decoded: %1\nOpen it with its default application.")
            .arg(reader.errorString());
        return result;
    }
    const int densityX = image.dotsPerMeterX(), densityY = image.dotsPerMeterY();
    if (image.width() > limit.width() || image.height() > limit.height())
        image = image.scaled(limit, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    image.setDotsPerMeterX(densityX);
    image.setDotsPerMeterY(densityY);
    result.image = image;
    result.rotatable = !reader.supportsAnimation();
    return result;
}

QPushButton *button(const QString &text, const QString &name, QWidget *parent)
{
    auto *result = new QPushButton(text, parent);
    result->setObjectName(name);
    result->setAccessibleName(text);
    result->setAutoDefault(false);
    return result;
}
}

PreviewPane::PreviewPane(QWidget *parent) : QWidget(parent),
    m_renderWork(std::make_shared<PreviewWorkQueue>()), m_tagWork(std::make_shared<PreviewWorkQueue>()),
    m_contactWork(std::make_shared<PreviewWorkQueue>()), m_printWork(std::make_shared<PreviewWorkQueue>())
{
    setObjectName(QStringLiteral("finderPreviewPane"));
    setAutoFillBackground(true);
    setFocusPolicy(Qt::StrongFocus);
    auto *layout = m_layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(10);
    m_headerWidget = new QWidget(this);
    auto *header = m_header = new QHBoxLayout(m_headerWidget);
    header->setContentsMargins(0, 0, 0, 0);
    m_close = button(QStringLiteral("×"), QStringLiteral("previewClose"), this);
    m_close->setAccessibleName(tr("Close Quick Look"));
    m_close->setFixedWidth(32);
    m_previous = button(QStringLiteral("‹"), QStringLiteral("previewPrevious"), this);
    m_previous->setAccessibleName(tr("Previous file"));
    m_next = button(QStringLiteral("›"), QStringLiteral("previewNext"), this);
    m_next->setAccessibleName(tr("Next file"));
    m_previous->setFixedWidth(30);
    m_next->setFixedWidth(30);
    m_name = new QLabel(this);
    m_name->setObjectName(QStringLiteral("previewName"));
    m_name->setAlignment(Qt::AlignCenter);
    m_name->setTextFormat(Qt::PlainText);
    m_name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_count = new QLabel(this);
    m_count->setObjectName(QStringLiteral("previewCount"));
    m_open = button(tr("Open"), QStringLiteral("previewOpen"), this);
    m_open->setAccessibleName(tr("Open with default application"));
    header->addWidget(m_close);
    header->addWidget(m_previous);
    header->addWidget(m_next);
    header->addWidget(m_name, 1);
    header->addWidget(m_count);
    header->addWidget(m_open);
    layout->addWidget(m_headerWidget);

    m_previewArea = new QWidget(this);
    auto *previewLayout = new QVBoxLayout(m_previewArea);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    previewLayout->setSpacing(10);

    m_content = new QStackedWidget(this);
    m_content->setObjectName(QStringLiteral("previewContent"));
    m_fallback = new QLabel(this);
    m_fallback->setObjectName(QStringLiteral("previewFallback"));
    m_fallback->setAlignment(Qt::AlignCenter);
    m_fallback->setWordWrap(true);
    m_fallback->setTextFormat(Qt::PlainText);
    m_content->addWidget(m_fallback);
    m_picture = new PreviewImageLabel(this);
    static_cast<PreviewImageLabel *>(m_picture)->zoomRequested = [this](qreal zoom) { setZoomFactor(zoom); };
    m_picture->setObjectName(QStringLiteral("previewImage"));
    m_picture->setAlignment(Qt::AlignCenter);
    m_picture->setMinimumSize(1, 1);
    m_picture->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_picture->installEventFilter(this);
    m_content->addWidget(m_picture);
    m_text = new QTextEdit(this);
    m_text->setObjectName(QStringLiteral("previewText"));
    m_text->setReadOnly(true);
    m_text->setLineWrapMode(QTextEdit::NoWrap);
    m_text->setAcceptRichText(false);
    m_text->setFrameShape(QFrame::NoFrame);
    QFont monospace(QStringLiteral("Noto Sans Mono"));
    monospace.setPointSize(10);
    monospace.setWeight(QFont::Normal);
    m_text->setFont(monospace);
    m_content->addWidget(m_text);
    m_videoPage = new QWidget(this);
    auto *videoLayout = new QVBoxLayout(m_videoPage);
    videoLayout->setContentsMargins(0, 0, 0, 0);
    m_content->addWidget(m_videoPage);
    m_contactList = new QListWidget(this);
    m_contactList->setObjectName(QStringLiteral("previewContactSheet"));
    m_contactList->setAccessibleName(tr("Selection contact sheet"));
    m_contactList->setViewMode(QListView::IconMode);
    m_contactList->setResizeMode(QListView::Adjust);
    m_contactList->setMovement(QListView::Static);
    m_contactList->setIconSize(QSize(128, 96));
    m_contactList->setGridSize(QSize(160, 136));
    m_contactList->setWordWrap(true);
    m_contactList->setSpacing(8);
    m_contactList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_content->addWidget(m_contactList);
    previewLayout->addWidget(m_content, 1);

    m_viewControls = new QWidget(this);
    m_viewControls->setObjectName(QStringLiteral("previewViewControls"));
    auto *viewControls = new QHBoxLayout(m_viewControls);
    viewControls->setContentsMargins(0, 0, 0, 0);
    m_zoomOut = button(QStringLiteral("−"), QStringLiteral("previewZoomOut"), this);
    m_zoomOut->setAccessibleName(tr("Zoom out"));
    m_zoomReset = button(tr("Fit"), QStringLiteral("previewZoomReset"), this);
    m_zoomIn = button(QStringLiteral("+"), QStringLiteral("previewZoomIn"), this);
    m_zoomIn->setAccessibleName(tr("Zoom in"));
    m_contactToggle = button(tr("Index Sheet"), QStringLiteral("previewIndexSheet"), this);
    m_contactToggle->setCheckable(true);
    m_slideshow = button(tr("Slideshow"), QStringLiteral("previewSlideshow"), this);
    m_slideshow->setCheckable(true);
    m_fullScreen = button(tr("Full Screen"), QStringLiteral("previewFullScreen"), this);
    m_fullScreen->setCheckable(true);
    viewControls->addWidget(m_zoomOut);
    viewControls->addWidget(m_zoomReset);
    viewControls->addWidget(m_zoomIn);
    viewControls->addStretch();
    viewControls->addWidget(m_contactToggle);
    viewControls->addWidget(m_slideshow);
    viewControls->addWidget(m_fullScreen);
    m_nativeActions = new QToolButton(this);
    m_nativeActions->setObjectName(QStringLiteral("previewNativeActions"));
    m_nativeActions->setText(tr("Actions"));
    m_nativeActions->setAccessibleName(tr("Open, Print and Share actions"));
    m_nativeActions->setPopupMode(QToolButton::InstantPopup);
    auto *nativeMenu = new QMenu(m_nativeActions);
    m_openWithAction = nativeMenu->addAction(tr("Open With…"));
    m_openWithAction->setObjectName(QStringLiteral("previewOpenWith"));
    connect(m_openWithAction, &QAction::triggered, this, &PreviewPane::openWithCurrent);
    m_printAction = nativeMenu->addAction(tr("Print…"));
    m_printAction->setObjectName(QStringLiteral("previewPrint"));
    connect(m_printAction, &QAction::triggered, this, &PreviewPane::printCurrent);
    auto *shareMenu = nativeMenu->addMenu(tr("Share"));
    m_copyUrlsAction = shareMenu->addAction(tr("Copy File Locations"));
    connect(m_copyUrlsAction, &QAction::triggered, this, [this] {
        QStringList locations;
        for (const auto &url : actionUrls()) locations.append(url.isLocalFile() ? url.toLocalFile()
            : url.toDisplayString(QUrl::RemovePassword));
        QGuiApplication::clipboard()->setText(locations.join(QLatin1Char('\n')));
    });
    m_emailAction = shareMenu->addAction(tr("Email as Attachments…"));
    m_emailAction->setObjectName(QStringLiteral("previewEmail"));
    connect(m_emailAction, &QAction::triggered, this, [this] {
        const auto urls = actionUrls();
        if (urls.isEmpty()) return;
        for (const auto &url : urls) if (!url.isLocalFile() || !QFileInfo(url.toLocalFile()).isFile()) return;
        auto *job = new KEMailClientLauncherJob;
        job->setAttachments(urls);
        job->setUiDelegate(KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, window()));
        connect(job, &KJob::result, this, [this, url = currentUrl()](KJob *result) {
            if (result->error()) emit actionError(url, result->errorText());
        });
        job->start();
    });
    m_shareAction = shareMenu->addAction(tr("Share…"));
    m_shareAction->setObjectName(QStringLiteral("previewShare"));
    connect(m_shareAction, &QAction::triggered, this, &PreviewPane::shareCurrent);
    m_nativeActions->setMenu(nativeMenu);
    viewControls->addWidget(m_nativeActions);
    previewLayout->addWidget(m_viewControls);
    m_slideshowTimer = new QTimer(this);
    m_contactTimer = new QTimer(this);
    m_contactTimer->setSingleShot(true);
    m_contactTimer->setInterval(80);
    m_sourceWatcher = new QFileSystemWatcher(this);
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(120);
    connect(m_sourceWatcher, &QFileSystemWatcher::fileChanged, this, [this] { m_refreshTimer->start(); });
    connect(m_sourceWatcher, &QFileSystemWatcher::directoryChanged, this, [this] {
        // Changes to adjacent files (including our PDF print output) do not
        // invalidate a source whose inode watch is still active. Removed or
        // atomically replaced sources lose that watch and require re-arming.
        const QString path = currentUrl().toLocalFile();
        if (QFileInfo(path).isFile() && m_sourceWatcher->files().contains(path)) return;
        m_refreshTimer->start();
    });
    connect(m_refreshTimer, &QTimer::timeout, this, &PreviewPane::refreshCurrent);
    connect(m_contactTimer, &QTimer::timeout, this, &PreviewPane::loadContactThumbnails);
    connect(m_contactList->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { m_contactTimer->start(); });
    connect(m_contactList, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        m_index = m_contactList->row(item);
        setContactSheet(false);
        showCurrent();
        emit currentUrlChanged(currentUrl());
    });
    connect(m_zoomOut, &QPushButton::clicked, this, [this] { setZoomFactor(m_zoom / 1.2); });
    connect(m_zoomIn, &QPushButton::clicked, this, [this] { setZoomFactor(m_zoom * 1.2); });
    connect(m_zoomReset, &QPushButton::clicked, this, &PreviewPane::resetZoom);
    connect(m_contactToggle, &QPushButton::clicked, this, &PreviewPane::setContactSheet);
    connect(m_fullScreen, &QPushButton::clicked, this, &PreviewPane::setFullScreen);
    connect(m_slideshow, &QPushButton::clicked, this, [this](bool enabled) { setSlideshow(enabled); });
    connect(m_slideshowTimer, &QTimer::timeout, this, [this] {
        if (m_urls.size() < 2 || !isVisible()) { setSlideshow(false); return; }
        if (m_index + 1 == m_urls.size()) { m_index = 0; showCurrent(); emit currentUrlChanged(currentUrl()); }
        else advance(1);
    });

    m_pageControls = new QWidget(this);
    auto *pageLayout = new QHBoxLayout(m_pageControls);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    m_pagePrevious = button(QStringLiteral("‹"), QStringLiteral("previewPreviousPage"), this);
    m_pagePrevious->setAccessibleName(tr("Previous PDF page"));
    m_pageNext = button(QStringLiteral("›"), QStringLiteral("previewNextPage"), this);
    m_pageNext->setAccessibleName(tr("Next PDF page"));
    m_pageLabel = new QLabel(this);
    m_pageLabel->setObjectName(QStringLiteral("previewPageCount"));
    pageLayout->addStretch();
    pageLayout->addWidget(m_pagePrevious);
    pageLayout->addWidget(m_pageLabel);
    pageLayout->addWidget(m_pageNext);
    pageLayout->addStretch();
    previewLayout->addWidget(m_pageControls);

    m_mediaControls = new QWidget(this);
    auto *mediaLayout = new QHBoxLayout(m_mediaControls);
    mediaLayout->setContentsMargins(0, 0, 0, 0);
    m_play = button(tr("Play"), QStringLiteral("previewPlay"), this);
    m_seek = new QSlider(Qt::Horizontal, this);
    m_seek->setObjectName(QStringLiteral("previewSeek"));
    m_seek->setAccessibleName(tr("Playback position"));
    m_seek->setRange(0, 10000);
    m_seek->setEnabled(false);
    m_playbackTime = new QLabel(QStringLiteral("0:00 / 0:00"), this);
    m_playbackTime->setObjectName(QStringLiteral("previewPlaybackTime"));
    mediaLayout->addWidget(m_play);
    mediaLayout->addWidget(m_seek, 1);
    mediaLayout->addWidget(m_playbackTime);
    previewLayout->addWidget(m_mediaControls);
    layout->addWidget(m_previewArea, 1);
    m_metadata = new QLabel(this);
    m_metadata->setObjectName(QStringLiteral("previewMetadata"));
    m_metadata->setAlignment(Qt::AlignCenter);
    m_metadata->setWordWrap(true);
    m_metadata->setTextFormat(Qt::PlainText);
    m_metadata->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_metadata);

    connect(m_close, &QPushButton::clicked, this, &PreviewPane::requestClose);
    connect(m_previous, &QPushButton::clicked, this, [this] { advance(-1); });
    connect(m_next, &QPushButton::clicked, this, [this] { advance(1); });
    connect(m_open, &QPushButton::clicked, this, &PreviewPane::openCurrent);
    connect(m_pagePrevious, &QPushButton::clicked, this, [this] { renderPage(m_page - 1); });
    connect(m_pageNext, &QPushButton::clicked, this, [this] { renderPage(m_page + 1); });
    connect(m_play, &QPushButton::clicked, this, &PreviewPane::toggleMedia);
    connect(m_seek, &QSlider::sliderReleased, this, [this] {
        if (m_player && m_player->isSeekable())
            m_player->setPosition(m_player->duration() * m_seek->value() / 10000);
    });
    showCurrent();
}

PreviewPane::~PreviewPane()
{
    m_renderWork->cancel();
    m_tagWork->cancel();
    m_contactWork->cancel();
    m_printWork->cancel();
#if defined(FINDER_PREVIEW_PRINT_SUPPORT) && FINDER_PREVIEW_PRINT_SUPPORT
    if (m_printState && m_printState->painter) m_printState->painter->end();
    if (m_printState && m_printState->printer) m_printState->printer->abort();
#endif
    cancelNativePreview();
    stopMedia();
}

QSize PreviewPane::originalImageSize() const { return m_originalImageSize; }

void PreviewPane::refreshCurrent()
{
    m_images.clear();
    m_fingerprint.clear();
    showCurrent();
    if (m_contactSheet) { ++m_contactGeneration; m_contactWork->cancel(); populateContactSheet(); }
}

QList<QUrl> PreviewPane::urls() const { return m_urls; }

bool PreviewPane::toggleQuickLook(const QList<QUrl> &urls, const QUrl &primary)
{
    if (isVisible()) { requestClose(); return false; }
    updateQuickLook(urls, primary);
    if (m_urls.isEmpty()) return false;
    show();
    setFocus(Qt::ShortcutFocusReason);
    return true;
}

void PreviewPane::updateQuickLook(const QList<QUrl> &urls, const QUrl &primary)
{
    const QUrl previous = currentUrl();
    QSignalBlocker blocker(this);
    setUrls(urls);
    const int index = int(m_urls.indexOf(primary));
    if (index >= 0 && index != m_index) {
        m_index = index;
        showCurrent();
    }
    blocker.unblock();
    if (previous != currentUrl()) emit currentUrlChanged(currentUrl());
    if (m_urls.isEmpty() && isVisible()) requestClose();
}

void PreviewPane::setZoomFactor(qreal factor)
{
    if (!std::isfinite(factor)) return;
    m_zoom = std::clamp(factor, .25, 16.0);
    updateImageSize();
    updatePreviewControls();
}

qreal PreviewPane::zoomFactor() const { return m_zoom; }
void PreviewPane::resetZoom()
{
    static_cast<PreviewImageLabel *>(m_picture)->pan = {};
    setZoomFactor(1);
}

void PreviewPane::setFullScreen(bool enabled)
{
    // Embedded Gallery/column previews must not fullscreen their Finder host.
    if (m_compact || m_gallery || enabled == isFullScreen()) return;
    QWidget *host = window();
    if (enabled) {
        m_beforeFullScreen = host->windowState();
        host->showFullScreen();
    } else {
        host->setWindowState(m_beforeFullScreen & ~Qt::WindowFullScreen);
        host->show();
    }
    updatePreviewControls();
}

bool PreviewPane::isFullScreen() const { return window()->windowState().testFlag(Qt::WindowFullScreen); }
bool PreviewPane::contactSheet() const { return m_contactSheet; }
bool PreviewPane::slideshowRunning() const { return m_slideshowTimer->isActive(); }

QList<QUrl> PreviewPane::actionUrls() const
{
    return m_contactSheet ? m_urls : currentUrl().isEmpty() ? QList<QUrl>() : QList<QUrl>{currentUrl()};
}

void PreviewPane::openCurrent()
{
    const QUrl url = currentUrl();
    if (url.isEmpty()) return;
    if (isSignalConnected(QMetaMethod::fromSignal(&PreviewPane::openRequested))) {
        emit openRequested(url);
        return;
    }
    auto *job = new KIO::OpenUrlJob(url, m_mime);
    job->setUiDelegate(KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, window()));
    connect(job, &KJob::result, this, [this, url](KJob *result) {
        if (result->error()) emit actionError(url, result->errorText());
    });
    job->start();
}

void PreviewPane::openWithCurrent()
{
    const QList<QUrl> urls = actionUrls();
    if (urls.isEmpty()) return;
    auto *job = new KIO::ApplicationLauncherJob;
    job->setUrls(urls);
    job->setUiDelegate(KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, window()));
    connect(job, &KJob::result, this, [this, url = currentUrl()](KJob *result) {
        if (result->error()) emit actionError(url, result->errorText());
    });
    job->start();
}

void PreviewPane::setShareProviderAvailable(bool available)
{
    m_shareProviderAvailable = available;
    updateNativeActions();
}

void PreviewPane::shareCurrent()
{
    if (m_shareProviderAvailable && !actionUrls().isEmpty()) emit shareRequested(actionUrls());
}

bool PreviewPane::canPrintCurrent() const
{
#if defined(FINDER_PREVIEW_PRINT_SUPPORT) && FINDER_PREVIEW_PRINT_SUPPORT
    if (m_printState || m_contactSheet || !currentUrl().isLocalFile()) return false;
    const QFileInfo info(currentUrl().toLocalFile());
    return info.isFile() && info.isReadable() && (m_mime.startsWith(QStringLiteral("image/"))
        || m_mime == QStringLiteral("application/pdf") || m_content->currentWidget() == m_text);
#else
    return false;
#endif
}

void PreviewPane::updateNativeActions()
{
    const auto urls = actionUrls();
    m_nativeActions->setEnabled(!urls.isEmpty());
    m_openWithAction->setEnabled(!urls.isEmpty());
    m_copyUrlsAction->setEnabled(!urls.isEmpty());
    m_shareAction->setVisible(m_shareProviderAvailable);
    m_shareAction->setEnabled(m_shareProviderAvailable && !urls.isEmpty());
    const bool localFiles = !urls.isEmpty() && std::all_of(urls.begin(), urls.end(), [](const QUrl &url) {
        return url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile();
    });
    const auto emailHandler = KApplicationTrader::preferredService(QStringLiteral("x-scheme-handler/mailto"));
    m_emailAction->setEnabled(localFiles && bool(emailHandler));
    m_emailAction->setToolTip(emailHandler ? QString() : tr("No native email application is configured."));
    m_printAction->setEnabled(canPrintCurrent());
#if !defined(FINDER_PREVIEW_PRINT_SUPPORT) || !FINDER_PREVIEW_PRINT_SUPPORT
    m_printAction->setToolTip(tr("This build does not include Qt PrintSupport."));
#endif
    const QString mime = currentUrl().isLocalFile()
        ? QMimeDatabase().mimeTypeForFile(currentUrl().toLocalFile(), QMimeDatabase::MatchExtension).name() : m_mime;
    const auto handler = mime.isEmpty() ? KService::Ptr() : KApplicationTrader::preferredService(mime);
    m_open->setText(handler ? tr("Open with %1").arg(handler->name()) : tr("Open"));
}

void PreviewPane::printCurrent()
{
#if defined(FINDER_PREVIEW_PRINT_SUPPORT) && FINDER_PREVIEW_PRINT_SUPPORT
    if (!canPrintCurrent()) return;
    setSlideshow(false);
    const QUrl url = currentUrl();
    auto printer = std::make_shared<QPrinter>(QPrinter::HighResolution);
    printer->setDocName(url.fileName());
    QPrintDialog dialog(printer.get(), window());
    dialog.setWindowTitle(tr("Print %1").arg(url.fileName()));
    if (m_pageCount > 0) {
        dialog.setMinMax(1, m_pageCount);
        dialog.setOption(QAbstractPrintDialog::PrintPageRange);
    }
    if (dialog.exec() == QDialog::Accepted && currentUrl() == url) printToPrinter(printer);
#endif
}

void PreviewPane::printToPrinter(const std::shared_ptr<QPrinter> &printer)
{
#if defined(FINDER_PREVIEW_PRINT_SUPPORT) && FINDER_PREVIEW_PRINT_SUPPORT
    if (!printer || !canPrintCurrent()) return;
    setSlideshow(false);
    const QUrl url = currentUrl();
    if (m_content->currentWidget() == m_text) {
        // The bounded, user-visible plain text is printable with Qt's native
        // pagination. Never send only a screenshot of the text viewport.
        m_text->document()->print(printer.get());
        emit printFinished(url, printer->printerState() == QPrinter::Error ? tr("Printing failed.") : QString());
        return;
    }
    m_printState = std::make_shared<PreviewPrintState>();
    m_printState->url = url;
    m_printState->mime = m_mime;
    m_printState->printer = printer;
    printer->setFullPage(true); // paintRectPixels below uses sheet coordinates.
    if (printer->printRange() == QPrinter::PageRange) {
        // The source may be refreshing while a host supplies a print range.
        // Clamp against the worker's actual document page count after load.
        m_printState->page = std::max(0, printer->fromPage() - 1);
        m_printState->lastPage = std::max(m_printState->page, printer->toPage() - 1);
    }
    auto *progress = new QProgressDialog(tr("Preparing pages…"), tr("Cancel"), 0, 0, window());
    progress->setAttribute(Qt::WA_DeleteOnClose);
    progress->setWindowTitle(tr("Print %1").arg(url.fileName()));
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    m_printState->progress = progress;
    connect(progress, &QProgressDialog::canceled, this, [this] { finishPrint(tr("Printing was cancelled.")); });
    progress->show();
    updateNativeActions();
    requestPrintPage();
#else
    Q_UNUSED(printer);
    emit printFinished(currentUrl(), tr("This build does not include Qt PrintSupport."));
#endif
}

void PreviewPane::requestPrintPage()
{
#if defined(FINDER_PREVIEW_PRINT_SUPPORT) && FINDER_PREVIEW_PRINT_SUPPORT
    if (!m_printState) return;
    const auto state = m_printState;
    submitPreview(m_printWork, this, [url = state->url, mime = state->mime, page = state->page](const auto &cancelled) {
        if (mime == QStringLiteral("application/pdf")) {
            // 4096² pixel budget is independent of the
            // 1600×1200 display cache. One page at a time bounds print memory.
            return readRaster(url.toLocalFile(), true, page, QSize(4096, 4096), cancelled);
        }
        PreviewRaster result;
        QImageReader reader(url.toLocalFile());
        reader.setAutoTransform(true);
        const QSize size = reader.size();
        if (!size.isValid() || qint64(size.width()) * size.height() > 64000000) {
            result.error = tr("This image is too large to print safely. Open it with its default application.");
        } else if (!cancelled->load()) {
            // Intentionally no setScaledSize: print the oriented original,
            // preserving its color space, resolution and alpha information.
            result.image = reader.read();
            if (result.image.isNull()) result.error = reader.errorString();
        }
        return result;
    }, [this, state](const PreviewRaster &raster) {
        if (state != m_printState) return;
        if (raster.image.isNull()) { finishPrint(raster.error.isEmpty() ? tr("This page could not be printed.") : raster.error); return; }
        if (!state->started) {
            state->painter = std::make_unique<QPainter>();
            if (!state->painter->begin(state->printer.get())) { finishPrint(tr("Could not start the printer.")); return; }
            state->started = true;
            if (state->lastPage < 0) state->lastPage = std::max(0, raster.pageCount - 1);
            if (raster.pageCount > 0) {
                state->page = std::clamp(state->page, 0, raster.pageCount - 1);
                state->lastPage = std::clamp(state->lastPage, state->page, raster.pageCount - 1);
            }
        } else if (!state->printer->newPage()) { finishPrint(tr("Could not create the next printed page.")); return; }
        QRectF area = state->printer->pageLayout().paintRectPixels(state->printer->resolution());
        const QSizeF fitted = QSizeF(raster.image.size()).scaled(area.size(), Qt::KeepAspectRatio);
        const QRectF target(area.center() - QPointF(fitted.width() / 2, fitted.height() / 2), fitted);
        state->painter->setRenderHint(QPainter::SmoothPixmapTransform);
        state->painter->fillRect(area, Qt::white);
        state->painter->drawImage(target, raster.image);
        if (state->progress) state->progress->setLabelText(tr("Printing page %1 of %2…").arg(state->page + 1).arg(state->lastPage + 1));
        if (state->page < state->lastPage) {
            ++state->page;
            QTimer::singleShot(0, this, &PreviewPane::requestPrintPage);
        } else finishPrint({});
    });
#endif
}

void PreviewPane::finishPrint(const QString &error)
{
#if defined(FINDER_PREVIEW_PRINT_SUPPORT) && FINDER_PREVIEW_PRINT_SUPPORT
    if (!m_printState) return;
    const auto state = std::move(m_printState);
    m_printWork->cancel();
    QString finalError = error;
    if (state->painter && state->painter->isActive() && !state->painter->end() && finalError.isEmpty())
        finalError = tr("The printer could not finish the job.");
    if (!finalError.isEmpty()) state->printer->abort();
    if (state->progress) { state->progress->disconnect(this); state->progress->close(); }
    updateNativeActions();
    if (!finalError.isEmpty()) emit actionError(state->url, finalError);
    emit printFinished(state->url, finalError);
#else
    Q_UNUSED(error);
#endif
}

void PreviewPane::setContactSheet(bool enabled)
{
    enabled = enabled && m_urls.size() > 1 && !m_compact && !m_gallery;
    if (enabled == m_contactSheet) return;
    m_contactSheet = enabled;
    ++m_contactGeneration;
    m_contactWork->cancel();
    if (enabled) {
        setSlideshow(false);
        stopMedia();
        m_renderWork->cancel();
        ++m_previewGeneration;
        cancelNativePreview();
        populateContactSheet();
        m_content->setCurrentWidget(m_contactList);
        m_contactList->setFocus(Qt::ShortcutFocusReason);
    } else {
        m_contactTimer->stop();
        m_fingerprint.clear();
        showCurrent();
        setFocus(Qt::ShortcutFocusReason);
    }
    updatePreviewControls();
}

void PreviewPane::setSlideshow(bool running, int intervalMs)
{
    running = running && m_urls.size() > 1 && !m_compact && !m_gallery;
    if (running) {
        setContactSheet(false);
        m_slideshowTimer->start(std::clamp(intervalMs, 500, 60000));
    } else m_slideshowTimer->stop();
    updatePreviewControls();
}

void PreviewPane::requestClose()
{
    setSlideshow(false);
    if (!m_compact && !m_gallery && isFullScreen()) setFullScreen(false);
    hide();
    emit closeRequested();
}

void PreviewPane::updatePreviewControls()
{
    m_viewControls->setVisible(!m_compact && !m_gallery);
    const bool raster = !m_image.isNull() && !m_contactSheet && m_content->currentWidget() == m_picture;
    m_zoomIn->setEnabled(raster && m_zoom < 16);
    m_zoomOut->setEnabled(raster && m_zoom > .25);
    m_zoomReset->setEnabled(raster);
    m_zoomReset->setText(qFuzzyCompare(m_zoom, 1) ? tr("Fit") : tr("%1% of Fit").arg(qRound(m_zoom * 100)));
    m_contactToggle->setEnabled(m_urls.size() > 1);
    m_contactToggle->setChecked(m_contactSheet);
    m_slideshow->setEnabled(m_urls.size() > 1);
    m_slideshow->setChecked(slideshowRunning());
    m_slideshow->setText(slideshowRunning() ? tr("Pause Slideshow") : tr("Slideshow"));
    m_fullScreen->setChecked(isFullScreen());
    m_fullScreen->setText(isFullScreen() ? tr("Exit Full Screen") : tr("Full Screen"));
    if (m_contactSheet) { m_pageControls->hide(); m_mediaControls->hide(); }
    updateNativeActions();
}

void PreviewPane::updateSourceWatch()
{
    const auto files = m_sourceWatcher->files();
    const auto directories = m_sourceWatcher->directories();
    if (!files.isEmpty()) m_sourceWatcher->removePaths(files);
    if (!directories.isEmpty()) m_sourceWatcher->removePaths(directories);
    if (!isVisible() || !currentUrl().isLocalFile()) return;
    const QFileInfo info(currentUrl().toLocalFile());
    // Watch parent as well: atomic replace removes an inode watch.
    if (info.isFile()) m_sourceWatcher->addPath(info.absoluteFilePath());
    if (info.dir().exists()) m_sourceWatcher->addPath(info.absolutePath());
}

void PreviewPane::populateContactSheet()
{
    m_contactList->clear();
    for (const QUrl &url : m_urls) {
        const QString icon = url.isLocalFile() && QFileInfo(url.toLocalFile()).isDir()
            ? QStringLiteral("folder") : QMimeDatabase().mimeTypeForUrl(url).iconName();
        auto *item = new QListWidgetItem(QIcon::fromTheme(icon, QIcon::fromTheme(QStringLiteral("text-x-generic"))),
            url.fileName().isEmpty() ? url.toDisplayString(QUrl::RemovePassword) : url.fileName(), m_contactList);
        item->setToolTip(url.toDisplayString(QUrl::RemovePassword));
        item->setData(Qt::UserRole, url);
    }
    m_contactList->setCurrentRow(m_index);
    m_contactTimer->start();
}

void PreviewPane::loadContactThumbnails()
{
    if (!m_contactSheet || !isVisible()) return;
    const quint64 generation = ++m_contactGeneration;
    struct Request { int row; QString path; QString key; bool pdf; };
    struct Thumbnail { int row; QString key; PreviewRaster raster; };
    QList<Request> requests;
    const QRect visible = m_contactList->viewport()->rect().adjusted(0, -136, 0, 136);
    // Load only visible items plus one row, in a batch of at most 24; no eager
    // full-selection decode. The 32 MiB cache includes all thumbnail rasters.
    for (int row = 0; row < m_contactList->count() && requests.size() < 24; ++row) {
        auto *item = m_contactList->item(row);
        if (!m_contactList->visualItemRect(item).intersects(visible)) continue;
        const QUrl url = item->data(Qt::UserRole).toUrl();
        if (!url.isLocalFile()) continue;
        const QFileInfo info(url.toLocalFile());
        if (!info.isFile()) continue;
        const QString key = url.toString() + QLatin1Char('|') + QString::number(info.size()) + QLatin1Char('|')
            + QString::number(info.lastModified().toMSecsSinceEpoch()) + QStringLiteral("|contact");
        if (item->data(Qt::UserRole + 1).toString() == key) continue;
        if (const PreviewRaster *cached = m_images.object(key)) {
            item->setIcon(QIcon(QPixmap::fromImage(cached->image)));
            item->setData(Qt::UserRole + 1, key);
            continue;
        }
        const QMimeType mime = QMimeDatabase().mimeTypeForFile(info, QMimeDatabase::MatchExtension);
        const bool pdf = mime.inherits(QStringLiteral("application/pdf"));
        if (mime.name().startsWith(QStringLiteral("image/")) || pdf)
            requests.append({row, info.absoluteFilePath(), key, pdf});
    }
    if (requests.isEmpty()) return;
    submitPreview(m_contactWork, this, [requests](const auto &cancelled) {
        QList<Thumbnail> thumbnails;
        for (const auto &request : requests) {
            if (cancelled->load()) break;
            thumbnails.append({request.row, request.key, readRaster(request.path, request.pdf, 0, QSize(256, 192), cancelled)});
        }
        return thumbnails;
    }, [this, generation](const QList<Thumbnail> &thumbnails) {
        if (!m_contactSheet || generation != m_contactGeneration) return;
        for (const auto &thumbnail : thumbnails) {
            auto *item = m_contactList->item(thumbnail.row);
            if (!item) continue;
            item->setData(Qt::UserRole + 1, thumbnail.key);
            if (thumbnail.raster.image.isNull()) {
                item->setToolTip(item->toolTip() + QLatin1Char('\n') + thumbnail.raster.error);
            } else {
                m_images.insert(thumbnail.key, new PreviewRaster(thumbnail.raster),
                    std::max<qsizetype>(1, (thumbnail.raster.image.sizeInBytes() + 1023) / 1024));
                item->setIcon(QIcon(QPixmap::fromImage(thumbnail.raster.image)));
            }
        }
        m_contactTimer->start();
    });
}

void PreviewPane::setUrls(const QList<QUrl> &urls)
{
    const QUrl previous = currentUrl();
    QList<QUrl> valid;
    for (const QUrl &url : urls)
        if (url.isValid() && !url.isEmpty() && !valid.contains(url)) valid.append(url);
    const bool changed = valid != m_urls;
    m_urls = valid;
    m_index = m_urls.isEmpty() ? -1 : std::max(0, int(m_urls.indexOf(previous)));
    showCurrent();
    if (changed && m_contactSheet) {
        if (m_urls.size() < 2) setContactSheet(false);
        else { ++m_contactGeneration; m_contactWork->cancel(); populateContactSheet(); }
    }
    if (m_urls.size() < 2) setSlideshow(false);
    updatePreviewControls();
    if (previous != currentUrl()) emit currentUrlChanged(currentUrl());
}

QUrl PreviewPane::currentUrl() const
{
    return m_index >= 0 && m_index < m_urls.size() ? m_urls.at(m_index) : QUrl();
}

void PreviewPane::advance(int delta)
{
    if (m_urls.isEmpty()) return;
    const int next = std::clamp(m_index + delta, 0, int(m_urls.size()) - 1);
    if (next == m_index) return;
    m_index = next;
    showCurrent();
    if (m_contactSheet) m_contactList->setCurrentRow(m_index);
    emit currentUrlChanged(currentUrl());
}

void PreviewPane::setCompact(bool compact)
{
    if (compact) { setSlideshow(false); setContactSheet(false); }
    m_compact = compact;
    m_close->setVisible(!compact);
    if (!m_gallery) layout()->setContentsMargins(compact ? 12 : 16, 12, compact ? 12 : 16, 12);
    updatePreviewControls();
}

void PreviewPane::setGallery(bool gallery)
{
    if (m_gallery == gallery) return;
    if (gallery) { setSlideshow(false); setContactSheet(false); }
    m_gallery = gallery;
    m_layout->removeWidget(m_previewArea);
    m_layout->removeWidget(m_metadata);
    if (gallery) {
        createGallery();
        auto *row = qobject_cast<QHBoxLayout *>(m_galleryArea->layout());
        row->insertWidget(0, m_previewArea, 1);
        auto *summary = m_gallerySidebar->findChild<QWidget *>(QStringLiteral("previewFileSummary"));
        qobject_cast<QVBoxLayout *>(summary->layout())->insertWidget(0, m_name);
        auto *information = m_gallerySidebar->findChild<QWidget *>(QStringLiteral("previewInformation"));
        qobject_cast<QVBoxLayout *>(information->layout())->addWidget(m_metadata);
        auto *actions = m_gallerySidebar->findChild<QWidget *>(QStringLiteral("previewQuickActions"));
        qobject_cast<QHBoxLayout *>(actions->layout())->insertWidget(0, m_open, 1);
        m_layout->setContentsMargins(0, 0, 0, 0);
        m_layout->setSpacing(0);
        m_previewArea->layout()->setContentsMargins(22, 18, 22, 12);
        m_layout->addWidget(m_galleryArea, 1);
        m_headerWidget->hide();
        m_galleryArea->show();
        m_name->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        m_name->setWordWrap(true);
        m_name->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        m_metadata->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        m_metadata->setTextFormat(Qt::RichText);
        updateGalleryWidth();
        refreshTags();
    } else {
        m_layout->removeWidget(m_galleryArea);
        m_galleryArea->hide();
        m_layout->addWidget(m_previewArea, 1);
        m_layout->addWidget(m_metadata);
        m_header->insertWidget(3, m_name, 1);
        m_header->insertWidget(4, m_count);
        m_header->addWidget(m_open);
        m_headerWidget->show();
        m_layout->setContentsMargins(16, 12, 16, 12);
        m_layout->setSpacing(10);
        m_previewArea->layout()->setContentsMargins(0, 0, 0, 0);
        m_name->setAlignment(Qt::AlignCenter);
        m_name->setWordWrap(false);
        m_name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        m_metadata->setAlignment(Qt::AlignCenter);
        m_metadata->setTextFormat(Qt::PlainText);
    }
    setMetadata(m_metadataText);
    updateImageSize();
    updatePreviewControls();
}

void PreviewPane::updateGalleryWidth()
{
    if (m_gallerySidebar) m_gallerySidebar->setFixedWidth(std::clamp(width() / 3, 224, 300));
}

void PreviewPane::setMetadata(const QString &text)
{
    m_metadataText = text;
    if (m_gallery) updateGalleryMetadata();
    else m_metadata->setText(text);
}

void PreviewPane::createGallery()
{
    if (m_galleryArea) return;
    m_galleryArea = new QWidget(this);
    m_galleryArea->setObjectName(QStringLiteral("previewGalleryArea"));
    auto *row = new QHBoxLayout(m_galleryArea);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    m_gallerySidebar = new QWidget(m_galleryArea);
    m_gallerySidebar->setObjectName(QStringLiteral("previewGalleryDetails"));
    m_gallerySidebar->setAutoFillBackground(true);
    m_gallerySidebar->setBackgroundRole(QPalette::Window);
    m_gallerySidebar->setStyleSheet(QStringLiteral(
        "#previewGalleryDetails{background:palette(window);border-left:1px solid palette(mid);}"
        "#previewGalleryDetails QLabel{color:palette(window-text);background:transparent;}"
        "#previewKindSize,#previewTagStatus{color:palette(placeholder-text);font-size:11px;}"
        "#previewGalleryDetails #previewName{font-weight:600;}"
        "#previewGalleryDetails #previewMetadata{font-size:11px;}"
        "#previewInformationHeading,#previewTagsHeading{font-weight:600;}"
        "#previewShowMore{background:transparent;border:0;color:palette(link);padding:0;font-size:11px;}"
        "#previewTagEditor{background:palette(base);color:palette(text);border:1px solid palette(mid);border-radius:4px;padding:5px;}"
        "#previewTagColors QToolButton{border:0;border-radius:11px;background:transparent;padding:1px;}"
        "#previewTagColors QToolButton:checked{background:palette(mid);}"
        "#previewTagChips QToolButton{background:palette(button);color:palette(button-text);border:0;border-radius:7px;padding:3px 7px;text-align:left;}"
        "#previewQuickActions{background:palette(base);border-top:1px solid palette(mid);}"
        "#previewQuickActions QPushButton,#previewQuickActions QToolButton{background:transparent;color:palette(button-text);border:0;border-radius:5px;padding:5px;font-size:11px;}"
        "#previewQuickActions QPushButton:hover,#previewQuickActions QToolButton:hover{background:palette(button);}"
        "#previewQuickActions QPushButton:disabled,#previewQuickActions QToolButton:disabled{color:palette(placeholder-text);}"
    ));
    auto *details = new QVBoxLayout(m_gallerySidebar);
    details->setContentsMargins(0, 0, 0, 0);
    details->setSpacing(0);
    auto *scroll = new QScrollArea(m_gallerySidebar);
    scroll->setObjectName(QStringLiteral("previewDetailsScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral("QScrollArea,QScrollArea>QWidget>QWidget{background:transparent;}"));
    auto *body = new QWidget(scroll);
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(12, 18, 12, 12);
    bodyLayout->setSpacing(18);
    auto *identity = new QWidget(body);
    auto *identityLayout = new QHBoxLayout(identity);
    identityLayout->setContentsMargins(0, 0, 0, 0);
    identityLayout->setSpacing(10);
    m_thumbnail = new QLabel(identity);
    m_thumbnail->setObjectName(QStringLiteral("previewThumbnail"));
    m_thumbnail->setFixedSize(44, 52);
    m_thumbnail->setAlignment(Qt::AlignCenter);
    auto *summary = new QWidget(identity);
    summary->setObjectName(QStringLiteral("previewFileSummary"));
    auto *summaryLayout = new QVBoxLayout(summary);
    summaryLayout->setContentsMargins(0, 0, 0, 0);
    summaryLayout->setSpacing(3);
    m_kindSize = new QLabel(summary);
    m_kindSize->setObjectName(QStringLiteral("previewKindSize"));
    m_kindSize->setTextFormat(Qt::PlainText);
    m_kindSize->setWordWrap(true);
    summaryLayout->addWidget(m_kindSize);
    identityLayout->addWidget(m_thumbnail);
    identityLayout->addWidget(summary, 1);
    bodyLayout->addWidget(identity);

    auto *information = new QWidget(body);
    information->setObjectName(QStringLiteral("previewInformation"));
    auto *informationLayout = new QVBoxLayout(information);
    informationLayout->setContentsMargins(0, 0, 0, 0);
    informationLayout->setSpacing(6);
    auto *informationHeader = new QHBoxLayout;
    auto *heading = new QLabel(tr("Information"), information);
    heading->setObjectName(QStringLiteral("previewInformationHeading"));
    informationHeader->addWidget(heading);
    informationHeader->addStretch();
    m_showMore = button(tr("Show More"), QStringLiteral("previewShowMore"), information);
    informationHeader->addWidget(m_showMore);
    informationLayout->addLayout(informationHeader);
    bodyLayout->addWidget(information);
    connect(m_showMore, &QPushButton::clicked, this, [this] {
        m_moreDetails = !m_moreDetails;
        updateGalleryMetadata();
    });

    auto *tags = new QWidget(body);
    auto *tagsLayout = new QVBoxLayout(tags);
    tagsLayout->setContentsMargins(0, 0, 0, 0);
    tagsLayout->setSpacing(7);
    auto *tagsHeading = new QLabel(tr("Tags"), tags);
    tagsHeading->setObjectName(QStringLiteral("previewTagsHeading"));
    tagsLayout->addWidget(tagsHeading);
    m_tagChips = new QWidget(tags);
    m_tagChips->setObjectName(QStringLiteral("previewTagChips"));
    auto *chipsLayout = new QVBoxLayout(m_tagChips);
    chipsLayout->setContentsMargins(0, 0, 0, 0);
    chipsLayout->setSpacing(4);
    tagsLayout->addWidget(m_tagChips);
    m_tagEditor = new QLineEdit(tags);
    m_tagEditor->setObjectName(QStringLiteral("previewTagEditor"));
    m_tagEditor->setPlaceholderText(tr("Add Tags…"));
    m_tagEditor->setAccessibleName(tr("Add a file tag"));
    m_tagEditor->setMaxLength(256);
    tagsLayout->addWidget(m_tagEditor);
    connect(m_tagEditor, &QLineEdit::returnPressed, this, [this] {
        requestTagChange(m_tagEditor->text(), 1);
    });
    m_tagColors = new QWidget(tags);
    m_tagColors->setObjectName(QStringLiteral("previewTagColors"));
    auto *colorsLayout = new QHBoxLayout(m_tagColors);
    colorsLayout->setContentsMargins(0, 0, 0, 0);
    colorsLayout->setSpacing(3);
    for (const QString &tag : FileTags::standardTags()) {
        auto *color = new QToolButton(m_tagColors);
        color->setObjectName(QStringLiteral("previewTag%1").arg(tag));
        color->setProperty("fileTag", tag);
        color->setAccessibleName(tr("Toggle %1 tag").arg(tag));
        color->setToolTip(tag);
        color->setCheckable(true);
        color->setFixedSize(23, 23);
        QPixmap dot(18, 18);
        dot.fill(Qt::transparent);
        QPainter painter(&dot);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(FileTags::tagColor(tag));
        painter.drawEllipse(QRectF(2, 2, 14, 14));
        color->setIcon(QIcon(dot));
        color->setIconSize(QSize(18, 18));
        m_colorButtons.append(color);
        colorsLayout->addWidget(color);
        connect(color, &QToolButton::clicked, this, [this, tag] {
            requestTagChange(tag, m_tags.contains(tag) ? 2 : 1);
        });
    }
    colorsLayout->addStretch();
    tagsLayout->addWidget(m_tagColors);
    m_tagStatus = new QLabel(tags);
    m_tagStatus->setObjectName(QStringLiteral("previewTagStatus"));
    m_tagStatus->setTextFormat(Qt::PlainText);
    m_tagStatus->setWordWrap(true);
    tagsLayout->addWidget(m_tagStatus);
    bodyLayout->addWidget(tags);
    bodyLayout->addStretch();
    scroll->setWidget(body);
    details->addWidget(scroll, 1);

    auto *actions = new QWidget(m_gallerySidebar);
    actions->setObjectName(QStringLiteral("previewQuickActions"));
    auto *actionsLayout = new QHBoxLayout(actions);
    actionsLayout->setContentsMargins(5, 10, 5, 10);
    actionsLayout->setSpacing(2);
    m_getInfo = button(tr("Get Info"), QStringLiteral("previewGetInfo"), actions);
    connect(m_getInfo, &QPushButton::clicked, this, [this] {
        if (!currentUrl().isEmpty()) emit getInfoRequested(currentUrl());
    });
    actionsLayout->addWidget(m_getInfo, 1);
    m_moreActions = new QToolButton(actions);
    m_moreActions->setObjectName(QStringLiteral("previewMoreActions"));
    m_moreActions->setText(tr("More…"));
    m_moreActions->setPopupMode(QToolButton::InstantPopup);
    auto *menu = new QMenu(m_moreActions);
    m_copyLocation = menu->addAction(tr("Copy Location"));
    menu->addAction(m_openWithAction);
    menu->addAction(m_printAction);
    menu->addMenu(m_nativeActions->menu()->actions().last()->menu());
    m_copyLocation->setObjectName(QStringLiteral("previewCopyLocation"));
    connect(m_copyLocation, &QAction::triggered, this, [this] {
        const QUrl url = currentUrl();
        if (!url.isEmpty()) QGuiApplication::clipboard()->setText(url.isLocalFile()
            ? url.toLocalFile() : url.toDisplayString(QUrl::RemovePassword));
    });
    m_rotateCopy = menu->addAction(tr("Rotate Left as Copy…"));
    m_rotateCopy->setObjectName(QStringLiteral("previewRotateCopy"));
    connect(m_rotateCopy, &QAction::triggered, this, [this] {
        if (m_rotatableImage && !currentUrl().isEmpty()) emit rotateCopyRequested(currentUrl());
    });
    m_moreActions->setMenu(menu);
    actionsLayout->addWidget(m_moreActions, 1);
    details->addWidget(actions);
    row->addWidget(m_gallerySidebar);
    updateGalleryMetadata();
    const QImage thumbnail = m_thumbnailImage;
    updateThumbnail(thumbnail);
    updateTagWidgets();
    updateQuickActions();
}

void PreviewPane::setInformation(const QString &key, const QString &value, bool detail)
{
    auto &rows = detail ? m_moreInformation : m_information;
    const auto found = std::find_if(rows.begin(), rows.end(), [&](const auto &row) { return row.first == key; });
    if (found != rows.end()) {
        if (value.isEmpty()) rows.erase(found);
        else found->second = value;
    } else if (!value.isEmpty()) rows.append({key, value});
    updateGalleryMetadata();
}

void PreviewPane::updateGalleryMetadata()
{
    if (!m_gallery || !m_kindSize) return;
    m_kindSize->setText(m_kindSizeText);
    m_showMore->setText(m_moreDetails ? tr("Show Less") : tr("Show More"));
    m_showMore->setVisible(!m_moreInformation.isEmpty());
    const QColor foreground = palette().color(QPalette::WindowText);
    const QColor background = m_gallerySidebar->palette().color(QPalette::Window);
    const QColor secondary = QColor::fromRgbF(
        foreground.redF() * .7 + background.redF() * .3,
        foreground.greenF() * .7 + background.greenF() * .3,
        foreground.blueF() * .7 + background.blueF() * .3);
    QString html = QStringLiteral("<table width=\"100%\" cellspacing=\"0\" cellpadding=\"3\">");
    auto append = [&](const auto &rows) {
        for (const auto &row : rows) {
            QString value = row.second.toHtmlEscaped();
            value.replace(QLatin1Char('/'), QStringLiteral("/&#8203;"));
            html += QStringLiteral("<tr><td valign=\"top\" style=\"color:%1;white-space:nowrap;\">%2</td>"
                "<td align=\"right\" style=\"color:%3;\">%4</td></tr>")
                .arg(secondary.name(), row.first.toHtmlEscaped(), foreground.name(), value);
        }
    };
    append(m_information);
    if (m_moreDetails) append(m_moreInformation);
    html += QStringLiteral("</table>");
    m_metadata->setText(html);
    m_metadata->setToolTip(m_metadataText);
}

void PreviewPane::updateThumbnail(const QImage &image)
{
    m_thumbnailImage = image;
    if (!m_thumbnail) return;
    if (!image.isNull()) {
        m_thumbnail->setPixmap(QPixmap::fromImage(image.scaled(m_thumbnail->size() * devicePixelRatioF(),
            Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        QPixmap pixmap = m_thumbnail->pixmap();
        pixmap.setDevicePixelRatio(devicePixelRatioF());
        m_thumbnail->setPixmap(pixmap);
    } else {
        const QFileInfo info(currentUrl().toLocalFile());
        const QString icon = info.isDir() ? QStringLiteral("folder")
            : QMimeDatabase().mimeTypeForUrl(currentUrl()).iconName();
        m_thumbnail->setPixmap(QIcon::fromTheme(icon, QIcon::fromTheme(QStringLiteral("text-x-generic")))
            .pixmap(QSize(40, 48)));
    }
}

void PreviewPane::refreshTags()
{
    if (!m_gallery) return;
    const QUrl url = currentUrl();
    m_tagWork->cancel();
    const quint64 generation = ++m_tagGeneration;
    m_tags.clear();
    m_tagReadError.clear();
    m_readingTags = !url.isEmpty() && !m_pendingTags.contains(url);
    updateTagWidgets();
    if (!m_readingTags) return;
    struct Result { QStringList tags; QString error; };
    submitPreview(m_tagWork, this, [url](const auto &cancelled) {
        Result result;
        if (!cancelled->load()) result.tags = FileTags::tags(url, &result.error);
        return result;
    }, [this, url, generation](const Result &result) {
        if (generation != m_tagGeneration || url != currentUrl()) return;
        m_readingTags = false;
        m_tags = result.tags;
        m_tagReadError = result.error;
        updateTagWidgets();
    });
}

void PreviewPane::updateTagWidgets()
{
    if (!m_tagEditor) return;
    const bool pending = m_pendingTags.contains(currentUrl());
    const bool editable = !currentUrl().isEmpty() && !m_readingTags && !pending && m_tagReadError.isEmpty();
    m_tagEditor->setEnabled(editable);
    for (auto *color : m_colorButtons) {
        color->setChecked(m_tags.contains(color->property("fileTag").toString()));
        color->setEnabled(editable);
    }
    auto *chips = qobject_cast<QVBoxLayout *>(m_tagChips->layout());
    while (auto *item = chips->takeAt(0)) {
        // A chip can be the sender of this update; dispose after its clicked
        // signal returns rather than destroying it during event dispatch.
        if (auto *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    for (const QString &tag : m_tags) {
        auto *chip = new QToolButton(m_tagChips);
        chip->setObjectName(QStringLiteral("previewTagChip"));
        chip->setText(chip->fontMetrics().elidedText(tag, Qt::ElideRight,
            std::max(100, m_gallerySidebar->width() - 65)) + QStringLiteral("  ×"));
        chip->setToolTip(tr("Remove %1 tag").arg(tag));
        chip->setAccessibleName(tr("Remove %1 tag").arg(tag));
        chip->setProperty("fileTag", tag);
        chip->setEnabled(editable);
        chips->addWidget(chip, 0, Qt::AlignLeft);
        connect(chip, &QToolButton::clicked, this, [this, tag] { requestTagChange(tag, 2); });
    }
    m_tagChips->setVisible(!m_tags.isEmpty());
    const QString status = pending ? tr("Saving tags…") : m_readingTags ? tr("Reading tags…")
        : !m_tagReadError.isEmpty() ? m_tagReadError : m_tagError;
    m_tagStatus->setText(status);
    m_tagStatus->setVisible(!status.isEmpty());
}

void PreviewPane::requestTagChange(const QString &value, int operation)
{
    const QString tag = value.trimmed();
    if (tag.isEmpty() || currentUrl().isEmpty() || m_readingTags ||
        m_pendingTags.contains(currentUrl()) || !m_tagReadError.isEmpty()) return;
    if (tag.contains(QLatin1Char(',')) || std::any_of(tag.cbegin(), tag.cend(), [](QChar c) {
        return c.category() == QChar::Other_Control;
    })) {
        m_tagError = tr("Tags cannot contain commas or control characters.");
        updateTagWidgets();
        return;
    }
    ++m_tagGeneration;
    m_tagWork->cancel();
    m_tagErrors.remove(currentUrl());
    m_pendingTags.insert(currentUrl());
    m_tagError.clear();
    updateTagWidgets();
    emit tagChangeRequested(currentUrl(), tag, operation);
}

void PreviewPane::tagChangeFinished(const QUrl &url, const QString &error)
{
    m_pendingTags.remove(url);
    if (error.isEmpty()) m_tagErrors.remove(url);
    else m_tagErrors.insert(url, error);
    if (url != currentUrl()) return;
    if (error.isEmpty()) {
        m_tagError.clear();
        if (m_tagEditor) m_tagEditor->clear();
        refreshTags();
    } else {
        m_tagError = error;
        updateTagWidgets();
    }
}

void PreviewPane::updateQuickActions()
{
    if (!m_getInfo) return;
    const bool available = !currentUrl().isEmpty() && (!currentUrl().isLocalFile()
        || QFileInfo::exists(currentUrl().toLocalFile()));
    m_getInfo->setEnabled(available);
    m_moreActions->setEnabled(available);
    m_copyLocation->setEnabled(available);
    m_rotateCopy->setVisible(m_rotatableImage);
    m_rotateCopy->setEnabled(available && m_rotatableImage);
}

void PreviewPane::cancelNativePreview()
{
    if (!m_nativePreview) return;
    auto *job = m_nativePreview.data();
    m_nativePreview.clear();
    job->disconnect(this);
    job->kill();
}

void PreviewPane::requestNativePreview(bool contentPreview)
{
    const QUrl url = currentUrl();
    const QFileInfo info(url.toLocalFile());
    if (!url.isLocalFile() || !info.isFile() || !info.isReadable() || info.size() > nativePreviewLimit) return;
    const QStringList plugins = KIO::PreviewJob::defaultPlugins();
    if (plugins.isEmpty()) return;
    cancelNativePreview();
    const QString fingerprint = m_fingerprint;
    auto *job = KIO::filePreview({KFileItem(url, m_mime, 0)}, contentPreview ? QSize(1024, 768) : QSize(96, 96), &plugins);
    m_nativePreview = job;
    job->setScaleType(KIO::PreviewJob::ScaledAndCached);
    job->setIgnoreMaximumSize(false);
    connect(job, &KIO::PreviewJob::gotPreview, this,
        [this, url, fingerprint, contentPreview](const KFileItem &, const QPixmap &pixmap) {
            if (url != currentUrl() || fingerprint != m_fingerprint || pixmap.isNull()) return;
            QImage image = pixmap.toImage();
            if (image.width() > renderLimit.width() || image.height() > renderLimit.height())
                image = image.scaled(renderLimit, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            if (contentPreview) {
                auto *raster = new PreviewRaster;
                raster->image = image;
                m_images.insert(fingerprint + QStringLiteral("|native"), raster,
                    std::max<qsizetype>(1, image.sizeInBytes() / 1024));
                showImage(image);
            } else updateThumbnail(image);
        });
    // Unsupported documents retain a truthful readable fallback. Native KDE
    // thumbnail workers add installed office/raw/other format support safely.
    connect(job, &KJob::result, this, [this, job] { if (m_nativePreview == job) m_nativePreview.clear(); });
}

void PreviewPane::showCurrent()
{
    const QUrl url = currentUrl();
    m_previous->setEnabled(m_index > 0);
    m_next->setEnabled(m_index >= 0 && m_index + 1 < m_urls.size());
    m_previous->setVisible(m_urls.size() > 1);
    m_next->setVisible(m_urls.size() > 1);
    m_count->setText(m_urls.size() > 1 ? tr("%1 of %2").arg(m_index + 1).arg(m_urls.size()) : QString());
    m_open->setEnabled(!url.isEmpty());
    m_name->setText(url.fileName());
    m_name->setToolTip(url.toDisplayString(QUrl::RemovePassword));
    updateSourceWatch();
    if (m_contactSheet && !url.isEmpty()) {
        m_content->setCurrentWidget(m_contactList);
        updatePreviewControls();
        return;
    }
    if (url.isEmpty()) {
        cancelNativePreview();
        m_renderWork->cancel();
        ++m_previewGeneration;
        m_fingerprint.clear();
        m_mime.clear();
        m_tagError = m_tagErrors.value(url);
        m_kindSizeText.clear();
        m_information.clear();
        m_moreInformation.clear();
        m_rotatableImage = false;
        stopMedia();
        m_pageCount = 0;
        m_originalImageSize = {};
        m_image = QImage();
        m_pageControls->hide();
        m_mediaControls->hide();
        setMetadata({});
        updateThumbnail();
        updateQuickActions();
        refreshTags();
        showFallback(tr("Select a file to preview"));
        return;
    }
    if (!url.isLocalFile()) {
        cancelNativePreview();
        m_renderWork->cancel();
        ++m_previewGeneration;
        stopMedia();
        m_pageCount = 0;
        m_originalImageSize = {};
        m_fingerprint.clear();
        m_mime.clear();
        m_tagError = m_tagErrors.value(url);
        m_image = QImage();
        m_kindSizeText = tr("Remote item");
        m_information.clear();
        m_moreInformation.clear();
        m_rotatableImage = false;
        setInformation(tr("Location"), url.toDisplayString(QUrl::RemovePassword), true);
        m_pageControls->hide();
        m_mediaControls->hide();
        setMetadata(url.toDisplayString(QUrl::RemovePassword));
        updateThumbnail();
        updateQuickActions();
        refreshTags();
        showFallback(tr("Preview is available for local files.\nOpen this item with its default application."));
        return;
    }
    const QFileInfo info(url.toLocalFile());
    const QString fingerprint = url.toString() + QLatin1Char('|') + QString::number(info.size())
        + QLatin1Char('|') + QString::number(info.lastModified().toMSecsSinceEpoch());
    if (fingerprint == m_fingerprint) return;
    cancelNativePreview();
    m_renderWork->cancel();
    ++m_previewGeneration;
    m_fingerprint = fingerprint;
    m_mime.clear();
    m_tagError = m_tagErrors.value(url);
    m_information.clear();
    m_moreInformation.clear();
    m_rotatableImage = false;
    if (m_tagEditor) m_tagEditor->clear();
    stopMedia();
    m_pageCount = 0;
    m_originalImageSize = {};
    m_page = 0;
    m_image = QImage();
    resetZoom();
    static_cast<PreviewImageLabel *>(m_picture)->raster = {};
    m_picture->clear();
    m_text->clear();
    m_pageControls->hide();
    m_mediaControls->hide();
    m_open->setEnabled(info.exists());
    updateThumbnail();
    updateQuickActions();
    refreshTags();
    if (!info.exists()) {
        m_kindSizeText = tr("Unavailable item");
        setMetadata(url.toDisplayString());
        showFallback(tr("This item is no longer available."));
        return;
    }
    const auto date = [](const QDateTime &value) {
        return value.isValid() ? QLocale().toString(value, QLocale::ShortFormat) : QString();
    };
    setInformation(tr("Created"), date(info.birthTime()));
    setInformation(tr("Modified"), date(info.lastModified()));
    setInformation(tr("Last Opened"), date(info.lastRead()), true);
    setInformation(tr("Where"), info.absolutePath(), true);
    setInformation(tr("Name & Extension"), info.fileName(), true);
    if (info.isDir()) {
        m_kindSizeText = tr("Folder");
        setMetadata(tr("Folder · Modified %1").arg(QLocale().toString(info.lastModified(), QLocale::ShortFormat)));
        showFallback(tr("%1\n\nFolder").arg(info.fileName()));
        return;
    }
    if (!info.isFile()) {
        m_kindSizeText = tr("Special file");
        setMetadata(info.absoluteFilePath());
        showFallback(tr("Preview is unavailable for this file type."));
        return;
    }
    const QMimeType mime = QMimeDatabase().mimeTypeForFile(info, QMimeDatabase::MatchExtension);
    m_mime = mime.name();
    const QString kind = mime.comment().isEmpty() ? mime.name() : mime.comment();
    m_kindSizeText = tr("%1 – %2").arg(kind, QLocale().formattedDataSize(info.size()));
    setInformation(tr("Kind"), kind, true);
    setInformation(tr("Size"), tr("%1 (%2 bytes)").arg(QLocale().formattedDataSize(info.size()),
        QLocale().toString(info.size())), true);
    setMetadata(tr("%1 · %2 · Modified %3")
        .arg(mime.comment(), QLocale().formattedDataSize(info.size()),
             QLocale().toString(info.lastModified(), QLocale::ShortFormat)));

    if (m_mime.startsWith(QStringLiteral("image/")) || mime.isDefault()) {
        requestRaster(false);
        return;
    }
    if (mime.inherits(QStringLiteral("application/pdf"))) {
        requestRaster(true);
        return;
    }
    const QStringList textSuffixes = {QStringLiteral("md"), QStringLiteral("json"), QStringLiteral("yaml"),
        QStringLiteral("yml"), QStringLiteral("py"), QStringLiteral("js"), QStringLiteral("ts"),
        QStringLiteral("qml"), QStringLiteral("sh"), QStringLiteral("xml"), QStringLiteral("log"),
        QStringLiteral("csv"), QStringLiteral("cpp"), QStringLiteral("h"), QStringLiteral("rs"),
        QStringLiteral("toml"), QStringLiteral("ini"), QStringLiteral("conf"), QStringLiteral("c"),
        QStringLiteral("hpp"), QStringLiteral("css"), QStringLiteral("go"), QStringLiteral("java"),
        QStringLiteral("rb"), QStringLiteral("swift"), QStringLiteral("sql"), QStringLiteral("srt"),
        QStringLiteral("vtt"), QStringLiteral("desktop"), QStringLiteral("cmake")};
    if (m_mime.startsWith(QStringLiteral("text/")) || textSuffixes.contains(info.suffix().toLower())
        || m_mime.endsWith(QStringLiteral("+xml")) || m_mime.endsWith(QStringLiteral("+json"))
        || mime.inherits(QStringLiteral("application/json")) || mime.inherits(QStringLiteral("application/xml"))) {
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly)) {
            showFallback(tr("The file could not be read."));
            return;
        }
        const QByteArray bytes = file.read(textLimit + 1);
        QString text;
        if (bytes.startsWith("\xff\xfe") || bytes.startsWith("\xfe\xff")) {
            const bool littleEndian = bytes.startsWith("\xff\xfe");
            const QByteArray bounded = bytes.left(textLimit);
            text.reserve(bounded.size() / 2);
            for (qsizetype i = 2; i + 1 < bounded.size(); i += 2) {
                const auto a = static_cast<unsigned char>(bounded[i]), b = static_cast<unsigned char>(bounded[i + 1]);
                text.append(QChar(littleEndian ? quint16(a | (b << 8)) : quint16((a << 8) | b)));
            }
        } else if (bytes.contains('\0')) {
            showFallback(tr("This file contains binary data.\nOpen it with its default application."));
            return;
        } else text = QString::fromUtf8(bytes.left(textLimit));
        m_text->setPlainText(text);
        m_content->setCurrentWidget(m_text);
        updatePreviewControls();
        if (bytes.size() > textLimit) {
            setInformation(tr("Preview"), tr("First 512 KiB"), true);
            setMetadata(m_metadataText + tr(" · Preview limited to the first 512 KiB"));
        }
        return;
    }
    if (m_mime.startsWith(QStringLiteral("audio/")) || m_mime.startsWith(QStringLiteral("video/"))) {
        m_mediaPath = info.absoluteFilePath();
        m_mediaControls->show();
        m_play->setText(tr("Play"));
        m_seek->setEnabled(false);
        m_seek->setValue(0);
        m_playbackTime->setText(QStringLiteral("0:00 / 0:00"));
        showFallback(m_mime.startsWith(QStringLiteral("video/")) ? tr("Video\nPress Play to preview")
                                                                  : tr("Audio\nPress Play to preview"));
        if (m_gallery) requestNativePreview(false);
        return;
    }
    showFallback(tr("%1\n\n%2\nOpen with its default application to view this format.").arg(info.fileName(), mime.comment()));
    if (const PreviewRaster *cached = m_images.object(fingerprint + QStringLiteral("|native"))) showImage(cached->image);
    else requestNativePreview(true);
}

void PreviewPane::showImage(const QImage &image)
{
    m_image = image;
    m_picture->clear();
    m_content->setCurrentWidget(m_picture);
    static_cast<PreviewImageLabel *>(m_picture)->raster = image;
    updateThumbnail(image);
    updateImageSize();
    updatePreviewControls();
    QTimer::singleShot(0, this, &PreviewPane::updateImageSize);
}

void PreviewPane::updateImageSize()
{
    if (m_image.isNull()) return;
    auto *picture = static_cast<PreviewImageLabel *>(m_picture);
    picture->zoom = m_zoom;
    picture->constrainPan();
    picture->update();
    const QSize size = m_picture->contentsRect().size();
    if (size.width() < 1 || size.height() < 1) return;
    const qreal dpr = devicePixelRatioF();
    QSize target = m_image.size().scaled(size * dpr, Qt::KeepAspectRatio);
    if (target.width() > renderLimit.width() || target.height() > renderLimit.height())
        target.scale(renderLimit, Qt::KeepAspectRatio);
    if (m_picture->pixmap().size() == target && qFuzzyCompare(m_picture->pixmap().devicePixelRatio(), dpr)) return;
    QPixmap pixmap = QPixmap::fromImage(m_image.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    pixmap.setDevicePixelRatio(dpr);
    m_picture->setPixmap(pixmap);
}

void PreviewPane::requestRaster(bool pdf, int page)
{
    const QUrl url = currentUrl();
    const QString fingerprint = m_fingerprint;
    const quint64 generation = ++m_previewGeneration;
    const QString key = fingerprint + (pdf ? QStringLiteral("|page:%1").arg(page) : QString());
    m_page = page;
    if (const PreviewRaster *cached = m_images.object(key)) {
        m_renderWork->cancel();
        applyRaster(*cached, pdf);
        return;
    }
    showFallback(tr("Loading preview…"));
    submitPreview(m_renderWork, this,
        [path = url.toLocalFile(), pdf, page](const auto &cancelled) {
            return readRaster(path, pdf, page, renderLimit, cancelled);
        },
        [this, url, fingerprint, generation, key, pdf](const PreviewRaster &raster) {
            if (generation != m_previewGeneration || url != currentUrl() || fingerprint != m_fingerprint) return;
            if (!raster.image.isNull()) m_images.insert(key, new PreviewRaster(raster),
                std::max<qsizetype>(1, (raster.image.sizeInBytes() + 1023) / 1024));
            applyRaster(raster, pdf);
        });
}

void PreviewPane::applyRaster(const PreviewRaster &raster, bool pdf)
{
    if (pdf) {
        m_pageCount = raster.pageCount;
        if (m_pageCount > 0) {
            m_page = std::clamp(m_page, 0, m_pageCount - 1);
            setInformation(tr("Pages"), QLocale().toString(m_pageCount));
            m_pageControls->show();
            m_pagePrevious->setEnabled(m_page > 0);
            m_pageNext->setEnabled(m_page + 1 < m_pageCount);
            m_pageLabel->setText(tr("Page %1 of %2").arg(m_page + 1).arg(m_pageCount));
            setInformation(tr("Page Size"), tr("%1 × %2 pt")
                .arg(QLocale().toString(raster.pagePoints.width(), 'f', 1),
                     QLocale().toString(raster.pagePoints.height(), 'f', 1)), true);
        }
    } else {
        m_originalImageSize = raster.originalSize;
        m_rotatableImage = raster.rotatable;
        if (raster.originalSize.isValid())
            setInformation(tr("Dimensions"), tr("%1 × %2").arg(raster.originalSize.width()).arg(raster.originalSize.height()));
        if (!raster.resolution.isEmpty())
            setInformation(tr("Resolution"), tr("%1 × %2 dpi")
                .arg(QLocale().toString(raster.resolution.width(), 'f', 0),
                     QLocale().toString(raster.resolution.height(), 'f', 0)));
        setInformation(tr("Color Space"), raster.image.colorSpace().description(), true);
        updateQuickActions();
    }
    if (raster.image.isNull()) showFallback(raster.error);
    else showImage(raster.image);
}

void PreviewPane::renderPage(int page)
{
    if (m_pageCount < 1) return;
    requestRaster(true, std::clamp(page, 0, m_pageCount - 1));
}

void PreviewPane::stopMedia()
{
    if (m_player) {
        m_player->stop();
        m_player->setSource(QUrl());
    }
    m_mediaPath.clear();
}

void PreviewPane::toggleMedia()
{
    if (m_mediaPath.isEmpty()) return;
    if (!m_player) {
        m_player = new QMediaPlayer(this);
        m_audio = new QAudioOutput(this);
        m_audio->setVolume(.6f);
        m_player->setAudioOutput(m_audio);
        m_video = new QVideoWidget(m_videoPage);
        m_video->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        m_videoPage->layout()->addWidget(m_video);
        m_player->setVideoOutput(m_video);
        connect(m_player, &QMediaPlayer::durationChanged, this, [this] { updatePlaybackTime(); });
        connect(m_player, &QMediaPlayer::positionChanged, this, [this] { updatePlaybackTime(); });
        connect(m_player, &QMediaPlayer::seekableChanged, m_seek, &QSlider::setEnabled);
        connect(m_player, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState state) {
            m_play->setText(state == QMediaPlayer::PlayingState ? tr("Pause") : tr("Play"));
        });
        connect(m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &message) {
            showFallback(tr("Playback is unavailable: %1\nOpen with the default application.").arg(message));
        });
    }
    if (m_player->source().isEmpty()) {
        m_player->setSource(QUrl::fromLocalFile(m_mediaPath));
        if (m_mime.startsWith(QStringLiteral("video/"))) m_content->setCurrentWidget(m_videoPage);
    }
    if (m_player->playbackState() == QMediaPlayer::PlayingState) m_player->pause();
    else m_player->play();
}

void PreviewPane::updatePlaybackTime()
{
    if (!m_player) return;
    m_playbackTime->setText(playbackTime(m_player->position()) + QStringLiteral(" / ") + playbackTime(m_player->duration()));
    if (!m_seek->isSliderDown() && m_player->duration() > 0)
        m_seek->setValue(int(m_player->position() * 10000 / m_player->duration()));
}

void PreviewPane::showFallback(const QString &message)
{
    m_fallback->setText(message);
    m_content->setCurrentWidget(m_fallback);
    updatePreviewControls();
}

void PreviewPane::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_gallery) updateGalleryWidth();
    updateImageSize();
    if (m_contactSheet) m_contactTimer->start();
}

void PreviewPane::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange) updateGalleryMetadata();
    if (event->type() == QEvent::WindowStateChange) updatePreviewControls();
}

void PreviewPane::hideEvent(QHideEvent *event)
{
    finishPrint(tr("Printing was cancelled."));
    cancelNativePreview();
    m_renderWork->cancel();
    m_tagWork->cancel();
    m_contactWork->cancel();
    ++m_contactGeneration;
    m_contactTimer->stop();
    m_refreshTimer->stop();
    m_slideshowTimer->stop();
    updateSourceWatch();
    ++m_previewGeneration;
    ++m_tagGeneration;
    m_fingerprint.clear();
    if (m_player) m_player->stop();
    QWidget::hideEvent(event);
}

void PreviewPane::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    showCurrent();
    if (window() != this) window()->installEventFilter(this);
    if (m_contactSheet) m_contactTimer->start();
}

void PreviewPane::keyPressEvent(QKeyEvent *event)
{
    const bool command = event->modifiers().testFlag(Qt::MetaModifier) || event->modifiers().testFlag(Qt::ControlModifier);
    QWidget *focused = QApplication::focusWidget();
    const bool controlFocus = qobject_cast<QLineEdit *>(focused) || qobject_cast<QTextEdit *>(focused)
        || qobject_cast<QAbstractButton *>(focused) || qobject_cast<QSlider *>(focused);
    if (!m_compact && !m_gallery && command && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
        setContactSheet(!m_contactSheet);
        event->accept();
    } else if (command && (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal || event->key() == Qt::Key_Minus)
               && !m_image.isNull() && !m_contactSheet && m_content->currentWidget() == m_picture) {
        setZoomFactor(m_zoom * (event->key() == Qt::Key_Minus ? 1 / 1.2 : 1.2));
        event->accept();
    } else if (command && event->key() == Qt::Key_0 && m_content->currentWidget() == m_picture) {
        resetZoom();
        event->accept();
    } else if (!m_compact && !m_gallery && command && event->key() == Qt::Key_F) {
        setFullScreen(!isFullScreen());
        event->accept();
    } else if (!m_compact && !m_gallery && event->key() == Qt::Key_Escape) {
        if (isFullScreen()) { setSlideshow(false); setFullScreen(false); }
        else if (m_contactSheet) setContactSheet(false);
        else requestClose();
        event->accept();
    } else if (!m_compact && !m_gallery && event->key() == Qt::Key_Space && !controlFocus && event->modifiers() == Qt::NoModifier) {
        requestClose();
        event->accept();
    } else if (!controlFocus && !m_contactSheet && event->key() == Qt::Key_Left && event->modifiers() == Qt::NoModifier) {
        advance(-1);
        event->accept();
    } else if (!controlFocus && !m_contactSheet && event->key() == Qt::Key_Right && event->modifiers() == Qt::NoModifier) {
        advance(1);
        event->accept();
    } else QWidget::keyPressEvent(event);
}

bool PreviewPane::eventFilter(QObject *object, QEvent *event)
{
    if (object == m_picture && event->type() == QEvent::Resize) updateImageSize();
    if (object == window() && event->type() == QEvent::WindowStateChange) updatePreviewControls();
    return QWidget::eventFilter(object, event);
}

#include "preview.moc"
