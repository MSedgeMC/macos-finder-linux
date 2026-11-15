// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
// Standalone: this file + src/inspector.{h,cpp}, src/tags.cpp and
// src/preferences.cpp; AUTOMOC, C++20, Qt6::Widgets, Threads::Threads,
// KF6::FileMetaData and KF6::KIOCore. Link with --wrap=open, --wrap=fstat,
// --wrap=statx and --wrap=faccessat. Run via tests/run-headless.py.
// Wrappers affect only this executable's calls, never Qt's fixture I/O.
#include "inspector.h"

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QTableView>
#include <QTemporaryDir>
#include <QThread>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace {
void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

struct Hooks {
    std::mutex mutex;
    std::condition_variable wake;
    QByteArray prefix;
    QList<QByteArray> gated;
    QByteArray denied;
    QByteArray unreadable;
    QByteArray noBirth;
    int entered = 0;
    int active = 0;
    int peak = 0;
    qsizetype probes = 0;
    QHash<QByteArray, qsizetype> counts;
    QHash<int, QByteArray> descriptors;
    bool released = true;
} hooks;

void releaseGates()
{
    std::lock_guard lock(hooks.mutex);
    hooks.released = true;
    hooks.wake.notify_all();
}
void gate(const QList<QString> &paths)
{
    std::lock_guard lock(hooks.mutex);
    hooks.gated.clear();
    for (const auto &path : paths) hooks.gated.append(QFile::encodeName(path));
    hooks.entered = hooks.active = hooks.peak = 0;
    hooks.released = false;
}
bool waitFor(const std::function<bool()> &condition, int timeoutMs = 10000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return condition();
}
void drain(int milliseconds = 80)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
}
void write(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "create fixture");
    require(file.write(bytes) == bytes.size(), "write fixture");
}
void tag(const QString &path, const QByteArray &tags)
{
    require(::setxattr(QFile::encodeName(path).constData(), "user.xdg.tags", tags.constData(), tags.size(), 0) == 0,
        "seed real native tags");
}
void modified(const QString &path, qint64 seconds)
{
    const struct timespec times[2]{{seconds, 0}, {seconds, 0}};
    require(::utimensat(AT_FDCWD, QFile::encodeName(path).constData(), times, AT_SYMLINK_NOFOLLOW) == 0,
        "set real filesystem dates");
}
QTableView *table(Inspector &inspector) { return inspector.findChild<QTableView *>("inspectorItems"); }
QLabel *label(Inspector &inspector, const QString &name)
{
    auto *result = inspector.findChild<QLabel *>(name);
    require(result, "expected information label");
    return result;
}
void completed(Inspector &inspector)
{
    require(waitFor([&] { return !inspector.isLoading(); }), "async metadata completion");
    require(!inspector.summary().cancelled, "unexpected metadata cancellation");
}
void inspect(Inspector &inspector, const QList<QUrl> &urls)
{
    QElapsedTimer timer;
    timer.start();
    inspector.setUrls(urls);
    require(timer.elapsed() < 150, "selection change blocked GUI thread");
    require(inspector.isLoading() || urls.isEmpty(), "nonempty selection did not start asynchronously");
    completed(inspector);
}
int entered()
{
    std::lock_guard lock(hooks.mutex);
    return hooks.entered;
}
qsizetype probes()
{
    std::lock_guard lock(hooks.mutex);
    return hooks.probes;
}

// Replaces a selection when its result is already queued, before delivery.
class BeforeDelivery final : public QObject {
public:
    std::function<void()> before;
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::MetaCall && before) {
            auto action = std::move(before);
            action();
        }
        return false;
    }
};
} // namespace

extern "C" int __real_open(const char *, int, ...);
extern "C" int __real_fstat(int, struct stat *);
extern "C" int __real_statx(int, const char *, int, unsigned int, struct statx *);
extern "C" int __real_faccessat(int, const char *, int, int);

