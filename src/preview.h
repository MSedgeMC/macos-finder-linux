// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#pragma once

#include <QCache>
#include <QImage>
#include <QList>
#include <QPair>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <QWidget>
#include <memory>

class QLabel;
class QShowEvent;
class QPushButton;
class QTextEdit;
class QStackedWidget;
class QMediaPlayer;
class QAudioOutput;
class QVideoWidget;
class QSlider;
class QHBoxLayout;
class QVBoxLayout;
class QLineEdit;
class QToolButton;
class QAction;
class QListWidget;
class QTimer;
class QFileSystemWatcher;
namespace KIO { class PreviewJob; }
struct PreviewRaster;
struct PreviewWorkQueue;

// Preview shared by Gallery and the separate Quick Look window. The caller
// owns native actions and serializes tag mutations with other application writes.
class PreviewPane : public QWidget {
    Q_OBJECT
public:
    explicit PreviewPane(QWidget *parent = nullptr);
    ~PreviewPane() override;
    void setUrls(const QList<QUrl> &urls);
    QUrl currentUrl() const;
    void advance(int delta);
    void setCompact(bool compact);
    void setGallery(bool gallery);
    void refreshTags();
    // Force re-read after a host file/metadata notification, even if size and
    // mtime are unchanged. Running work and stale deliveries are cancelled.
    void refreshCurrent();
    // Geometry of the auto-oriented original, never the bounded display raster.
    // rotateCopyRequested continues to identify the original source file.
    QSize originalImageSize() const;
    // Reusable Quick Look host: retain its window/pane, connect closeRequested
    // to hide (not delete), and forward active selection to updateQuickLook.
    // toggleQuickLook returns true when opened, false when toggled closed.
    // primary is the actual last-selected URL, including search selections.
    bool toggleQuickLook(const QList<QUrl> &urls, const QUrl &primary = QUrl());
    void updateQuickLook(const QList<QUrl> &urls, const QUrl &primary = QUrl());
    QList<QUrl> urls() const;
    // Raster zoom is relative to fit-to-window (.25–16); decoding stays bounded.
    void setZoomFactor(qreal factor);
    qreal zoomFactor() const;
    void resetZoom();
    void setFullScreen(bool fullScreen);
    bool isFullScreen() const;
    void setContactSheet(bool enabled);
    bool contactSheet() const;
    // Starts a looping selection slideshow; interval is bounded to .5–60s.
    // Stops on close/hide or contact sheet, and never auto-starts media.
    void setSlideshow(bool running, int intervalMs = 4000);
    bool slideshowRunning() const;
    // Call for every tagChangeRequested completion, including rejected/busy
    // requests. Errors are displayed without optimistically changing tag chips.
    void tagChangeFinished(const QUrl &url, const QString &error = QString());

signals:
    void openRequested(const QUrl &url);
    void closeRequested();
    void getInfoRequested(const QUrl &url);
    // Export a rotated original as a new copy through native conflict/Undo
    // handling. Do not export the bounded/scaled preview or overwrite silently.
    void rotateCopyRequested(const QUrl &url);
    // operation matches the application's TagsJob: 1 = add, 2 = remove.
    void tagChangeRequested(const QUrl &url, const QString &tag, int operation);
    // Navigation only; hosts may reflect the primary item in their selection.
    // updateQuickLook already updates the pane, so avoid echo update loops.
    void currentUrlChanged(const QUrl &url);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    void showCurrent();
    void showImage(const QImage &image);
    void updateImageSize();
    void renderPage(int page);
    void stopMedia();
    void toggleMedia();
    void updatePlaybackTime();
    void showFallback(const QString &message = QString());
    void setMetadata(const QString &text);
    void updateGalleryWidth();
    void createGallery();
    void updateGalleryMetadata();
    void setInformation(const QString &key, const QString &value, bool detail = false);
    void updateThumbnail(const QImage &image = QImage());
    void updateTagWidgets();
    void requestTagChange(const QString &tag, int operation);
    void updateQuickActions();
    void requestNativePreview(bool contentPreview);
    void cancelNativePreview();
    void requestRaster(bool pdf, int page = 0);
    void applyRaster(const PreviewRaster &raster, bool pdf);
    void updateSourceWatch();
    void requestClose();
    void updatePreviewControls();
    void populateContactSheet();
    void loadContactThumbnails();

