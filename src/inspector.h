// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#pragma once

#include <QDateTime>
#include <QDialog>
#include <QList>
#include <QMap>
#include <QStringList>
#include <QUrl>
#include <memory>

// Portable, read-only selection information. The host owns native properties
// dialogs and reveal operations; this class never changes permissions or tags.
// Call on the GUI thread. setUrls captures the host selection; when pinned it
// remembers the latest selection without replacing the inspected URLs.
class Inspector : public QDialog {
    Q_OBJECT
    Q_PROPERTY(bool followsSelection READ followsSelection WRITE setFollowsSelection NOTIFY followsSelectionChanged)
    Q_PROPERTY(bool loading READ isLoading NOTIFY loadingChanged)
public:
    struct Summary {
        qsizetype selectedCount = 0;
        qsizetype inspectedCount = 0;
        qsizetype availableCount = 0;
        qsizetype fileCount = 0;
        qsizetype folderCount = 0;
        qsizetype linkCount = 0;
        qsizetype otherCount = 0;
        qsizetype remoteCount = 0;
        qsizetype unavailableCount = 0;
        qsizetype unreadableCount = 0; // Contents inaccessible; stat may still succeed.
        quint64 fileBytes = 0; // Logical bytes of selected regular files only.
        bool sizeOverflow = false;
        qsizetype modifiedCount = 0;
        qsizetype createdCount = 0;
        QDateTime earliestModified, latestModified;
        QDateTime earliestCreated, latestCreated; // No ctime-as-creation fallback.
        QMap<QString, qsizetype> kinds; // Regular-file MIME kinds inferred by name.
        QString location; // URL parent, without credentials; not canonicalized.
        bool multipleLocations = false;
        QStringList commonTags; // Intersection of successfully read native tag sets.
        qsizetype tagsReadCount = 0;
        qsizetype tagsUnavailableCount = 0;
        bool tagSetsDiffer = false;
        bool cancelled = false;
        bool timeLimited = false;
    };

    explicit Inspector(QWidget *parent = nullptr);
    ~Inspector() override;
    void setUrls(QList<QUrl> urls);
    QList<QUrl> urls() const;
    bool followsSelection() const;
    void setFollowsSelection(bool follow);
    bool isLoading() const;
    Summary summary() const;
    // At most this many entries receive metadata per refresh. Every captured
    // URL remains in the virtualized Items list with its own native-info link.
    static constexpr qsizetype maximumMetadataItems = 4096;

public slots:
    void refresh();
    void cancelMetadata();

signals:
    void propertiesRequested(QUrl url);
    void revealRequested(QList<QUrl> urls);
    void followsSelectionChanged(bool follow);
    void loadingChanged(bool loading);
    void metadataChanged();

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    class Private;
    std::unique_ptr<Private> d;
};
