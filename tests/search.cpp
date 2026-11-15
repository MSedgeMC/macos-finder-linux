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
#include <QDateTime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <stdexcept>
#include <sys/xattr.h>

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
void fixture(const QString &path, const QByteArray &contents = "test fixture\n") {
    require(QDir().mkpath(QFileInfo(path).path()), "create fixture directory");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "create fixture file");
    require(file.write(contents) > 0, "write fixture");
}
struct Result {
    QList<QUrl> urls;
    int count = -1;
    int skipped = 0;
    int heartbeats = 0;
    bool limited = false;
    bool ended = false;
    QString error;
    QJsonObject provider;
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
    auto status = QObject::connect(&search, &LocalFileSearch::providerStatus, &loop, [&](quint64, const QJsonObject &value) {
        result.provider = value;
    });
    QElapsedTimer elapsed;
    elapsed.start();
    search.start(root, query, options);
    require(elapsed.elapsed() < 100, "start blocked the UI thread");
    loop.exec();
    QObject::disconnect(batch);
    QObject::disconnect(finished);
    QObject::disconnect(error);
    QObject::disconnect(status);
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
        LocalFileSearch::Options criteria;
        criteria.kind = "image";
        result = scan(search, url, QString(), criteria);
        require(result.error.isEmpty() && result.urls.contains(QUrl::fromLocalFile(root + "/nested/deeper/needle-photo.png")), "kind-only search returns matching files");
        criteria = {};
        criteria.minimumSize = 1;
        result = scan(search, url, QString(), criteria);
        require(result.error.isEmpty() && result.count > 0, "size-only search returns matching files");
        cases.append("metadata-only criteria scan without a filename query");

        // Behavioral fixtures: expected files are independently chosen from
        // their actual names, metadata and contents, not evaluator internals.
        const QString rich = temporary.path() + "/rich";
        fixture(rich + "/invoice.txt", "Quarterly amber budget\n");
        fixture(rich + "/draft.txt", "not approved\n");
        fixture(rich + "/holiday.png", QByteArray("\x89PNG\0binary", 11));
        fixture(rich + "/large.log", QByteArray(20000, 'x') + "amber tail");
        fixture(rich + "/blob.bin", QByteArray("\0amber binary", 13));
        fixture(rich + "/notes", "extensionless amber text\n");
        fixture(rich + "/utf16.txt", QByteArray::fromHex("fffe61006d00620065007200"));
        fixture(rich + "/utf8.txt", QStringLiteral("Résumé AMBER\n").toUtf8());
        require(QFile::link(temporary.path() + "/outside/needle-outside.txt", rich + "/external.txt"), "content symlink fixture");
        require(mkfifo(QFile::encodeName(rich + "/pipe.txt").constData(), 0600) == 0, "FIFO fixture");
        require(setxattr(QFile::encodeName(rich + "/invoice.txt").constData(), "user.xdg.tags", "Blue,Project", 12, 0) == 0, "rich tag fixture");
        const auto richUrl = QUrl::fromLocalFile(rich);
        const auto condition = [](const QString &field, const QString &op, const QJsonValue &value) {
            return QJsonObject{{"field", field}, {"op", op}, {"value", value}};
        };
        const auto combine = [](const QString &match, const QJsonArray &children) {
            return QJsonObject{{"match", match}, {"criteria", children}};
        };
        LocalFileSearch::Options richOptions;
        richOptions.criteria = combine("all", {
            combine("any", {condition("kind", "equals", "image"), condition("tag", "equals", "Blue")}),
            combine("none", {condition("name", "beginsWith", "draft")})});
        result = scan(search, richUrl, {}, richOptions);
        require(result.error.isEmpty() && result.count == 2 && result.urls.contains(QUrl::fromLocalFile(rich + "/invoice.txt")) &&
            result.urls.contains(QUrl::fromLocalFile(rich + "/holiday.png")), "nested metadata Boolean criteria");
        richOptions.criteria = combine("none", {combine("any", {condition("name", "equals", "invoice.txt"), condition("kind", "equals", "image")})});
        result = scan(search, richUrl, "invoice", richOptions);
        require(result.error.isEmpty() && result.count == 0, "query is ANDed with nested None criteria");
        richOptions.criteria = condition("tag", "equals", "Blu");
        result = scan(search, richUrl, {}, richOptions);
        require(result.count == 0, "criteria tags must match entire tag");
        cases.append("nested Any/All/None metadata and exact tags compose with legacy filename constraints");