extern "C" int __wrap_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if ((flags & O_CREAT) || ((flags & O_TMPFILE) == O_TMPFILE)) {
        va_list arguments;
        va_start(arguments, flags); mode = va_arg(arguments, mode_t); va_end(arguments);
    }
    const QByteArray name(path);
    if (flags & O_PATH) {
        std::unique_lock lock(hooks.mutex);
        if (!hooks.prefix.isEmpty() && name.startsWith(hooks.prefix + '/')) {
            ++hooks.probes;
            ++hooks.counts[name];
        }
        if (!hooks.released && hooks.gated.contains(name)) {
            ++hooks.entered;
            ++hooks.active;
            hooks.peak = std::max(hooks.peak, hooks.active);
            hooks.wake.notify_all();
            hooks.wake.wait(lock, [] { return hooks.released; });
            --hooks.active;
        }
        if (!hooks.denied.isEmpty() && name == hooks.denied) { errno = EACCES; return -1; }
    }
    const int descriptor = __real_open(path, flags, mode);
    if (descriptor >= 0) {
        std::lock_guard lock(hooks.mutex);
        hooks.descriptors.insert(descriptor, name);
    }
    return descriptor;
}

extern "C" int __wrap_fstat(int descriptor, struct stat *metadata)
{
    return __real_fstat(descriptor, metadata);
}

extern "C" int __wrap_statx(int descriptor, const char *path, int flags, unsigned int mask, struct statx *metadata)
{
    {
        std::lock_guard lock(hooks.mutex);
        if (!hooks.noBirth.isEmpty() && hooks.descriptors.value(descriptor) == hooks.noBirth) {
            errno = ENOSYS; return -1;
        }
    }
    return __real_statx(descriptor, path, flags, mask, metadata);
}

