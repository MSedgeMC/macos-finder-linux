// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QUrl>
#include <functional>
#include <memory>

// Read-only Linux Smart Folder descriptor reader. Requires only Qt6::Core,
// C++17, POSIX file APIs and /proc/self/fd; no moc, Qt Concurrent, KIO or GUI
// dependencies. Uses public APIs available in Qt 6.8.
// All public methods and completions belong to this object's thread.
class SmartFolderReader final : public QObject {
public:
    using Completion = std::function<void(const QJsonObject &definition,
                                          const QUrl &root, const QString &error,
                                          quint64 generation)>;

    explicit SmartFolderReader(QObject *parent = nullptr);
    ~SmartFolderReader() override;

    // Pure URL check, with no filesystem access. Once recognized, callers
    // should consume the open action even if asynchronous validation fails.
    static bool isSmartFolderUrl(const QUrl &url);

    // Always asynchronous, including errors. Only the newest request can
    // complete; cancel/destruction silently suppress pending completions.
    // Success has a validated definition/root and empty error; errors have
    // empty definition/root. Optional criteria use the current format defaults.
    quint64 read(const QUrl &descriptor, Completion completion);
    void cancel();
    quint64 generation() const;
    bool isRunning() const;

    // The owner must capture/check its QPointer<Browser>, tab identity and
    // navigation policy in Completion, and cancel on target/context changes.
    // Validation does not navigate or modify search controls. Local root
    // existence/directory checks run in the worker. Remote folder URLs receive
    // syntax validation only (file, smb, sftp, ftp, fish, webdav, webdavs, trash,
    // remote, mtp); availability/protocol handling stays with the native caller.
    // Across all readers, at most two pool workers run. Each reader retains
    // only its active request and newest pending request. Uninterruptible
    // filesystem calls may occupy a worker, but never block cancel/destruction.
private:
    class Private;
    std::unique_ptr<Private> d;
};
