// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#pragma once

#include <QObject>
#include <QList>
#include <QUrl>
#include <QJsonObject>
#include <memory>

// Read-only bounded local search. Start/cancel and all public signals run
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
        qint64 maxTotalContentBytes = 32 * 1024 * 1024;
        // Portable saved-query schema (independent of Apple's .savedSearch):
        // group: {"match":"all"|"any"|"none", "criteria":[nodes...]}
        // leaf: {"field":"name"|"kind"|"size"|"modified"|"created"|"tag"|"content",
        //        "op":"contains"|"equals"|"beginsWith"|"endsWith"|
        //             "atLeast"|"atMost"|"greaterThan"|"lessThan", "value":...}
        // Name/content use text operators; kind/tag use equals. Size/date use
        // numeric operators, bytes/Unix seconds as decimal strings or exact
        // JSON integers <= 2^53-1. Dates are compared at second precision.
        // Existing option predicates and the query are ANDed with this tree.
        // Unknown metadata/content stays unknown under negation (no false hits).
        QJsonObject criteria;
        bool parseKeywords = false; // opt-in; old start() keeps literal name matching
        // Indexed searches use only an existing local Baloo 6 index, never
        // activate an indexer. Coverage is index-only; see providerStatus.
        enum class Provider { Local, PreferIndex, IndexOnly };
        Provider provider = Provider::Local;
        // Scan this descendant first, within the same total search budgets.
        QString preferredPath;
        bool hasCriteria() const {
            return !requiredTag.isEmpty() || !kind.isEmpty() || !contentText.trimmed().isEmpty()
                || minimumSize >= 0 || maximumSize >= 0 || modifiedAfter >= 0 || modifiedBefore >= 0
                || !criteria.isEmpty();
        }
    };

    explicit LocalFileSearch(QObject *parent = nullptr);
    ~LocalFileSearch() override;
    quint64 start(const QUrl &root, const QString &filename);
    quint64 start(const QUrl &root, const QString &filename, const Options &options);
    quint64 generation() const;
    bool isRunning() const;
    // Validation: at most 128 nodes, depth 12, 4096 characters per text value.
    static bool validateCriteria(const QJsonObject &criteria, QString *error = nullptr);
    // Bounded AND/OR/NOT + parentheses, quoted text, implicit AND, metadata
    // keywords above (also filename/date), size units B/KB/MB/GB/KiB/MiB/GiB,
    // ISO dates/datetimes or Unix seconds. Empty result + error means invalid.
    static QJsonObject parseQuery(const QString &query, QString *error = nullptr);

public slots:
    void cancel();

signals:
    void started(quint64 generation, const QUrl &root, const QString &filename);
    void filesFound(quint64 generation, const QList<QUrl> &files);
    void finished(quint64 generation, int resultCount, bool limited, int skippedDirectories);
    void canceled(quint64 generation);
    void error(quint64 generation, const QString &message);
    // provider, coverage, detail, indexAvailable, examinedEntries,
    // contentBytesRead, incompleteContentFiles, unavailableMetadata, limited.
    // Emitted before results and again on completion; same generation rules.
    void providerStatus(quint64 generation, const QJsonObject &status);

private:
    class Private;
    std::unique_ptr<Private> d;
};
