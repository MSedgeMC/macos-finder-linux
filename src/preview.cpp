// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#include "preview.h"
#include "tags.h"

#include <KFileItem>
#include <KIO/PreviewJob>
#include <QAction>
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
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMediaPlayer>
#include <QMenu>
#include <QMimeDatabase>
#include <QPainter>
#include <QPdfDocument>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSlider>
#include <QStackedWidget>
#include <QTextEdit>
#include <QTimer>
#include <QThread>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVideoWidget>
#include <QGuiApplication>
#include <QtEndian>
#include <algorithm>
#include <memory>

namespace {
constexpr qsizetype textLimit = 512 * 1024;
constexpr qint64 maxDecodedPixels = 16 * 1024 * 1024;
const QSize renderLimit(1600, 1200);
constexpr qint64 nativePreviewLimit = 64 * 1024 * 1024;

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
    cancelNativePreview();
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
    if (!m_gallery) layout()->setContentsMargins(compact ? 12 : 16, 12, compact ? 12 : 16, 12);
}

void PreviewPane::setGallery(bool gallery)
{
    if (m_gallery == gallery) return;
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
    const quint64 generation = ++m_tagGeneration;
    m_tags.clear();
    m_tagReadError.clear();
    m_readingTags = !url.isEmpty() && !m_pendingTags.contains(url);
    updateTagWidgets();
    if (!m_readingTags) return;
    struct Result { QStringList tags; QString error; };
    auto result = std::make_shared<Result>();
    // Reads run off the UI thread; all writes are owned by the application's
    // serialized TagsJob. A generation rejects stale results after selection.
    auto *worker = QThread::create([url, result] { result->tags = FileTags::tags(url, &result->error); });
    connect(worker, &QThread::finished, this, [this, url, generation, result] {
        if (generation != m_tagGeneration || url != currentUrl()) return;
        m_readingTags = false;
        m_tags = result->tags;
        m_tagReadError = result->error;
        updateTagWidgets();
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
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
    m_pendingTags.insert(currentUrl());
    m_tagError.clear();
    updateTagWidgets();
    emit tagChangeRequested(currentUrl(), tag, operation);
}

void PreviewPane::tagChangeFinished(const QUrl &url, const QString &error)
{
    m_pendingTags.remove(url);
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
                m_images.insert(fingerprint + QStringLiteral("|native"), new QImage(image),
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
    if (url.isEmpty()) {
        cancelNativePreview();
        m_fingerprint.clear();
        m_mime.clear();
        m_tagError.clear();
        m_kindSizeText.clear();
        m_information.clear();
        m_moreInformation.clear();
        m_rotatableImage = false;
        stopMedia();
        m_pdf->close();
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
        stopMedia();
        m_pdf->close();
        m_fingerprint.clear();
        m_mime.clear();
        m_tagError.clear();
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
    m_fingerprint = fingerprint;
    m_mime.clear();
    m_tagError.clear();
    m_information.clear();
    m_moreInformation.clear();
    m_rotatableImage = false;
    if (m_tagEditor) m_tagEditor->clear();
    stopMedia();
    m_pdf->close();
    m_page = 0;
    m_image = QImage();
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
    const QMimeType mime = QMimeDatabase().mimeTypeForFile(info);
    m_mime = mime.name();
    const QString kind = mime.comment().isEmpty() ? mime.name() : mime.comment();
    m_kindSizeText = tr("%1 – %2").arg(kind, QLocale().formattedDataSize(info.size()));
    setInformation(tr("Kind"), kind, true);
    setInformation(tr("Size"), tr("%1 (%2 bytes)").arg(QLocale().formattedDataSize(info.size()),
        QLocale().toString(info.size())), true);
    setMetadata(tr("%1 · %2 · Modified %3")
        .arg(mime.comment(), QLocale().formattedDataSize(info.size()),
             QLocale().toString(info.lastModified(), QLocale::ShortFormat)));

    QImageReader reader(info.absoluteFilePath());
    reader.setAutoTransform(true);
    // Some image plugins claim arbitrary text/PDF inputs. Keep document and
    // text navigation in their proper native preview instead of accepting
    // those plugins' rasterized first page as an image.
    if ((m_mime.startsWith(QStringLiteral("image/")) || mime.isDefault()) && reader.canRead()) {
        const QSize original = reader.size();
        QSize oriented = original;
        if (reader.transformation().testFlag(QImageIOHandler::TransformationRotate90)) oriented.transpose();
        if (oriented.isValid())
            setInformation(tr("Dimensions"), tr("%1 × %2").arg(oriented.width()).arg(oriented.height()));
        QSizeF resolution = explicitImageResolution(info.absoluteFilePath());
        if (reader.transformation().testFlag(QImageIOHandler::TransformationRotate90)) resolution.transpose();
        if (!resolution.isEmpty())
            setInformation(tr("Resolution"), tr("%1 × %2 dpi")
                .arg(QLocale().toString(resolution.width(), 'f', 0), QLocale().toString(resolution.height(), 'f', 0)));
        if (QImage *cached = m_images.object(fingerprint)) {
            // Rebuild original geometry above before using the cached raster.
            m_rotatableImage = !reader.supportsAnimation();
            updateQuickActions();
            setInformation(tr("Color Space"), cached->colorSpace().description(), true);
            setMetadata(m_metadataText + tr(" · %1 × %2").arg(oriented.width()).arg(oriented.height()));
            showImage(*cached);
            return;
        }
        const qint64 pixels = qint64(original.width()) * original.height();
        if (!original.isValid() || pixels <= 0 ||
            (pixels > maxDecodedPixels && !reader.supportsOption(QImageIOHandler::ScaledSize))) {
            showFallback(tr("This image is too large to preview safely.\nOpen it with its default application."));
            return;
        }
        if (original.width() > renderLimit.width() || original.height() > renderLimit.height())
            reader.setScaledSize(original.scaled(renderLimit, Qt::KeepAspectRatio));
        QImage image = reader.read();
        if (!image.isNull()) {
            const int densityX = image.dotsPerMeterX(), densityY = image.dotsPerMeterY();
            if (image.width() > renderLimit.width() || image.height() > renderLimit.height())
                image = image.scaled(renderLimit, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            image.setDotsPerMeterX(densityX);
            image.setDotsPerMeterY(densityY);
            m_images.insert(fingerprint, new QImage(image), std::max<qsizetype>(1, image.sizeInBytes() / 1024));
            setInformation(tr("Color Space"), image.colorSpace().description(), true);
            m_rotatableImage = !reader.supportsAnimation();
            updateQuickActions();
            setMetadata(m_metadataText + tr(" · %1 × %2").arg(oriented.width()).arg(oriented.height()));
            showImage(image);
            return;
        }
        m_rotatableImage = false;
        updateQuickActions();
        showFallback(tr("The image could not be decoded.\nOpen it with its default application."));
        return;
    }
    if (mime.inherits(QStringLiteral("application/pdf"))) {
        const auto error = m_pdf->load(info.absoluteFilePath());
        if (error == QPdfDocument::Error::None && m_pdf->pageCount() > 0) {
            setInformation(tr("Pages"), QLocale().toString(m_pdf->pageCount()));
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
    if (const QImage *cached = m_images.object(fingerprint + QStringLiteral("|native"))) showImage(*cached);
    else requestNativePreview(true);
}

void PreviewPane::showImage(const QImage &image)
{
    m_image = image;
    m_picture->clear();
    m_content->setCurrentWidget(m_picture);
    updateThumbnail(image);
    updateImageSize();
    QTimer::singleShot(0, this, &PreviewPane::updateImageSize);
}

void PreviewPane::updateImageSize()
{
    if (m_image.isNull()) return;
    const QSize size = m_picture->contentsRect().size();
    if (size.width() < 1 || size.height() < 1) return;
    const qreal dpr = devicePixelRatioF();
    const QSize target = m_image.size().scaled(size * dpr, Qt::KeepAspectRatio);
    if (m_picture->pixmap().size() == target && qFuzzyCompare(m_picture->pixmap().devicePixelRatio(), dpr)) return;
    QPixmap pixmap = QPixmap::fromImage(m_image.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    pixmap.setDevicePixelRatio(dpr);
    m_picture->setPixmap(pixmap);
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
    const QSizeF points = m_pdf->pagePointSize(m_page);
    setInformation(tr("Page Size"), tr("%1 × %2 pt")
        .arg(QLocale().toString(points.width(), 'f', 1), QLocale().toString(points.height(), 'f', 1)), true);
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

void PreviewPane::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange) updateGalleryMetadata();
}

void PreviewPane::hideEvent(QHideEvent *event)
{
    if (m_player) m_player->stop();
    QWidget::hideEvent(event);
}

void PreviewPane::keyPressEvent(QKeyEvent *event)
{
    if (!m_gallery && (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Space)) {
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
