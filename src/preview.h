// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#pragma once

#include <QCache>
#include <QImage>
#include <QList>
#include <QPair>
#include <QPointer>
#include <QPolygonF>
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
class QPrinter;
class QMenu;
class QDialog;
class PreviewPdfView;
namespace KIO { class PreviewJob; }
struct PreviewRaster;
struct PreviewWorkQueue;
struct PreviewPrintState;
struct PreviewTrimState;

// Preview shared by Gallery and the separate Quick Look window. The caller
// owns mutations and serializes tag writes. Built-in native launch/print/share
// actions never alter the source; openRequested retains host launch ownership.
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
    // Close hides the pane; a host-managed reopen must show() both pane/window.
    // toggleQuickLook returns true when opened, false when toggled closed.
    // primary is the actual last-selected URL, including search selections.
    bool toggleQuickLook(const QList<QUrl> &urls, const QUrl &primary = QUrl());
    void updateQuickLook(const QList<QUrl> &urls, const QUrl &primary = QUrl());
    QList<QUrl> urls() const;
    // Raster zoom is relative to fit-to-window (.25–16); decoding stays bounded.
    void setZoomFactor(qreal factor);
    qreal zoomFactor() const;
    void resetZoom();
    // Image dimensions at one original pixel per logical screen pixel. Uses
    // the bounded raster; large enlargement may be less detailed than source.
    void actualSize();
    qreal imageScale() const;
    void setTextWrapping(bool enabled);
    bool textWrapping() const;
    void reloadPreviewPreferences();
    void setFullScreen(bool fullScreen);
    // Fullscreen transitions use the pane's top-level window, restore its
    // prior state and are disabled for compact/Gallery embedding.
    bool isFullScreen() const;
    void setContactSheet(bool enabled);
    bool contactSheet() const;
    // Starts a looping selection slideshow; interval is bounded to .5–60s.
    // Stops on close/hide or contact sheet, and never auto-starts media.
    void setSlideshow(bool running, int intervalMs = 4000);
    bool slideshowRunning() const;
    // Existing openRequested remains host-owned. With no connected host, Open
    // uses a native KIO launcher; Open With always uses the native chooser.
    void openCurrent();
    void openWithCurrent();
    // Copy URLs and native email attachments are built in. A host with an
    // additional real sharing backend can enable Share… with this capability.
    void setShareProviderAvailable(bool available);
    void shareCurrent();
    // PrintSupport is optional: compile with FINDER_PREVIEW_PRINT_SUPPORT=1
    // and link Qt6::PrintSupport. Otherwise Print remains visibly disabled.
    bool canPrintCurrent() const;
    void printCurrent();
    // Supplied printer owns settings/output; no dialog (useful for host print
    // workflows and isolated PDF-output tests). Original image resolution is
    // decoded separately; PDFs print every selected page, not preview pixels.
    void printToPrinter(const std::shared_ptr<QPrinter> &printer);
    // Call for every tagChangeRequested completion, including rejected/busy
    // requests. Errors are displayed without optimistically changing tag chips.
    void tagChangeFinished(const QUrl &url, const QString &error = QString());
    // A separately decorated native Quick Look host owns close/minimize/zoom.
    // This only suppresses the embedded close button, never changes window flags.
    void setHostWindowControls(bool enabled);
    // One-row Quick Look presentation. Enable before adding the returned
    // header to the host's 40px chrome layout beside its window controls.
    // The existing filename/Open/actions stay live; no second title is needed.
    // Hosted mode suppresses embedded close, bottom view toolbar and metadata
    // by default. Actions retains view commands and a Show Information toggle;
    // PDF search/navigation and media playback remain inline and functional.
    // The host owns its background, window controls, size and window flags.
    void setHostedQuickLook(bool hosted);
    bool hostedQuickLook() const;
    // Reparent this widget, never copy its contents or delete it independently.
    // Keep pane and header together for the host lifetime. On disabling hosted
    // mode the pane reclaims the header; pane destruction deletes it if moved.
    // Compact/Gallery temporarily suppress hosted chrome without losing it.
    QWidget *quickLookHeaderWidget() const;
    // Zero-based PDF navigation. Search/selection are asynchronous and bounded;
    // image-only PDFs require an external OCR provider for searchable text.
    int pdfPageCount() const;
    int pdfCurrentPage() const;
    void setPdfPage(int page);
    void searchPdf(const QString &text);
    void advancePdfSearch(int delta);
    void selectPdfText(int page, const QPointF &start, const QPointF &end);
    QString selectedPdfText() const;
    // Installed ffmpeg/ffprobe only. Export a new same-container copy through
    // native KIO conflict/Undo handling; the original is never replaced.
    bool canTrimCurrent() const;
    bool trimBusy() const;
    void trimCurrent();
    void exportTrimCopy(qint64 startMs, qint64 endMs, const QUrl &destination);
    void cancelTrim();

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
    // Host may populate a real provider dialog; enabled only by capability.
    // Contact sheet shares the complete selection, otherwise the current item.
    void shareRequested(const QList<QUrl> &urls);
    void actionError(const QUrl &url, const QString &message);
    // Empty error means completed; cancellation/errors retain source identity.
    void printFinished(const QUrl &url, const QString &error);
    // Host supplies genuine file actions for the current preview selection.
    void quickActionsMenuRequested(QMenu *menu, const QList<QUrl> &urls);
    void printReadinessChanged(bool ready);
    void pdfPageChanged(int page, int pageCount);
    void pdfSearchFinished(const QString &query, int matches, bool limited);
    void pdfSelectionChanged(const QString &text);
    void trimProgressChanged(int percent);
    // destination is the actual native-conflict result; may be empty on error.
    void trimFinished(const QUrl &source, const QUrl &destination, const QString &error);