        richOptions = {};
        richOptions.parseKeywords = true;
        result = scan(search, richUrl, "name=invoice.txt OR name=draft.txt AND NOT tag:Blue", richOptions);
        require(result.error.isEmpty() && result.count == 2, "Boolean precedence and NOT keyword matching");
        result = scan(search, richUrl, "(name=invoice.txt OR name=draft.txt) AND NOT tag:Blue", richOptions);
        require(result.error.isEmpty() && result.count == 1 && result.urls.first() == QUrl::fromLocalFile(rich + "/draft.txt"), "parenthesis precedence");
        result = scan(search, richUrl, "kind:text size>=10B size<1KiB content:\"amber budget\"", richOptions);
        require(result.error.isEmpty() && result.count == 1 && result.urls.first() == QUrl::fromLocalFile(rich + "/invoice.txt"), "implicit AND size and quoted content keywords");
        fixture(rich + "/kind:text", "literal keyword filename\n");
        result = scan(search, richUrl, "kind:text");
        require(result.count == 1 && result.urls.first() == QUrl::fromLocalFile(rich + "/kind:text"), "literal backward API keywords");
        require(QFile::remove(rich + "/kind:text"), "remove literal keyword fixture");
        cases.append("opt-in keyword grammar with quoted text, implicit AND, precedence, parentheses and old literal API");

        const QDate date(2025, 7, 14);
        const qint64 begin = date.startOfDay().toSecsSinceEpoch();
        const timespec times[2]{{begin + 43200, 0}, {begin + 43200, 0}};
        require(utimensat(AT_FDCWD, QFile::encodeName(rich + "/invoice.txt").constData(), times, AT_SYMLINK_NOFOLLOW) == 0, "fixed date fixture");
        result = scan(search, richUrl, "modified:2025-07-14", richOptions);
        require(result.error.isEmpty() && result.count == 1 && result.urls.first() == QUrl::fromLocalFile(rich + "/invoice.txt"), "ISO date covers whole local day");
        richOptions.parseKeywords = false;
        richOptions.criteria = condition("size", "equals", QString::number(QFileInfo(rich + "/invoice.txt").size()));
        result = scan(search, richUrl, "invoice", richOptions);
        require(result.count == 1, "exact size equality");
        richOptions.criteria = condition("modified", "lessThan", QString::number(begin + 43200));
        result = scan(search, richUrl, "invoice", richOptions);
        require(result.count == 0, "strict timestamp boundary");
        richOptions.criteria = condition("created", "atLeast", "0");
        result = scan(search, richUrl, "invoice", richOptions);
        require(result.error.isEmpty() && (result.count == 1 || result.provider.value("unavailableMetadata").toInt() > 0), "birthtime available or honestly unavailable");
        cases.append("exact byte sizes, strict Unix timestamp boundaries, whole ISO day and genuine birthtime availability");

