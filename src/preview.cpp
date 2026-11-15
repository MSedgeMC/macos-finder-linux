// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#include "preview.h"

#include <QAudioOutput>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QMediaPlayer>
#include <QMimeDatabase>
#include <QPainter>
#include <QPdfDocument>
#include <QPushButton>
#include <QResizeEvent>
#include <QSlider>
#include <QStackedWidget>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QVideoWidget>
#include <algorithm>

namespace {
constexpr qsizetype textLimit = 512 * 1024;
constexpr qint64 maxDecodedPixels = 16 * 1024 * 1024;
const QSize renderLimit(1600, 1200);

QString playbackTime(qint64 milliseconds)
{
    const qint64 seconds = std::max<qint64>(0, milliseconds / 1000);
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
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

PreviewPane::PreviewPane(QWidget *parent) : QWidget(parent), m_pdf(new QPdfDocument(this))
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
    m_picture = new QLabel(this);
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
    previewLayout->addWidget(m_content, 1);

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

    connect(m_close, &QPushButton::clicked, this, &PreviewPane::closeRequested);
    connect(m_previous, &QPushButton::clicked, this, [this] { advance(-1); });
    connect(m_next, &QPushButton::clicked, this, [this] { advance(1); });
    connect(m_open, &QPushButton::clicked, this, [this] {
        if (!currentUrl().isEmpty()) emit openRequested(currentUrl());
    });
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
    stopMedia();
}

void PreviewPane::setUrls(const QList<QUrl> &urls)
{
    const QUrl previous = currentUrl();
    QList<QUrl> valid;
    for (const QUrl &url : urls)
        if (url.isValid() && !url.isEmpty() && !valid.contains(url)) valid.append(url);
    m_urls = valid;
    m_index = m_urls.isEmpty() ? -1 : std::max(0, int(m_urls.indexOf(previous)));
    showCurrent();
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
}

void PreviewPane::setCompact(bool compact)
{
    m_close->setVisible(!compact);
    layout()->setContentsMargins(compact ? 12 : 16, 12, compact ? 12 : 16, 12);
}

void PreviewPane::setGallery(bool gallery)
{
    if (m_gallery == gallery) return;
    m_gallery = gallery;
    m_layout->removeWidget(m_previewArea);
    m_layout->removeWidget(m_metadata);
    if (gallery) {
        if (!m_galleryArea) {
            m_galleryArea = new QWidget(this);
            m_galleryArea->setObjectName(QStringLiteral("previewGalleryArea"));
            auto *row = new QHBoxLayout(m_galleryArea);
            row->setContentsMargins(0, 0, 0, 0);
            row->setSpacing(18);
            m_gallerySidebar = new QWidget(m_galleryArea);
            m_gallerySidebar->setObjectName(QStringLiteral("previewGalleryDetails"));
            auto *details = new QVBoxLayout(m_gallerySidebar);
            details->setContentsMargins(12, 14, 8, 10);
            details->setSpacing(18);
            details->addWidget(m_name);
            details->addWidget(m_metadata);
            details->addStretch();
            details->addWidget(m_count);
            details->addWidget(m_open, 0, Qt::AlignLeft);
            row->addWidget(m_previewArea, 1);
            row->addWidget(m_gallerySidebar);
        } else {
            auto *row = qobject_cast<QHBoxLayout *>(m_galleryArea->layout());
            row->insertWidget(0, m_previewArea, 1);
            auto *details = qobject_cast<QVBoxLayout *>(m_gallerySidebar->layout());
            details->insertWidget(0, m_name);
            details->insertWidget(1, m_metadata);
            details->insertWidget(3, m_count);
            details->addWidget(m_open, 0, Qt::AlignLeft);
        }
        m_layout->addWidget(m_galleryArea, 1);
        m_headerWidget->hide();
        m_galleryArea->show();
        m_name->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        m_name->setWordWrap(true);
        m_name->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        m_metadata->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        updateGalleryWidth();
    } else {
        m_layout->removeWidget(m_galleryArea);
        m_galleryArea->hide();
        m_layout->addWidget(m_previewArea, 1);
        m_layout->addWidget(m_metadata);
        m_header->insertWidget(3, m_name, 1);
        m_header->insertWidget(4, m_count);
        m_header->addWidget(m_open);
        m_headerWidget->show();
        m_name->setAlignment(Qt::AlignCenter);
        m_name->setWordWrap(false);
        m_name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        m_metadata->setAlignment(Qt::AlignCenter);
    }
    setMetadata(m_metadataText);
    updateImageSize();
}

void PreviewPane::updateGalleryWidth()
{
    if (m_gallerySidebar) m_gallerySidebar->setFixedWidth(std::clamp(width() / 3, 160, 220));
}

void PreviewPane::setMetadata(const QString &text)
{
    m_metadataText = text;
    QString display = text;
    if (m_gallery) display.replace(QStringLiteral(" · "), QStringLiteral("\n\n"));
    m_metadata->setText(display);
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
    m_name->setToolTip(url.toDisplayString());
    if (url.isEmpty()) {
        m_fingerprint.clear();
        stopMedia();
        m_pdf->close();
        m_image = QImage();
        m_pageControls->hide();
        m_mediaControls->hide();
        setMetadata({});
        showFallback(tr("Select a file to preview"));
        return;
    }
    if (!url.isLocalFile()) {
        stopMedia();
        m_pdf->close();
        m_fingerprint.clear();
        m_pageControls->hide();
        m_mediaControls->hide();
        setMetadata(url.toDisplayString());
        showFallback(tr("Preview is available for local files.\nOpen this item with its default application."));
        return;
    }
    const QFileInfo info(url.toLocalFile());
    const QString fingerprint = url.toString() + QLatin1Char('|') + QString::number(info.size())
        + QLatin1Char('|') + QString::number(info.lastModified().toMSecsSinceEpoch());
    if (fingerprint == m_fingerprint) return;
    m_fingerprint = fingerprint;
    stopMedia();
    m_pdf->close();
    m_page = 0;
    m_image = QImage();
    m_picture->clear();
    m_text->clear();
    m_pageControls->hide();
    m_mediaControls->hide();
    m_open->setEnabled(info.exists());
    if (!info.exists()) {
        setMetadata(url.toDisplayString());
        showFallback(tr("This item is no longer available."));
        return;
    }
    if (info.isDir()) {
        setMetadata(tr("Folder · Modified %1").arg(QLocale().toString(info.lastModified(), QLocale::ShortFormat)));
        showFallback(tr("%1\n\nFolder").arg(info.fileName()));
        return;
    }
    if (!info.isFile()) {
        setMetadata(info.absoluteFilePath());
        showFallback(tr("Preview is unavailable for this file type."));
        return;
    }
    const QMimeType mime = QMimeDatabase().mimeTypeForFile(info);
    m_mime = mime.name();
    setMetadata(tr("%1 · %2 · Modified %3")
        .arg(mime.comment(), QLocale().formattedDataSize(info.size()),
             QLocale().toString(info.lastModified(), QLocale::ShortFormat)));

    if (QImage *cached = m_images.object(fingerprint)) {
        showImage(*cached);
        return;
    }
    QImageReader reader(info.absoluteFilePath());
    reader.setAutoTransform(true);
    // Some image plugins claim arbitrary text/PDF inputs. Keep document and
    // text navigation in their proper native preview instead of accepting
    // those plugins' rasterized first page as an image.
    if (m_mime.startsWith(QStringLiteral("image/")) && reader.canRead()) {
        const QSize original = reader.size();
        const qint64 pixels = qint64(original.width()) * original.height();
        if (!original.isValid() || pixels <= 0 ||
            (pixels > maxDecodedPixels && !reader.supportsOption(QImageIOHandler::ScaledSize))) {
            showFallback(tr("This image is too large to preview safely.\nOpen it with its default application."));
            return;
        }
        QSize target = original;
        target.scale(renderLimit, Qt::KeepAspectRatio);
        reader.setScaledSize(target);
        QImage image = reader.read();
        if (!image.isNull()) {
            if (image.width() > renderLimit.width() || image.height() > renderLimit.height())
                image = image.scaled(renderLimit, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            m_images.insert(fingerprint, new QImage(image), std::max<qsizetype>(1, image.sizeInBytes() / 1024));
            setMetadata(m_metadataText + tr(" · %1 × %2").arg(original.width()).arg(original.height()));
            showImage(image);
            return;
        }
        showFallback(tr("The image could not be decoded.\nOpen it with its default application."));
        return;
    }
    if (mime.inherits(QStringLiteral("application/pdf"))) {
        const auto error = m_pdf->load(info.absoluteFilePath());
        if (error == QPdfDocument::Error::None && m_pdf->pageCount() > 0) {
            m_pageControls->show();
            renderPage(0);
        } else {
            showFallback(error == QPdfDocument::Error::IncorrectPassword
                ? tr("This PDF is password protected.\nOpen it with its default application.")
                : tr("The PDF could not be opened.\nOpen it with its default application."));
        }
        return;
    }
    const QStringList textSuffixes = {QStringLiteral("md"), QStringLiteral("json"), QStringLiteral("yaml"),
        QStringLiteral("yml"), QStringLiteral("py"), QStringLiteral("js"), QStringLiteral("ts"),
        QStringLiteral("qml"), QStringLiteral("sh"), QStringLiteral("xml"), QStringLiteral("log"),
        QStringLiteral("csv"), QStringLiteral("cpp"), QStringLiteral("h"), QStringLiteral("rs"),
        QStringLiteral("toml"), QStringLiteral("ini"), QStringLiteral("conf")};
    if (m_mime.startsWith(QStringLiteral("text/")) || textSuffixes.contains(info.suffix().toLower())) {
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly)) {
            showFallback(tr("The file could not be read."));
            return;
        }
        const QByteArray bytes = file.read(textLimit + 1);
        if (bytes.contains('\0')) {
            showFallback(tr("This file contains binary data.\nOpen it with its default application."));
            return;
        }
        m_text->setPlainText(QString::fromUtf8(bytes.left(textLimit)));
        m_content->setCurrentWidget(m_text);
        if (bytes.size() > textLimit)
            setMetadata(m_metadataText + tr(" · Preview limited to the first 512 KiB"));
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
        return;
    }
    showFallback(tr("%1\n\n%2\nOpen with its default application to view this format.").arg(info.fileName(), mime.comment()));
}

