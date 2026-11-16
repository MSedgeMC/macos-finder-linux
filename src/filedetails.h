// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#pragma once

#include <QAbstractItemModel>
#include <QDialog>
#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>
#include <atomic>
#include <memory>
#include <optional>

// Linux inode metadata, not macOS Finder flags or a database. Schema v1:
// user.finder.hide_extension: one ASCII byte '1' (Hide) or '0' (Show);
// absent means Inherit. user.xdg.comment: valid UTF-8, no NUL, <=16384 bytes;
// an empty comment removes the attribute. This is KDE UserMetaData's comment
// attribute. Malformed/oversized existing values are reported, never truncated.
// Only these two attributes are changed. No file contents, tags or dates change
// intentionally (the filesystem can update inode ctime when xattrs change).
namespace FileDetails {
inline constexpr char hideExtensionAttribute[] = "user.finder.hide_extension";
inline constexpr char commentAttribute[] = "user.xdg.comment";
inline constexpr qsizetype maximumCommentBytes = 16384;
inline constexpr qsizetype maximumMetadataItems = 4096;
inline constexpr int maximumTimeMs = 5000;

enum class ExtensionOverride { Inherit = -1, Show = 0, Hide = 1 };
enum class ErrorCode {
    None, InvalidInput, NonLocal, UnsafeFile, Missing, Unsupported,
    PermissionDenied, TooLarge, InvalidMetadata, Changed, StorageError,
    Cancelled, Busy
};
struct Error {
    ErrorCode code = ErrorCode::None;
    QString message;
    int nativeCode = 0;
    bool ok() const { return code == ErrorCode::None; }
};
struct Identity {
    quint64 device = 0, inode = 0;
    bool valid = false;
    bool operator==(const Identity &) const = default;
};
struct Details {
    QUrl url;
    Identity identity;
    bool directory = false;
    ExtensionOverride extensionOverride = ExtensionOverride::Inherit;
    QString comment;
    Error error; // Opening/pinning the inode failed; neither field was read.
    Error extensionError, commentError; // Independent field errors.
    bool available() const { return identity.valid && error.ok(); }
};
struct Limits {
    qsizetype maxItems = maximumMetadataItems;
    int maxTimeMs = maximumTimeMs;
};
using Cancellation = std::shared_ptr<std::atomic_bool>;
struct Batch {
    QList<Details> items;
    qsizetype requestedCount = 0;
    bool cancelled = false, limited = false;
};

// Synchronous worker APIs. Walk every ancestor with O_NOFOLLOW, reject '..',
// symlinks and special files, pin and verify a regular file/directory descriptor.
// No contents are read. Conservative O_RDONLY reopening can deny metadata on a
// file whose contents are unreadable. Kernel/network calls cannot be forcefully
// interrupted; limits/cancellation are cooperative between filesystem calls.
Details read(const QUrl &url, const Cancellation &cancelled = {});
Batch readBatch(const QList<QUrl> &urls, Limits limits = {}, const Cancellation &cancelled = {});
// Optional expected identity prevents overwriting a replacement at this path.
Error setExtensionOverride(const QUrl &url, ExtensionOverride value, Identity expected = {});
Error setComment(const QUrl &url, const QString &comment, Identity expected = {});

// Pure string function, safe in model/delegate data(). Pass a previously known
// MIME suffix without its dot; no filesystem or MIME lookup happens here.
// Inherit preserves existing known-suffix hiding. Hide also hides unknown last
// suffixes. Show, directories, and showAllExtensions keep the complete name.
QString displayName(const QString &name, bool directory, const QString &knownSuffix,
                    ExtensionOverride value, bool showAllExtensions);
}

// GUI-thread facade. Across all instances at most two workers run; per instance
// one latest read and one write can be queued. refresh rejects old reads;
// setUrls also rejects write readback from the previous selection.
// cached() and model()->data() NEVER perform filesystem calls or schedule work.
// The host queues setUrls as native listings change and refreshes on external
// metadata changes. There is no polling timer, persistent cache or disk scan.
class FileDetailsCache : public QObject {
    Q_OBJECT
public:
    enum Role {
        UrlRole = Qt::UserRole + 1, ExtensionOverrideRole, CommentRole,
        PendingRole, ErrorRole, DeviceRole, InodeRole
    };
    explicit FileDetailsCache(QObject *parent = nullptr);
    ~FileDetailsCache() override;
    void setUrls(QList<QUrl> urls, FileDetails::Limits limits = {});
    QList<QUrl> urls() const; // Bounded to maximumMetadataItems.
    std::optional<FileDetails::Details> cached(const QUrl &url) const;
    QAbstractItemModel *model() const;
    bool isLoading() const;
    bool isWriting() const;
    FileDetails::Batch lastBatch() const;
    // Async writes REQUIRE a cached inode identity; return 0 on rejection and
    // queue writeFinished(0, url, error). Nonzero IDs get exactly one completion
    // while the cache lives, even after setUrls/cancel. Once a write starts it
    // may commit despite destruction; cancel only cancels reads, never writes.
    quint64 setExtensionOverride(const QUrl &url, FileDetails::ExtensionOverride value);
    quint64 setComment(const QUrl &url, const QString &comment);
public slots:
    void refresh();
    void cancel();
signals:
    void loadingChanged(bool loading);
    void writingChanged(bool writing);
    void detailsChanged(QList<QUrl> urls);
    void batchFinished(FileDetails::Batch batch);
    void writeFinished(quint64 requestId, QUrl url, FileDetails::Error error);
private:
    class Private;
    std::unique_ptr<Private> d;
};

// Single-file Get Info editor. Host owns actual KIO rename/Undo and
// KPropertiesDialog permissions/Open With; this dialog only emits those requests.
// Host should call setUrl(newUrl) after successful native rename. Name is always
// shown in full here, irrespective of extension presentation preferences.
class FileDetailsDialog : public QDialog {
    Q_OBJECT
public:
    explicit FileDetailsDialog(QWidget *parent = nullptr);
    ~FileDetailsDialog() override;
    void setUrl(const QUrl &url);
    QUrl url() const;
    FileDetailsCache *metadataCache() const;
    void setExtensionPresentation(QString knownSuffix, bool showAllExtensions);
    // Visible host operation/blocked-close status; cleared by refresh, retarget
    // or the next write-state transition. An empty string restores normal status.
    void showOperationMessage(const QString &message);
public slots:
    void refresh();
    void reject() override;
signals:
    void closeRequested(); // Embedded editor: the host decides whether to close.
    void renameRequested(QUrl url, QString newName);
    void propertiesRequested(QUrl url);
    void metadataSaved(QUrl url); // Successful inode xattr mutation.
protected:
    void closeEvent(QCloseEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void showEvent(QShowEvent *event) override;
private:
    class Private;
    std::unique_ptr<Private> d;
};

Q_DECLARE_METATYPE(FileDetails::ExtensionOverride)
Q_DECLARE_METATYPE(FileDetails::Error)
Q_DECLARE_METATYPE(FileDetails::Details)
Q_DECLARE_METATYPE(FileDetails::Batch)