    QList<QUrl> m_urls;
    int m_index = -1;
    int m_page = 0;
    QString m_fingerprint;
    QString m_mime;
    QString m_mediaPath;
    QString m_metadataText;
    QString m_kindSizeText;
    QList<QPair<QString, QString>> m_information;
    QList<QPair<QString, QString>> m_moreInformation;
    QStringList m_tags;
    QString m_tagError;
    QString m_tagReadError;
    QHash<QUrl, QString> m_tagErrors;
    QSet<QUrl> m_pendingTags;
    quint64 m_tagGeneration = 0;
    bool m_readingTags = false;
    bool m_moreDetails = false;
    bool m_rotatableImage = false;
    quint64 m_previewGeneration = 0;
    int m_pageCount = 0;
    QSize m_originalImageSize;
    std::shared_ptr<PreviewWorkQueue> m_renderWork;
    std::shared_ptr<PreviewWorkQueue> m_tagWork;
    std::shared_ptr<PreviewWorkQueue> m_contactWork;
    bool m_compact = false;
    bool m_contactSheet = false;
    qreal m_zoom = 1;
    Qt::WindowStates m_beforeFullScreen;
    quint64 m_contactGeneration = 0;
    QListWidget *m_contactList;
    QWidget *m_viewControls;
    QPushButton *m_zoomOut;
    QPushButton *m_zoomIn;
    QPushButton *m_zoomReset;
    QPushButton *m_fullScreen;
    QPushButton *m_contactToggle;
    QPushButton *m_slideshow;
    QTimer *m_slideshowTimer;
    QTimer *m_contactTimer;
    QFileSystemWatcher *m_sourceWatcher;
    QTimer *m_refreshTimer;
    bool m_gallery = false;
    QVBoxLayout *m_layout;
    QHBoxLayout *m_header;
    QWidget *m_headerWidget;
    QWidget *m_previewArea;
    QWidget *m_galleryArea = nullptr;
    QWidget *m_gallerySidebar = nullptr;
    QWidget *m_tagChips = nullptr;
    QWidget *m_tagColors = nullptr;
    QLabel *m_thumbnail = nullptr;
    QLabel *m_kindSize = nullptr;
    QLabel *m_tagStatus = nullptr;
    QLineEdit *m_tagEditor = nullptr;
    QPushButton *m_showMore = nullptr;
    QPushButton *m_getInfo = nullptr;
    QToolButton *m_moreActions = nullptr;
    QAction *m_copyLocation = nullptr;
    QAction *m_rotateCopy = nullptr;
    QList<QToolButton *> m_colorButtons;
    QPointer<KIO::PreviewJob> m_nativePreview;
    QImage m_thumbnailImage;
    QImage m_image;
    QCache<QString, PreviewRaster> m_images{32768}; // 32 MiB, charged in KiB.
    QLabel *m_name;
    QLabel *m_count;
    QLabel *m_metadata;
    QLabel *m_picture;
    QLabel *m_fallback;
    QLabel *m_pageLabel;
    QLabel *m_playbackTime;
    QTextEdit *m_text;
    QPushButton *m_previous;
    QPushButton *m_next;
    QPushButton *m_close;
    QPushButton *m_open;
    QPushButton *m_pagePrevious;
    QPushButton *m_pageNext;
    QPushButton *m_play;
    QSlider *m_seek;
    QStackedWidget *m_content;
    QWidget *m_pageControls;
    QWidget *m_mediaControls;
    QWidget *m_videoPage;
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audio = nullptr;
    QVideoWidget *m_video = nullptr;
};
