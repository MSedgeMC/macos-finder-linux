// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#pragma once

#include <QObject>
#include <QList>
#include <QUrl>
#include <memory>

// Read-only recursive filename search. Start/cancel and all public signals run
// on this object's thread. KIO models may consume the returned local URLs.
// An older generation never delivers results, errors, or finished after start.
class LocalFileSearch : public QObject {
    Q_OBJECT
public:
    struct Options {
        bool showHidden = false;
        int maxResults = 2000;
        int maxEntries = 250000;
        int maxElapsedMs = 15000;
        QString requiredTag;
        // Bounded local metadata predicates; empty/negative means unrestricted.
        QString kind; // file, folder, image, pdf, text, audio, video, archive
        qint64 minimumSize = -1;
        qint64 maximumSize = -1;
        qint64 modifiedAfter = -1; // Unix seconds
        qint64 modifiedBefore = -1;
        QString contentText;
        int maxContentBytes = 512 * 1024;
        // Scan this descendant first, within the same total search budgets.
        QString preferredPath;
    };

    explicit LocalFileSearch(QObject *parent = nullptr);
    ~LocalFileSearch() override;
    quint64 start(const QUrl &root, const QString &filename);
    quint64 start(const QUrl &root, const QString &filename, const Options &options);
    quint64 generation() const;
    bool isRunning() const;

public slots:
    void cancel();

signals:
    void started(quint64 generation, const QUrl &root, const QString &filename);
    void filesFound(quint64 generation, const QList<QUrl> &files);
    void finished(quint64 generation, int resultCount, bool limited, int skippedDirectories);
    void canceled(quint64 generation);
    void error(quint64 generation, const QString &message);

private:
    class Private;
    std::unique_ptr<Private> d;
};