        richOptions = {};
        richOptions.contentText = "amber";
        result = scan(search, richUrl, {}, richOptions);
        require(result.error.isEmpty() && result.count == 5 && result.urls.contains(QUrl::fromLocalFile(rich + "/notes")) &&
            result.urls.contains(QUrl::fromLocalFile(rich + "/utf16.txt")) && result.urls.contains(QUrl::fromLocalFile(rich + "/utf8.txt")) &&
            !result.urls.contains(QUrl::fromLocalFile(rich + "/external.txt")), "actual Unicode content search and descriptor safety");
        require(result.provider.value("provider") == "local" && result.provider.value("incompleteContentFiles").toInt() > 0, "content limitations reported");
        richOptions.maxContentBytes = 64;
        result = scan(search, richUrl, "large", richOptions);
        require(result.count == 0 && result.limited && result.provider.value("incompleteContentFiles").toInt() == 1, "truncated content doesn't claim absent match");
        richOptions.contentText.clear();
        richOptions.criteria = combine("none", {condition("content", "contains", "amber")});
        result = scan(search, richUrl, {}, richOptions);
        require(result.error.isEmpty() && result.count == 1 && result.urls.first() == QUrl::fromLocalFile(rich + "/draft.txt"), "unknown content cannot match None criteria");
        richOptions.criteria = condition("content", "contains", "amber");
        richOptions.maxTotalContentBytes = 5;
        result = scan(search, richUrl, {}, richOptions);
        require(result.error.isEmpty() && result.provider.value("contentBytesRead").toString().toLongLong() <= 5 &&
            result.provider.value("incompleteContentFiles").toInt() > 0, "aggregate content IO budget");
        richOptions.maxTotalContentBytes = 32 * 1024 * 1024;
        result = scan(search, richUrl, "invoice", richOptions);
        require(result.count == 1, "bounded prefix proves positive content hit");
        richOptions.criteria = condition("content", "endsWith", "amber tail");
        result = scan(search, richUrl, "large", richOptions);
        require(result.count == 0, "truncated prefix cannot prove suffix");
        fixture(rich + "/invalid.txt", QByteArray("draft\xc3", 6));
        richOptions.criteria = combine("none", {condition("content", "contains", "amber")});
        result = scan(search, richUrl, "invalid", richOptions);
        require(result.count == 0 && result.provider.value("incompleteContentFiles").toInt() == 1, "incomplete UTF-8 must not prove content absence");
        require(QFile::remove(rich + "/invalid.txt"), "remove invalid Unicode fixture");
        cases.append("UTF-8, UTF-16 and extensionless content; binary/FIFO/symlink exclusion; prefix, aggregate IO and three-valued negation limits");

        QString parseError;
        const QStringList malformedQueries{"name:", "(invoice", "invoice OR", "NOT", "invoice )", "content:\"unclosed", "size>nonsense", "created:wrong", "kind:mail", "owner:root"};
        for (const auto &query : malformedQueries) {
            require(LocalFileSearch::parseQuery(query, &parseError).isEmpty() && !parseError.isEmpty(), "malformed query accepted");
        }
        require(LocalFileSearch::parseQuery(QString(20000, 'x'), &parseError).isEmpty() && !parseError.isEmpty(), "oversize query accepted");
        require(!LocalFileSearch::validateCriteria(condition("size", "equals", "9223372036854775808"), &parseError), "overflow size accepted");
        require(!LocalFileSearch::validateCriteria(condition("size", "equals", 1.5), &parseError), "fractional bytes accepted");
        require(!LocalFileSearch::validateCriteria(condition("name", "contains", ""), &parseError), "empty predicate accepted");
        require(!LocalFileSearch::validateCriteria(combine("none", {}), &parseError), "empty None matches entire tree");
        auto deep = condition("name", "contains", "invoice");
        for (int i = 0; i < 14; ++i) deep = combine("none", {deep});
        require(!LocalFileSearch::validateCriteria(deep, &parseError), "deep tree accepted");
        QJsonArray tooMany;
        for (int i = 0; i < 129; ++i) tooMany.append(condition("name", "contains", "invoice"));
        require(!LocalFileSearch::validateCriteria(combine("any", tooMany), &parseError), "unbounded criteria accepted");
        richOptions = {};
        richOptions.criteria = condition("unsupported", "equals", "x");
        result = scan(search, richUrl, {}, richOptions);
        require(!result.error.isEmpty() && result.urls.isEmpty(), "invalid search must error without scanning");
        richOptions.criteria = LocalFileSearch::parseQuery("(tag:Blue OR kind:image) AND NOT name:draft", &parseError);
        const auto restored = QJsonDocument::fromJson(QJsonDocument(richOptions.criteria).toJson()).object();
        require(restored == richOptions.criteria && LocalFileSearch::validateCriteria(restored, &parseError), "saved criteria roundtrip");
        result = scan(search, richUrl, {}, richOptions);
        require(result.count == 2, "parsed saved criteria match actual fixtures");
        cases.append("bounded invalid query/tree rejection and live JSON criteria roundtrip");