extern "C" int __wrap_faccessat(int descriptor, const char *path, int mode, int flags)
{
    const QByteArray inodePath(path);
    if (inodePath.startsWith("/proc/self/fd/")) {
        const int pinned = inodePath.mid(14).toInt();
        std::lock_guard lock(hooks.mutex);
        if (!hooks.unreadable.isEmpty() && hooks.descriptors.value(pinned) == hooks.unreadable) {
            errno = EACCES; return -1;
        }
    }
    return __real_faccessat(descriptor, path, mode, flags);
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("finder-inspector-private-test"));
    QJsonArray cases;
    try {
        // Child exits while an inode call is deliberately blocked. No native
        // window is shown; this inherits the runner's private offscreen session.
        if (app.arguments().contains(QStringLiteral("--blocked-exit"))) {
            const auto path = app.arguments().last(); // Parent owns and cleans fixture.
            write(path, "x");
            gate({path});
            auto *inspector = new Inspector;
            inspector->setUrls({QUrl::fromLocalFile(path)});
            require(waitFor([] { return entered() == 1; }), "exit test worker entered");
            delete inspector;
            // Explicitly exercise QApplication teardown. _Exit then avoids the
            // test hook's global condition-variable destructor trying to wait
            // for our deliberately blocked waiter. No production destructor
            // or controller is bypassed by that fixture-only exit.
            app.~QApplication();
            std::_Exit(0);
        }
        require(qEnvironmentVariable("QT_QPA_PLATFORM") == QStringLiteral("offscreen"), "use tests/run-headless.py");
        require(qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"), "private GUI environment");
        QTemporaryDir temporary;
        require(temporary.isValid(), "private fixture root");
        const auto root = temporary.path();
        { std::lock_guard lock(hooks.mutex); hooks.prefix = QFile::encodeName(root); }
        const auto text = root + "/<b>alpha.txt";
        const auto pdf = root + "/document.pdf";
        const auto folder = root + "/folder";
        const auto link = root + "/link";
        const auto broken = root + "/broken";
        const auto fifo = root + "/pipe";
        write(text, "0123456789");
        write(pdf, QByteArray(27, 'p'));
        require(QDir().mkdir(folder), "folder fixture");
        write(folder + "/must-not-scan.bin", QByteArray(123456, 'z'));
        require(QFile::link(text, link), "symlink fixture");
        require(::symlink("does-not-exist", QFile::encodeName(broken).constData()) == 0, "broken symlink fixture");
        require(::mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0, "FIFO fixture");
        tag(text, "Shared,Project α");
        tag(pdf, "Shared,Blue");
        tag(folder, "Shared");
        modified(text, 1700000000);
        modified(pdf, 1700003600);
        modified(folder, 1700001800);
        modified(link, 1700001000);
        modified(fifo, 1700002000);
        const auto textUrl = QUrl::fromLocalFile(text);
        const auto pdfUrl = QUrl::fromLocalFile(pdf);
        const auto folderUrl = QUrl::fromLocalFile(folder);
        Inspector inspector;
        require(inspector.followsSelection() && !inspector.isModal(), "default modeless selection inspector");
        require(inspector.urls().isEmpty() && !inspector.isLoading(), "empty initial selection");
        int notifications = 0;
        QList<bool> loading;
        QObject::connect(&inspector, &Inspector::metadataChanged, &inspector, [&] { ++notifications; });
        QObject::connect(&inspector, &Inspector::loadingChanged, &inspector, [&](bool value) { loading.append(value); });
        inspect(inspector, {textUrl});
        auto summary = inspector.summary();
        require(summary.selectedCount == 1 && summary.availableCount == 1 && summary.fileCount == 1 && summary.fileBytes == 10,
            "real single-file metadata");
        require(summary.earliestModified.toSecsSinceEpoch() == 1700000000 && summary.latestModified == summary.earliestModified,
            "actual modification date");
        struct statx birth {};
        if (__real_statx(AT_FDCWD, QFile::encodeName(text).constData(), AT_SYMLINK_NOFOLLOW, STATX_BTIME, &birth) == 0
            && (birth.stx_mask & STATX_BTIME)) {
            require(summary.createdCount == 1 && summary.earliestCreated.toSecsSinceEpoch() == birth.stx_btime.tv_sec,
                "actual filesystem birth time");
        } else require(summary.createdCount == 0, "invented creation date");
        require(summary.kinds.size() == 1 && summary.tagsReadCount == 1 && summary.commonTags == QStringList{"Project α", "Shared"},
            "actual kind and native tag values");
        require(label(inspector, "inspectorHeading")->text() == "<b>alpha.txt"
            && label(inspector, "inspectorHeading")->textFormat() == Qt::PlainText, "name interpreted as markup");
        require(loading == QList<bool>{true, false} && notifications == 1, "loading and completion signals");
        cases.append("actual metadata, birth/modification dates, read-only native tags and plain captured name");

        const QList<QUrl> multi{textUrl, pdfUrl, folderUrl, QUrl::fromLocalFile(link), QUrl::fromLocalFile(fifo)};
        inspect(inspector, multi);
        summary = inspector.summary();
        require(summary.selectedCount == 5 && summary.inspectedCount == 5 && summary.availableCount == 5
            && summary.fileCount == 2 && summary.folderCount == 1 && summary.linkCount == 1 && summary.otherCount == 1,
            "aggregate selected-entry type counts");
        require(summary.fileBytes == 37 && !summary.sizeOverflow, "folder, link or FIFO bytes included in file sum");
        require(summary.earliestModified.toSecsSinceEpoch() == 1700000000 && summary.latestModified.toSecsSinceEpoch() == 1700003600,
            "actual date range");
        require(summary.tagsReadCount == 3 && summary.tagsUnavailableCount == 2 && summary.commonTags == QStringList{"Shared"}
            && summary.tagSetsDiffer, "common tags confused with tag union or unsupported items");
        require(summary.location == root && !summary.multipleLocations, "common enclosing location");
        require(label(inspector, "inspectorHeading")->text().contains("Selection Summary"), "multiple items disguised as individual Get Info");
        {
            std::lock_guard lock(hooks.mutex);
            require(hooks.counts.value(QFile::encodeName(folder + "/must-not-scan.bin")) == 0, "folder contents were scanned");
        }
        cases.append("multiple-item counts, exact sizes, date ranges and shared tags; folders and links never scanned");

        const auto sparsePath = root + "/sparse.bin";
        const qint64 sparseBytes = (qint64(1) << 32) + 17;
        {
            QFile sparse(sparsePath);
            require(sparse.open(QIODevice::WriteOnly) && sparse.resize(sparseBytes), "create sparse file larger than 32-bit size");
        }
        inspect(inspector, {QUrl::fromLocalFile(sparsePath), textUrl});
        require(inspector.summary().fileBytes == quint64(sparseBytes + 10), "logical sparse sizes lost 64-bit precision");
        auto localhost = textUrl;
        localhost.setHost(QStringLiteral("localhost"));
        inspect(inspector, {localhost});
        require(inspector.summary().fileBytes == 10 && inspector.summary().tagsReadCount == 1,
            "explicit localhost URL did not use local metadata and tags");
        inspect(inspector, multi);
        cases.append("real sparse-file sizes stay exact beyond 32 bits; file://localhost reads the local inode");

        QUrl requested;
        QList<QUrl> revealed;
        QObject::connect(&inspector, &Inspector::propertiesRequested, &inspector, [&](QUrl url) { requested = url; });
        QObject::connect(&inspector, &Inspector::revealRequested, &inspector, [&](QList<QUrl> urls) { revealed = urls; });
        auto *view = table(inspector);
        view->setCurrentIndex(view->model()->index(1, 0));
        inspector.findChild<QPushButton *>("inspectorProperties")->click();
        require(requested == pdfUrl, "native properties button lost selected URL");
        requested = QUrl();
        view->clicked(view->model()->index(2, 2));
        require(requested == folderUrl, "per-item native Get Info link");
        requested = QUrl();
        view->activated(view->model()->index(0, 0));
        require(requested == textUrl, "keyboard item activation");
        inspector.findChild<QPushButton *>("inspectorReveal")->click();
        require(revealed == multi, "reveal did not forward captured URLs");
        require(label(inspector, "inspectorItem_name")->text() == "document.pdf", "selected item detail name");
        require(inspector.findChildren<QCheckBox *>().size() == 1, "fake locked/permission controls");
        cases.append("per-URL native properties and enclosing-folder signals preserve host ownership");

        inspector.setFollowsSelection(false);
        inspector.setUrls({pdfUrl});
        require(inspector.urls() == multi && !inspector.isLoading(), "pinned selection replaced");
        inspector.setFollowsSelection(true);
        completed(inspector);
        require(inspector.urls() == QList<QUrl>{pdfUrl} && inspector.summary().fileBytes == 27, "latest host selection lost while pinned");
        const int beforeIdentical = notifications;
        inspector.setUrls({pdfUrl});
        require(!inspector.isLoading() && notifications == beforeIdentical, "identical selection rescanned");
        write(pdf, QByteArray(41, 'p'));
        inspector.refresh();
        completed(inspector);
        require(inspector.summary().fileBytes == 41, "explicit refresh ignored actual metadata change");
        inspect(inspector, {textUrl, QUrl::fromLocalFile(folder + "/must-not-scan.bin")});
        require(inspector.summary().multipleLocations, "different URL parents merged");
        cases.append("pin/unpin tracks latest host selection; refresh discovers changed metadata without paint-time scanning");

        {
            std::lock_guard lock(hooks.mutex);
            hooks.noBirth = QFile::encodeName(text);
            hooks.unreadable = QFile::encodeName(text);
            hooks.denied = QFile::encodeName(root + "/permission-denied.txt");
        }
        const QUrl remote("sftp://name:secret@example.invalid/share/remote.txt");
        inspect(inspector, {textUrl, QUrl::fromLocalFile(root + "/missing.txt"), remote,
            QUrl::fromLocalFile(root + "/permission-denied.txt"), QUrl::fromLocalFile(broken), QUrl("relative.txt")});
        summary = inspector.summary();
        require(summary.fileCount == 1 && summary.fileBytes == 10 && summary.linkCount == 1 && summary.remoteCount == 1
            && summary.unavailableCount == 3 && summary.unreadableCount == 1, "unavailable and unreadable metadata falsely counted");
        require(summary.createdCount <= 1 && summary.modifiedCount == 2, "ctime used as creation fallback");
        view->setCurrentIndex(view->model()->index(0, 0));
        require(label(inspector, "inspectorItem_created")->text().contains("Not provided"), "missing birth time hidden");
        require(label(inspector, "inspectorItem_status")->text().contains("not readable"), "unreadable content status omitted");
        view->setCurrentIndex(view->model()->index(2, 0));
        require(label(inspector, "inspectorItem_status")->text().contains("remote"), "remote metadata fabricated");
        for (auto *value : inspector.findChildren<QLabel *>()) require(!value->text().contains("secret"), "remote credentials displayed");
        inspector.findChild<QPushButton *>("inspectorProperties")->click();
        require(requested == remote, "native remote properties URL changed");
        {
            std::lock_guard lock(hooks.mutex);
            hooks.noBirth.clear(); hooks.unreadable.clear(); hooks.denied.clear();
        }
        cases.append("missing/denied/relative/remote metadata and unreadable contents remain explicit; broken links stay links");

        QList<QUrl> large;
        for (int index = 0; index < 10000; ++index) large.append(QUrl::fromLocalFile(root + QStringLiteral("/absent-%1.bin").arg(index)));
        const auto beforeLarge = probes();
        inspect(inspector, large);
        summary = inspector.summary();
        require(summary.selectedCount == 10000 && summary.inspectedCount == Inspector::maximumMetadataItems
            && summary.unavailableCount == Inspector::maximumMetadataItems && summary.fileBytes == 0,
            "large-selection limit or truthful partial count");
        require(probes() - beforeLarge == Inspector::maximumMetadataItems, "metadata exceeded selected-entry budget");
        require(view->model()->rowCount() == 10000, "bounded metadata lost native properties for remaining captured URLs");
        view->setCurrentIndex(view->model()->index(9999, 0));
        require(view->model()->index(9999, 0).data().toString() == "absent-9999.bin", "large-selection name derived from wrong selection");
        inspector.findChild<QPushButton *>("inspectorProperties")->click();
        require(requested == large.last(), "uninspected URL has no native Get Info");
        require(label(inspector, "inspectorStatus")->text().contains("Partial summary"), "metadata truncation not explained");
        const auto beforePaint = probes();
        for (int index = 0; index < 10000; ++index) {
            view->model()->index(index, 0).data();
            view->model()->index(index, 1).data();
            view->model()->index(index, 2).data(Qt::ToolTipRole);
        }
        inspector.show();
        drain();
        const auto image = inspector.grab().toImage();
        require(!image.isNull() && image.width() >= 600, "private offscreen rendering");
        require(probes() == beforePaint, "paint/model reads performed filesystem scans");
        inspector.hide();
        cases.append("10,000 captured URLs retain native actions; only 4,096 probes; rendering/model lookups perform no I/O");

        const auto slow = root + "/slow.txt";
        const auto never = root + "/superseded.txt";
        write(slow, QByteArray(11, 's'));
        write(never, QByteArray(99, 'n'));
        gate({slow});
        const int beforeReplace = notifications;
        inspector.setUrls({QUrl::fromLocalFile(slow)});
        require(waitFor([] { return entered() == 1; }), "slow filesystem test entered");
        for (int index = 0; index < 100; ++index) {
            inspector.setUrls({QUrl::fromLocalFile(never)});
            inspector.setUrls({pdfUrl});
        }
        require(inspector.urls() == QList<QUrl>{pdfUrl}
            && label(inspector, "inspectorHeading")->text() == "document.pdf", "captured names stayed on stale selection");
        releaseGates();
        completed(inspector);
        require(notifications == beforeReplace + 1 && inspector.summary().fileBytes == 41, "stale worker result was delivered");
        {
            std::lock_guard lock(hooks.mutex);
            require(hooks.counts.value(QFile::encodeName(never)) == 0, "superseded requests were not coalesced");
        }
        BeforeDelivery delivery;
        inspector.installEventFilter(&delivery);
        delivery.before = [&] { inspector.setUrls({pdfUrl}); };
        const int beforeDelivery = notifications;
        inspector.setUrls({textUrl});
        completed(inspector);
        inspector.removeEventFilter(&delivery);
        require(notifications == beforeDelivery + 1 && inspector.urls() == QList<QUrl>{pdfUrl}
            && inspector.summary().fileBytes == 41, "already posted result ignored queued generation check");
        cases.append("rapid selections coalesce; running and already-queued stale metadata never replace current results");

        gate({slow});
        const int beforeCancel = notifications;
        inspector.setUrls({QUrl::fromLocalFile(slow)});
        require(waitFor([] { return entered() == 1; }), "cancel test worker entered");
        QElapsedTimer cancelTime;
        cancelTime.start();
        inspector.cancelMetadata();
        require(cancelTime.elapsed() < 100 && !inspector.isLoading() && inspector.summary().cancelled, "cancel blocked or not explicit");
        releaseGates();
        drain();
        require(notifications == beforeCancel, "cancelled worker notified");
        inspector.show();
        completed(inspector);
        require(inspector.summary().fileBytes == 11, "cancelled inspector did not refresh on reopening");
        gate({slow});
        inspector.refresh();
        require(waitFor([] { return entered() == 1; }), "hide test worker entered");
        cancelTime.restart();
        inspector.hide();
        require(cancelTime.elapsed() < 100 && !inspector.isLoading() && inspector.summary().cancelled, "hide blocked metadata cancellation");
        releaseGates();
        drain();
        inspect(inspector, {});
        require(!inspector.isLoading() && inspector.summary().selectedCount == 0 && view->model()->rowCount() == 0,
            "empty selection retained old metadata");
        cases.append("Stop/hide cancel nonblockingly; reopen refreshes; empty selection clears all results");

        // Time bound is cooperative: a blocked system call itself cannot be
        // interrupted, but after returning it cannot start another selected item.
        gate({slow});
        inspector.setUrls({QUrl::fromLocalFile(slow), pdfUrl});
        require(waitFor([] { return entered() == 1; }), "time-limit test worker entered");
        drain(5100);
        releaseGates();
        completed(inspector);
        require(inspector.summary().timeLimited && inspector.summary().inspectedCount == 1
            && inspector.summary().fileBytes == 11, "cooperative time limit or partial data accuracy");
        require(label(inspector, "inspectorStatus")->text().contains("time limit"), "time bound not explained");
        cases.append("five-second cooperative budget stops after blocked call returns and keeps partial metadata truthful");

        const auto slow2 = root + "/slow2.txt";
        write(slow2, "22");
        gate({slow, slow2});
        auto *first = new Inspector;
        auto *second = new Inspector;
        int deadSignals = 0;
        QObject::connect(first, &Inspector::metadataChanged, &app, [&] { ++deadSignals; });
        QObject::connect(second, &Inspector::metadataChanged, &app, [&] { ++deadSignals; });
        first->setUrls({QUrl::fromLocalFile(slow)});
        second->setUrls({QUrl::fromLocalFile(slow2)});
        require(waitFor([] { return entered() == 2; }), "two-worker bound test entered");
        QList<Inspector *> waiting;
        for (int index = 0; index < 32; ++index) {
            auto *value = new Inspector;
            value->setUrls({textUrl});
            waiting.append(value);
        }
        cancelTime.restart();
        delete first;
        delete second;
        for (auto *value : waiting) delete value;
        require(cancelTime.elapsed() < 150, "destruction joined blocked worker or queued request");
        {
            std::lock_guard lock(hooks.mutex);
            require(hooks.peak == 2, "global metadata worker concurrency exceeded two");
        }
        releaseGates();
        drain();
        require(deadSignals == 0, "dead controller received metadata");
        auto *reentrant = new Inspector;
        QPointer<Inspector> dead(reentrant);
        QObject::connect(reentrant, &Inspector::loadingChanged, &app, [reentrant](bool value) { if (!value) delete reentrant; });
        reentrant->setUrls({textUrl});
        require(waitFor([&] { return dead.isNull(); }), "completion receiver deletion");
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--blocked-exit"), root + "/blocked-exit.txt"});
        const bool exited = child.waitForStarted(2000) && child.waitForFinished(4000);
        if (!exited) { child.kill(); child.waitForFinished(2000); }
        require(exited && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
            "application shutdown waited for blocked metadata");
        cases.append("destruction/queued requests/completion deletion are safe; two-worker cap; app exit never joins blocked metadata");

        std::puts(QJsonDocument(QJsonObject{{"passed", true}, {"tests", cases.size()}, {"cases", cases},
            {"gui_platform", "private offscreen"}, {"production_files_changed", 0}}).toJson().constData());
        return 0;
    } catch (const std::exception &failure) {
        releaseGates();
        std::fprintf(stderr, "inspector test failed: %s\n", failure.what());
        return 1;
    }
}
