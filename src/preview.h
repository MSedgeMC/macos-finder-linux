// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#pragma once

#include <QCache>
#include <QImage>
#include <QList>
#include <QUrl>
#include <QWidget>

class QLabel;
class QPushButton;
class QTextEdit;
class QStackedWidget;
class QPdfDocument;
class QMediaPlayer;
class QAudioOutput;
class QVideoWidget;
class QSlider;
class QHBoxLayout;
class QVBoxLayout;

// Read-only preview shared by Gallery and the separate Quick Look window.
// The caller owns application activation and the native window/session policy.
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

signals:
    void openRequested(const QUrl &url);
    void closeRequested();

protected:
    void resizeEvent(QResizeEvent *event) override;
    void hideEvent(QHideEvent *event) override;
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

    QList<QUrl> m_urls;
    int m_index = -1;
    int m_page = 0;
    QString m_fingerprint;
    QString m_mime;
    QString m_mediaPath;
    QString m_metadataText;
    bool m_gallery = false;
    QVBoxLayout *m_layout;
    QHBoxLayout *m_header;
    QWidget *m_headerWidget;
    QWidget *m_previewArea;
    QWidget *m_galleryArea = nullptr;
    QWidget *m_gallerySidebar = nullptr;
    QImage m_image;
    QCache<QString, QImage> m_images{32768}; // 32 MiB, charged in KiB.
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
    QPdfDocument *m_pdf;
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audio = nullptr;
    QVideoWidget *m_video = nullptr;
};
