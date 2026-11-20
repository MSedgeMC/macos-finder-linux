// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#pragma once
#include <QMainWindow>
#include <QList>
#include <QUrl>
#include <QTimer>
#include <QPointer>
#include <QSet>
namespace KIO { class PreviewJob; }
class PreviewPane;
class QListWidget;
class QMenu;
class QStatusBar;
class QuickActions;
class ViewerWindow final : public QMainWindow {
public:
    explicit ViewerWindow(const QList<QUrl> &urls = {}, QWidget *parent = nullptr);
    ~ViewerWindow() override;
    void setUrls(const QList<QUrl> &urls);
    QList<QUrl> urls() const;
    PreviewPane *preview() const;
protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
private:
    void openFiles();
    void settingsDialog();
    void reloadSettings();
    void updateActions();
    void populateRecent();
    void loadThumbnails();
    void rebuildSidebar();
    void updateSidebarVisibility();
    PreviewPane *m_preview;
    QListWidget *m_sidebar;
    QMenu *m_recent;
    QMenu *m_tools;
    QuickActions *m_edits;
    QStatusBar *m_status;
    QTimer m_thumbnailTimer;
    QPointer<KIO::PreviewJob> m_thumbnailJob;
    QSet<QUrl> m_thumbnailLoaded;
    quint64 m_thumbnailGeneration = 0;
    QUrl m_sidebarPdf;
    QUrl m_capabilitySource;
    QList<QUrl> m_capabilitySelection;
    bool m_canExportImage = false, m_canCreatePdf = false;
    bool m_pageThumbnailsPending = false;
    bool m_sidebarOverride = false;
    bool m_sidebarEnabled = true;
};
