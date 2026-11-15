// SPDX-License-Identifier: MIT
// Independent headless tests; fixtures are private temporary local files.
#include "search.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <cstdio>
#include <stdexcept>
#include <sys/xattr.h>

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
void fixture(const QString &path) {
    require(QDir().mkpath(QFileInfo(path).path()), "create fixture directory");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "create fixture file");
    require(file.write("test fixture\n") > 0, "write fixture");
}
struct Result {
    QList<QUrl> urls;
    int count = -1;
    int skipped = 0;
    int heartbeats = 0;
    bool limited = false;
    bool ended = false;
    QString error;
};
Result scan(LocalFileSearch &search, const QUrl &root, const QString &query,
            const LocalFileSearch::Options &options = {}) {
    Result result;
    QEventLoop loop;
    QTimer deadline, heartbeat;
    deadline.setSingleShot(true);
    deadline.start(5000);
    heartbeat.start(1);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&heartbeat, &QTimer::timeout, &loop, [&] { ++result.heartbeats; });
    auto batch = QObject::connect(&search, &LocalFileSearch::filesFound, &loop, [&](quint64, const QList<QUrl> &urls) {
        require(QThread::currentThread() == QCoreApplication::instance()->thread(), "batch delivered off UI thread");
        result.urls += urls;
    });
    auto finished = QObject::connect(&search, &LocalFileSearch::finished, &loop, [&](quint64, int count, bool limited, int skipped) {
        result.count = count;
        result.limited = limited;
        result.skipped = skipped;
        result.ended = true;
        loop.quit();
    });
    auto error = QObject::connect(&search, &LocalFileSearch::error, &loop, [&](quint64, const QString &text) {
        result.error = text;
        result.ended = true;
        loop.quit();
    });
    QElapsedTimer elapsed;
    elapsed.start();
    search.start(root, query, options);
    require(elapsed.elapsed() < 100, "start blocked the UI thread");
    loop.exec();
    QObject::disconnect(batch);
    QObject::disconnect(finished);
    QObject::disconnect(error);
    require(result.ended, "search deadline expired");
    require(!search.isRunning(), "finished search marked running");
    return result;
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QJsonArray cases;
    try {
        QTemporaryDir temporary;
        require(temporary.isValid(), "temporary fixture root");
        const QString root = temporary.path() + "/tree";
        fixture(root + "/Needle.txt");
        fixture(root + "/nested/deeper/needle-photo.png");
        fixture(root + "/nested/ordinary.txt");
        fixture(root + "/.needle-secret");
        fixture(root + "/.hidden/needle-deep.txt");
        fixture(temporary.path() + "/outside/needle-outside.txt");
        require(QFile::link(temporary.path() + "/outside", root + "/needle-link"), "directory symlink fixture");
        require(QFile::link(root, root + "/nested/cycle"), "symlink cycle fixture");
        require(QFile::link(temporary.path() + "/absent", root + "/needle-dangling"), "dangling symlink fixture");
        LocalFileSearch search;
        const auto url = QUrl::fromLocalFile(root);
        auto result = scan(search, url, QStringLiteral("NEEDLE"));
        require(result.error.isEmpty() && result.count == 4 && result.urls.size() == 4, "recursive case-insensitive files and links");
        for (const auto &file : result.urls) {
            require(!file.toLocalFile().contains("needle-outside"), "followed directory symlink");
            require(!file.toLocalFile().contains(".hidden") && !file.toLocalFile().contains(".needle-secret"), "hidden item leaked");
        }
        cases.append("recursive case-insensitive names, no symlink recursion, hidden excluded");
        LocalFileSearch::Options hidden;
        hidden.showHidden = true;
        result = scan(search, url, "needle", hidden);
        require(result.error.isEmpty() && result.count == 6 && result.urls.size() == 6, "hidden files or hidden recursion missing");
        cases.append("hidden files and hidden directories included when requested");
        const QByteArray tagged = QFile::encodeName(root + "/Needle.txt");
        require(setxattr(tagged.constData(), "user.xdg.tags", "Blue,Project", 11, 0) == 0, "tag fixture xattr");
        LocalFileSearch::Options tags;
        tags.requiredTag = "Blue";
        result = scan(search, url, QString(), tags);
        require(result.error.isEmpty() && result.count == 1 && result.urls.first() == QUrl::fromLocalFile(root + "/Needle.txt"), "real tagged results");
        tags.requiredTag = "Blu";
        result = scan(search, url, QString(), tags);
        require(result.count == 0, "tags use full values rather than substring matching");
        cases.append("tag results read real xattrs without following symlinks or matching partial tags");
        tags.requiredTag = "Blue";
        tags.preferredPath = root + "/nested/deeper";
        const QByteArray preferred = QFile::encodeName(tags.preferredPath + "/needle-photo.png");
        require(setxattr(preferred.constData(), "user.xdg.tags", "Blue", 4, 0) == 0, "preferred tag fixture");
        tags.maxEntries = 1;
        result = scan(search, url, QString(), tags);
        require(result.count == 1 && result.urls.first() == QUrl::fromLocalFile(tags.preferredPath + "/needle-photo.png"), "preferred folder scanned within entry budget");
        tags.maxEntries = 250000;
        require(setxattr(QFile::encodeName(tags.preferredPath).constData(), "user.xdg.tags", "Blue", 4, 0) == 0, "preferred directory tag fixture");
        result = scan(search, url, QString(), tags);
        require(result.count == 3 && result.urls.contains(QUrl::fromLocalFile(tags.preferredPath)), "preferred directory missing or descendants duplicated");
        cases.append("preferred descendant is searched first without duplicate results or new budgets");
        auto bounded = hidden;
        bounded.maxResults = 2;
        result = scan(search, url, "needle", bounded);
        require(result.error.isEmpty() && result.limited && result.count == 2 && result.urls.size() == 2, "result bound");
        cases.append("bounded result count and limited status");
        bounded.maxResults = 1000;
        bounded.maxEntries = 1;
        result = scan(search, url, "needle", bounded);
        require(result.error.isEmpty() && result.limited && result.count <= 1, "entry bound");
        cases.append("entry budget ends scanning");
        result = scan(search, QUrl("smb://server/share"), "needle");
        require(!result.error.isEmpty() && result.urls.isEmpty(), "remote root fallback");
        result = scan(search, QUrl::fromLocalFile("/proc"), "needle");
        require(!result.error.isEmpty() && result.urls.isEmpty(), "virtual mount fallback");
        result = scan(search, QUrl::fromLocalFile(root + "/needle-link"), "needle");
        require(!result.error.isEmpty() && result.urls.isEmpty(), "root symlink followed");
        result = scan(search, QUrl::fromLocalFile(root + "/missing"), "needle");
        require(!result.error.isEmpty() && result.urls.isEmpty(), "missing root fallback");
        result = scan(search, QUrl::fromLocalFile(root + "/Needle.txt"), "needle");
        require(!result.error.isEmpty() && result.urls.isEmpty(), "regular-file root fallback");
        cases.append("remote URL, virtual mount and symlink roots refused");
        result = scan(search, url, "   ");
        require(result.error.isEmpty() && result.count == 0 && result.urls.isEmpty(), "empty query scanned entire tree");
        cases.append("empty query finishes with no results");

        const QString large = temporary.path() + "/large";
        for (int directory = 0; directory < 100; ++directory)
            for (int file = 0; file < 30; ++file)
                fixture(large + QStringLiteral("/folder-%1/needle-%2.txt").arg(directory).arg(file));
        result = scan(search, QUrl::fromLocalFile(large), "no-match");
        require(result.error.isEmpty() && result.count == 0 && result.heartbeats > 0, "UI event loop stopped during scanning");
        cases.append("UI timer continues during recursive scan");
        const int scanHeartbeats = result.heartbeats;
        auto timed = hidden;
        timed.maxElapsedMs = 1;
        result = scan(search, QUrl::fromLocalFile(large), "no-match", timed);
        require(result.error.isEmpty() && result.limited, "elapsed-time bound");
        cases.append("elapsed-time budget ends scanning");

        QEventLoop cancelLoop;
        QTimer cancelDeadline;
        cancelDeadline.setSingleShot(true);
        cancelDeadline.start(5000);
        QObject::connect(&cancelDeadline, &QTimer::timeout, &cancelLoop, &QEventLoop::quit);
        bool cancelSent = false, canceled = false;
        int lateBatches = 0;
        quint64 canceledGeneration = 0;
        auto cancelSignal = QObject::connect(&search, &LocalFileSearch::canceled, &cancelLoop, [&](quint64 generation) {
            canceled = generation == canceledGeneration;
            QTimer::singleShot(100, &cancelLoop, &QEventLoop::quit);
        });
        auto cancelBatch = QObject::connect(&search, &LocalFileSearch::filesFound, &cancelLoop, [&](quint64 generation, const QList<QUrl> &) {
            if (generation != canceledGeneration) return;
            if (cancelSent) { ++lateBatches; return; }
            cancelSent = true;
            search.cancel();
        });
        canceledGeneration = search.start(QUrl::fromLocalFile(large), "needle");
        cancelLoop.exec();
        QObject::disconnect(cancelSignal);
        QObject::disconnect(cancelBatch);
        require(cancelSent && canceled && lateBatches == 0 && !search.isRunning(), "cancel delivered stale batches");
        cases.append("cancellation from first batch suppresses queued old results");

        QEventLoop generationLoop;
        QTimer generationDeadline;
        generationDeadline.setSingleShot(true);
        generationDeadline.start(5000);
        QObject::connect(&generationDeadline, &QTimer::timeout, &generationLoop, &QEventLoop::quit);
        bool latestFinished = false;
        int olderBatches = 0;
        quint64 latest = 0;
        auto generationBatch = QObject::connect(&search, &LocalFileSearch::filesFound, &generationLoop, [&](quint64 generation, const QList<QUrl> &) {
            if (generation != latest) ++olderBatches;
        });
        auto generationFinished = QObject::connect(&search, &LocalFileSearch::finished, &generationLoop, [&](quint64 generation, int count, bool, int) {
            latestFinished = generation == latest && count == 4;
            generationLoop.quit();
        });
        search.start(QUrl::fromLocalFile(large), "needle");
        latest = search.start(url, "needle");
        generationLoop.exec();
        QObject::disconnect(generationBatch);
        QObject::disconnect(generationFinished);
        require(latestFinished && olderBatches == 0, "replacement generation leaked old results");
        cases.append("replacement request cancels prior generation and only latest finishes");

        QElapsedTimer destruction;
        destruction.start();
        auto *dying = new LocalFileSearch;
        dying->start(QUrl::fromLocalFile(large), "needle");
        delete dying;
        require(destruction.elapsed() < 100, "destructor blocked UI thread");
        QEventLoop cleanup;
        QTimer::singleShot(100, &cleanup, &QEventLoop::quit);
        cleanup.exec();
        cases.append("destruction cancels worker without joining on UI thread");
        std::puts(QJsonDocument(QJsonObject{{"passed", true}, {"cases", cases}, {"tests", cases.size()},
                    {"gui_windows_launched", 0}, {"fixture_scope", "QTemporaryDir"},
                    {"ui_heartbeats_during_3000_entry_scan", scanHeartbeats}}).toJson().constData());
        return 0;
    } catch (const std::exception &failure) {
        std::fprintf(stderr, "search test failed: %s\n", failure.what());
        return 1;
    }
}