        const QByteArray previousDb = qgetenv("BALOO_DB_PATH");
        const QString missingDb = temporary.path() + "/missing-index";
        qputenv("BALOO_DB_PATH", missingDb.toUtf8());
        richOptions = {};
        richOptions.provider = LocalFileSearch::Options::Provider::PreferIndex;
        result = scan(search, richUrl, "invoice", richOptions);
        require(result.count == 1 && result.provider.value("provider") == "local" && !result.provider.value("indexAvailable").toBool() &&
            !QFileInfo::exists(missingDb), "missing index fallback created index directory");
        result = scan(search, QUrl::fromLocalFile("/"), "invoice", richOptions);
        require(!result.error.isEmpty() && result.urls.isEmpty() && result.provider.value("examinedEntries").toInt() == 0,
            "computer-wide provider fallback scanned the disk");
        richOptions.provider = LocalFileSearch::Options::Provider::IndexOnly;
        result = scan(search, richUrl, "invoice", richOptions);
        require(!result.error.isEmpty() && result.urls.isEmpty() && !QFileInfo::exists(missingDb), "index-only absence fallback");
        // Synthetic CLI contract fixture exercises the untrusted external
        // provider boundary. It is not a claim about real index extraction.
        const QByteArray previousPath = qgetenv("PATH");
        const QString bin = temporary.path() + "/provider-bin";
        const QString mockDb = temporary.path() + "/provider-db";
        fixture(mockDb + "/index", "existing synthetic index");
        fixture(bin + "/baloosearch6", R"PY(#!/usr/bin/python3
import json, os, sys, time
a = sys.argv[1:]
if a == ['--version']:
    print('Baloo 6.24.0')
elif a == ['--help']:
    print('--sort --directory --limit')
else:
    assert a[0:2] == ['--sort', 'none']
    assert a[2] == '--limit' and int(a[3]) > 0
    assert '--' in a
    assert 'DISPLAY' not in os.environ and 'WAYLAND_DISPLAY' not in os.environ
    assert '/nonexistent/' in os.environ.get('DBUS_SESSION_BUS_ADDRESS', '')
    if 'slow' in a[-1]:
        time.sleep(2)
    else:
        assert a[-1] == 'modified>="1970-01-01T00:00:00Z"'
        for path in json.load(open(os.path.join(os.path.dirname(__file__), 'paths.json'))):
            print(path)
)PY");
        require(QFile::setPermissions(bin + "/baloosearch6", QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner), "executable provider fixture");
        const QString blocked = rich + "/link-dir";
        require(QFile::link(temporary.path() + "/outside", blocked), "indexed symlink ancestor fixture");
        fixture(bin + "/paths.json", QJsonDocument(QJsonArray{rich + "/invoice.txt", rich + "/pipe.txt", rich + "/invoice.txt",
            rich + "/../outside/needle-outside.txt", temporary.path() + "/outside/needle-outside.txt", blocked + "/needle-outside.txt"}).toJson());
        qputenv("PATH", bin.toUtf8() + ':' + previousPath);
        qputenv("BALOO_DB_PATH", mockDb.toUtf8());
        result = scan(search, richUrl, "invoice", richOptions);
        require(result.error.isEmpty() && result.count == 1 && result.urls.first() == QUrl::fromLocalFile(rich + "/invoice.txt") &&
            result.provider.value("provider") == "baloo" && result.provider.value("coverage") == "existing-index-only" && result.limited,
            "provider candidates escape scope, duplicate, follow symlink ancestor, or scan special files");
        richOptions.parseKeywords = true;
        richOptions.maxElapsedMs = 100;
        result = scan(search, richUrl, "content:slow", richOptions);
        require(!result.error.isEmpty() && result.urls.isEmpty(), "provider process ignores search elapsed budget");
        qputenv("PATH", previousPath);
        require(QFile::remove(blocked), "remove indexed symlink fixture");
        if (previousDb.isNull()) qunsetenv("BALOO_DB_PATH"); else qputenv("BALOO_DB_PATH", previousDb);
        cases.append("optional provider falls back honestly without creating an index; index-only unavailable reports error");
        cases.append("provider protocol isolates desktop session, rejects out-of-scope/special/duplicate candidates, and enforces process timeout");

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
