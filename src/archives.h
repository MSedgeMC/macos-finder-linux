// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <memory>

class QWidget;
struct ArchivesState;

namespace FinderArchives {
struct Capability {
    bool enabled = false;
    QString explanation;
};
struct Limits {
    int entries = 10000; // Includes implicit directories.
    qint64 archiveBytes = 512LL * 1024 * 1024;
    qint64 expandedBytes = 1024LL * 1024 * 1024;
    int depth = 64;
    int processingMilliseconds = 120000;
};
}

// Host integration: one controller per window; connect output(s)Created to
// refresh/select results, status/error/progress/busy to the host UI, and call
// extract(url) before external opening when canExtract(url).enabled is true.
// No menu, browser, window or application-launch policy is owned here.
// Link Qt6::Widgets, KF6::Archive, KF6::KIOCore/Gui/Widgets, Threads::Threads.
// KArchive is the public LGPL library; this module contains no copied renderer
// or archive library code. Stored/deflated, unencrypted, single-volume ZIPs
// (including ZIP64 sizes/offsets within the budgets) are supported.
// One task per controller; a process-wide two-thread pool/eight-task queue,
// 64 KiB streaming,
// bounded metadata and deadlines. Limits may only reduce the hard defaults.
// Sources and default output directories must be local, without symlink
// components. Publication uses native KIO copy, conflict UI, and one Undo job.
// Safe names use native auto-rename; a racing conflict remains KIO's decision.
// POSIX permissions and modification times are retained where ZIP/KIO support
// them. Links, devices, ownership changes, ACLs/xattrs and macOS metadata are
// not imported. No source content is modified. Nothing is published until all
// staging succeeds; cancellation during publication retains reported partial
// results and native Undo. Destruction cancels without waiting on a worker.
class Archives : public QObject {
    Q_OBJECT
public:
    explicit Archives(QWidget *dialogParent = nullptr, const FinderArchives::Limits &limits = {});
    ~Archives() override;
    static FinderArchives::Capability canCompress(const QList<QUrl> &sources, const QUrl &directory = {});
    static FinderArchives::Capability canExtract(const QUrl &archive);
    static QString suggestedArchiveName(const QList<QUrl> &sources);
    bool busy() const;
public slots:
    // Empty directory captures the first source's parent. Multiple selection
    // must share that parent; the caller's navigation cannot retarget work.
    void compress(const QList<QUrl> &sources, const QUrl &directory = {});
    // Expands one top-level item directly beside the ZIP. Multiple top-level
    // items use a folder named after the ZIP, retaining internal hierarchy.
    void extract(const QUrl &archive);
    void cancel();
signals:
    void busyChanged(bool busy);
    void statusChanged(const QString &message);
    // Staging bytes; total is the validated uncompressed input size.
    void progressChanged(qint64 completed, qint64 total);
    void publicationProgressChanged(int percent);
    void errorOccurred(const QString &message);
    void outputCreated(const QUrl &url); // Actual native target, after rename.
    void outputsCreated(const QList<QUrl> &urls);
    void finished(bool cancelled); // Exactly once per accepted operation.
private:
    void start(const QList<QUrl> &sources, const QUrl &directory, bool extraction);
    void publish();
    void preserveNextDirectory();
    void complete(bool cancelled, const QString &error = {});
    QPointer<QWidget> m_parent;
    std::shared_ptr<ArchivesState> m_state;
};