void PreviewPane::showImage(const QImage &image)
{
    m_image = image;
    m_picture->clear();
    m_content->setCurrentWidget(m_picture);
    updateImageSize();
    QTimer::singleShot(0, this, &PreviewPane::updateImageSize);
}

void PreviewPane::updateImageSize()
{
    if (m_image.isNull()) return;
    const QSize size = m_picture->size();
    if (size.width() < 1 || size.height() < 1) return;
    const QSize target = m_image.size().scaled(size, Qt::KeepAspectRatio);
    if (m_picture->pixmap().size() == target) return;
    m_picture->setPixmap(QPixmap::fromImage(m_image.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
}

void PreviewPane::renderPage(int page)
{
    if (m_pdf->pageCount() < 1) return;
    m_page = std::clamp(page, 0, m_pdf->pageCount() - 1);
    QSize size = m_pdf->pagePointSize(m_page).toSize();
    if (!size.isValid()) {
        showFallback(tr("This PDF page could not be rendered."));
        return;
    }
    size.scale(renderLimit, Qt::KeepAspectRatio);
    const QString key = m_fingerprint + QStringLiteral("|page:%1").arg(m_page);
    QImage image;
    if (const QImage *cached = m_images.object(key)) image = *cached;
    else {
        const QImage pageImage = m_pdf->render(m_page, size);
        if (!pageImage.isNull()) {
            // QtPdf leaves the page background transparent. Composite it on
            // paper white so black PDF content remains legible on dark glass.
            image = QImage(pageImage.size(), QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::white);
            QPainter painter(&image);
            painter.drawImage(0, 0, pageImage);
            m_images.insert(key, new QImage(image), std::max<qsizetype>(1, image.sizeInBytes() / 1024));
        }
    }
    if (image.isNull()) showFallback(tr("This PDF page could not be rendered."));
    else showImage(image);
    m_pagePrevious->setEnabled(m_page > 0);
    m_pageNext->setEnabled(m_page + 1 < m_pdf->pageCount());
    m_pageLabel->setText(tr("Page %1 of %2").arg(m_page + 1).arg(m_pdf->pageCount()));
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
}

void PreviewPane::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_gallery) updateGalleryWidth();
    updateImageSize();
}

void PreviewPane::hideEvent(QHideEvent *event)
{
    if (m_player) m_player->stop();
    QWidget::hideEvent(event);
}

void PreviewPane::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Space) {
        emit closeRequested();
        event->accept();
    } else if (event->key() == Qt::Key_Left && event->modifiers() == Qt::NoModifier) {
        advance(-1);
        event->accept();
    } else if (event->key() == Qt::Key_Right && event->modifiers() == Qt::NoModifier) {
        advance(1);
        event->accept();
    } else QWidget::keyPressEvent(event);
}

bool PreviewPane::eventFilter(QObject *object, QEvent *event)
{
    if (object == m_picture && event->type() == QEvent::Resize) updateImageSize();
    return QWidget::eventFilter(object, event);
}