protected:
    void paintEvent(QPaintEvent *event) override;
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
    void updatePresentation();
    void populateContactSheet();
    void loadContactThumbnails();
    QList<QUrl> actionUrls() const;
    void updateNativeActions();
    void requestPrintPage();
    void finishPrint(const QString &error);
    void resetPdf();
    void requestPdfPages();
    void requestPdfSearchBatch();
    void updatePdfPageControls();
    void finishTrim(const QString &error);
    void probeTrimSource();

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
    bool m_showPreviewInformation = true, m_showPreviewDates = true;
    bool m_showPreviewDimensions = true, m_showPreviewLocation = true, m_showPreviewActions = true;
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
    bool m_actualSize = false;
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
    QToolButton *m_nativeActions;
    QMenu *m_shareMenu;
    QAction *m_openWithAction;
    QAction *m_printAction;
    QAction *m_shareAction;
    QAction *m_emailAction;
    QAction *m_copyUrlsAction;
    bool m_shareProviderAvailable = false;
    std::shared_ptr<PreviewWorkQueue> m_printWork;
    std::shared_ptr<PreviewPrintState> m_printState;
    bool m_printReady = false;
    bool m_textReady = false;
    bool m_hostWindowControls = false;
    bool m_hostedQuickLook = false;
    QAction *m_zoomOutAction;
    QAction *m_zoomInAction;
    QAction *m_zoomResetAction;
    QAction *m_actualSizeAction;
    QAction *m_textWrapAction;
    QAction *m_contactAction;
    QAction *m_slideshowAction;
    QAction *m_fullScreenAction;
    QAction *m_informationAction;
    QAction *m_getInfoAction;
    PreviewPdfView *m_pdfView;
    QLineEdit *m_pdfSearch;
    QLineEdit *m_pdfPageEntry;
    QLabel *m_pdfSearchStatus;
    QPushButton *m_pdfSearchPrevious;
    QPushButton *m_pdfSearchNext;
    QTimer *m_pdfRenderTimer;
    std::shared_ptr<PreviewWorkQueue> m_pdfSearchWork;
    std::shared_ptr<PreviewWorkQueue> m_pdfSelectionWork;
    quint64 m_pdfSearchGeneration = 0;
    quint64 m_pdfSelectionGeneration = 0;
    QString m_pdfQuery;
    QString m_pdfSelectedText;
    struct PdfMatch { int page; QList<QPolygonF> bounds; };
    QList<PdfMatch> m_pdfMatches;
    int m_pdfSearchPage = 0;
    int m_pdfMatch = -1;
    qsizetype m_pdfSearchCharacters = 0;
    QAction *m_trimAction;
    QPushButton *m_trimButton;
    std::shared_ptr<PreviewTrimState> m_trimState;
    QPointer<QDialog> m_trimDialog;
    bool m_gallery = false;
    QVBoxLayout *m_layout;
    QHBoxLayout *m_header;
    QPointer<QWidget> m_headerWidget;
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
