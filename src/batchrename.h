// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#pragma once

#include <KFileItem>
#include <QDateTime>
#include <QDialog>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <memory>

class KJob;
namespace KIO { class Job; }

namespace FinderBatchRename {
constexpr int MaximumItems = 4096;
enum class Mode { ReplaceText, AddText, Format };
enum class Placement { BeforeName, AfterName };
enum class NameFormat { NameAndIndex, NameAndCounter, NameAndDate };
struct Options {
    Mode mode = Mode::ReplaceText;
    QString find;
    QString replacement;
    QString text;
    Placement placement = Placement::AfterName;
    NameFormat format = NameFormat::NameAndIndex;
    QString customName = QStringLiteral("File");
    int startNumber = 1; // 0…INT_MAX; overflowing sequences are rejected.
    QDateTime date = QDateTime::currentDateTime(); // Captured once, including preview.
};
struct Item {
    QUrl url;
    bool directory = false; // Captured KFileItem metadata; no synchronous stat.
};
struct Entry {
    Item item;
    QUrl destination;
    QString oldName;
    QString newName;
    QString problem;
    bool changed = false;
    bool extensionChanged = false;
};
struct Plan {
    QList<Entry> entries; // Immutable original URLs, in captured selection order.
    QString problem;
    int changedCount = 0;
    bool canRename() const { return problem.isEmpty() && changedCount > 0; }
};
// Supply listing KFileItems with a known file type, as Browser does. The
// public KFileItem::isDir() getter may stat arbitrary uninitialized items.
QList<Item> capture(const KFileItemList &items);
Plan preview(const QList<Item> &items, const Options &options);
// Explicit names support non-UI clients and dependency/cycle tests. Names are
// single components, never paths, and each target stays in its original parent.
Plan previewNames(const QList<Item> &items, const QStringList &names);
}

// PUBLIC KF6.2+ APIs only. Link Qt6::Widgets + KF6::KIOCore/KIOGui/KIOWidgets.
// All filesystem checks are asynchronous KIO stat jobs (at most four at once).
// Each successful moveAs is recorded by FileUndoManager as a native Rename:
// Undo reverses one step at a time, including temporary cycle-breaking steps.
// There is no application Undo stack. Cycles use at most one temporary name
// per cycle. Cancel is cooperative: the current rename/dependency group finishes
// before stopping, so cancellation cannot strand a temporary cycle name.
// Batches are bounded to 4096 items. Different parents are supported; selected
// ancestors and descendants together are rejected. URL identity is lexical;
// aliases/case-insensitive collisions remain subject to the native KIO worker.
// Local file URLs only: moveAs can fall back to copying on unsupported remote
// renames in KF6.2, so remote sources are rejected instead of writing file data.
// Errors can leave partial success, including a temporary URL after a cycle
// failure or native conflict cancellation; actual current URLs are reported,
// and every successful step has Undo. Destruction also cancels without waiting.
// Native conflict UI offers an alternate name, Skip for independent items, or
// Cancel; replacing another file/merging folders is deliberately unavailable.
// Apple A12 guide (macOS 27) supplies the options; date spelling/separators are
// a Linux approximation, not evidence of release-specific pixel/locale parity.
class BatchRenameEngine : public QObject {
    Q_OBJECT
public:
    explicit BatchRenameEngine(QWidget *dialogParent = nullptr, QObject *parent = nullptr);
    ~BatchRenameEngine() override;
    bool busy() const;
    QList<QUrl> originalUrls() const;
    QList<QUrl> currentUrls() const; // All captured items, including unchanged.
    void start(const FinderBatchRename::Plan &plan);
public slots:
    void cancel();
signals:
    void busyChanged(bool busy);
    void statusChanged(const QString &message);
    void progressChanged(int completed, int total);
    void itemRenamed(const QUrl &original, const QUrl &actual);
    void error(KJob *job); // Inspect synchronously; KIO owns/autodeletes the job.
    void validationFailed(const QString &message);
    void finished(const QList<QUrl> &renamedUrls, bool cancelled);
    // Immediately after creation, before asynchronous dispatch. Hosts normally
    // need not connect; tests can install a private native conflict delegate.
    void jobCreated(KIO::Job *job);
private:
    struct Private;
    std::unique_ptr<Private> d;
};

// Integration: new BatchRenameDialog(selectedItems, window), connect the two
// signals below as for RenameFileDialog, then show(). Modeless, delete-on-close.
// The dialog captures selection metadata/URLs at construction; navigation or
// later selection changes cannot retarget it. Rename does not close it until
// asynchronous work finishes. Cancel/Escape/close request cooperative stopping.
// Replace operates on actual filenames (even hidden extensions); Add/Format
// preserve recognized compound extensions, unknown final suffixes, and dotfiles.
// Replace extension changes honor Advanced/warnExtensionChange through an
// asynchronous confirmation. No filesystem writes occur before confirmation.
class BatchRenameDialog : public QDialog {
    Q_OBJECT
public:
    explicit BatchRenameDialog(const KFileItemList &items, QWidget *parent = nullptr);
    ~BatchRenameDialog() override;
    QList<QUrl> originalUrls() const;
    FinderBatchRename::Options options() const;
    FinderBatchRename::Plan preview() const;
    void setOptions(const FinderBatchRename::Options &options);
    bool busy() const;
public slots:
    void accept() override;
    void reject() override;
signals:
    void renamingFinished(const QList<QUrl> &urls); // Actual partial/success URLs.
    void error(KJob *job);
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    struct Private;
    std::unique_ptr<Private> d;
};
