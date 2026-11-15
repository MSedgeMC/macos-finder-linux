// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
// Diagnostic benchmark; intentionally has no absolute speed pass/fail limits.
// Private AUTOMOC C++20 target: this file and src/{browser,tags,preferences,
// scroll,appearance,batchrename,filedetails}.cpp; Qt6::Widgets, Threads::Threads,
// KF6::{KIOCore,KIOGui,KIOWidgets,KIOFileWidgets,JobWidgets,FileMetaData,Service}.
// Compile with -Wall -Wextra. Run tests/run-headless.py timeout 85s <executable>
// so the runner also cleans fixture directories on timeout. Optional argv[1]
// is the measurements JSON path.
#include "browser.h"
#include <QAbstractItemView>
#include <QAbstractEventDispatcher>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <KDirLister>
#include <KDirModel>
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <time.h>

namespace {
void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
double cpuMs() {
    timespec value{};
    require(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) == 0, "process CPU clock");
    return value.tv_sec * 1000.0 + value.tv_nsec / 1000000.0;
}
void turn() {
    QEventLoop loop;
    QTimer::singleShot(0, &loop, &QEventLoop::quit);
    loop.exec();
}
void settle(int milliseconds) {
    QEventLoop loop;
    QTimer timer;
    timer.setObjectName(QStringLiteral("finderBenchmarkTimer"));
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(milliseconds);
    loop.exec();
}
QJsonObject distribution(QList<double> values) {
    if (values.isEmpty()) return {{"samples", 0}, {"p95_ms", QJsonValue()}, {"max_ms", QJsonValue()}};
    std::sort(values.begin(), values.end());
    const auto p95 = qsizetype(std::ceil(values.size() * 0.95)) - 1;
    return {{"samples", double(values.size())}, {"p95_ms", values[p95]}, {"max_ms", values.back()}};
}
class Probe : public QObject {
public:
    QTimer heartbeat;
    QElapsedTimer clock;
    QList<double> gaps;
    double previous = 0;
    quint64 browserTimers = 0, processTimers = 0, metaCalls = 0, paints = 0, wakes = 0;
    QSet<const QObject *> objects;
    explicit Probe(Browser &browser) {
        objects.insert(&browser);
        for (auto *object : browser.findChildren<QObject *>()) objects.insert(object);
        qApp->installEventFilter(this);
        connect(QAbstractEventDispatcher::instance(), &QAbstractEventDispatcher::awake, this, [this] { ++wakes; });
        clock.start();
        heartbeat.setTimerType(Qt::PreciseTimer);
        heartbeat.setObjectName(QStringLiteral("finderBenchmarkTimer"));
        heartbeat.setInterval(10);
        connect(&heartbeat, &QTimer::timeout, this, [this] {
            const double now = clock.nsecsElapsed() / 1000000.0;
            gaps.append(now - previous);
            previous = now;
        });
        heartbeat.start();
    }
    bool eventFilter(QObject *object, QEvent *event) override {
        // Snapshot covers QObject children at construction. Embedded unparented
        // Browser timers are covered only by the process-wide timer count.
        if (event->type() == QEvent::Timer && object->objectName() != QStringLiteral("finderBenchmarkTimer")) ++processTimers;
        if (objects.contains(object)) {
            if (event->type() == QEvent::Timer) ++browserTimers;
            if (event->type() == QEvent::MetaCall) ++metaCalls;
            if (event->type() == QEvent::Paint) ++paints;
        }
        return false;
    }
    QJsonObject idle(const QString &label) {
        const auto first = gaps.size();
        const auto bt = browserTimers, pt = processTimers, mc = metaCalls, pc = paints, wc = wakes;
        const double cpu = cpuMs();
        QElapsedTimer wall; wall.start();
        settle(1000);
        const double elapsed = wall.nsecsElapsed() / 1000000.0;
        const double consumed = cpuMs() - cpu;
        return {{"label", label}, {"wall_ms", elapsed}, {"process_cpu_ms", consumed},
                {"one_core_cpu_percent", consumed / elapsed * 100.0},
                {"heartbeat", distribution(gaps.mid(first))},
                {"browser_timer_events", double(browserTimers - bt)},
                {"process_timer_events_excluding_probe", double(processTimers - pt)},
                {"browser_meta_call_events", double(metaCalls - mc)},
                {"browser_paint_events", double(paints - pc)},
                {"dispatcher_awake", double(wakes - wc)}};
    }
};
QSet<QUrl> urlSet(const QList<QUrl> &urls) { return QSet<QUrl>(urls.begin(), urls.end()); }
void sameSelection(Browser &browser, const QList<QUrl> &expected) {
    require(urlSet(browser.selectedUrls()) == urlSet(expected), "selection URLs preserved");
}
QJsonObject operation(const QString &name, Probe &probe, const std::function<void()> &action,
                      const std::function<void()> &verify) {
    turn(); // drain work from the preceding untimed correctness scan
    const auto first = probe.gaps.size();
    QElapsedTimer elapsed; elapsed.start();
    action();
    const double dispatch = elapsed.nsecsElapsed() / 1000000.0;
    turn();
    const double queued = elapsed.nsecsElapsed() / 1000000.0;
    // Delayed layout/metadata activity belongs to the operation's heartbeat
    // window, but this fixed settle delay is excluded from operation latency.
    settle(50);
    verify();
    return {{"operation", name}, {"dispatch_ms", dispatch}, {"queued_turn_ms", queued},
            {"heartbeat", distribution(probe.gaps.mid(first))}};
}
void verifySort(Browser &browser, int column, bool ascending, int count) {
    auto *model = browser.activeView()->model();
    require(model->rowCount() == count, "all sorted rows present");
    KFileItem previous;
    for (int row = 0; row < count; ++row) {
        const auto item = model->index(row, 0).data(KDirModel::FileItemRole).value<KFileItem>();
        require(!item.isNull(), "native file item in sorted model");
        if (!previous.isNull()) {
            bool ordered = true;
            if (column == Browser::NameColumn) ordered = ascending ? previous.name() <= item.name() : previous.name() >= item.name();
            if (column == Browser::SizeColumn) ordered = ascending ? previous.size() <= item.size() : previous.size() >= item.size();
            if (column == Browser::DateModifiedColumn) ordered = ascending ? previous.time(KFileItem::ModificationTime) <= item.time(KFileItem::ModificationTime) : previous.time(KFileItem::ModificationTime) >= item.time(KFileItem::ModificationTime);
            require(ordered, "sort order matches requested column and direction");
        }
        previous = item;
    }
}
QJsonObject benchmark(const QString &path, int count) {
    Browser browser;
    browser.resize(1000, 650);
    browser.directoryModel()->dirLister()->setAutoErrorHandlingEnabled(false);
    browser.setMode(Browser::List);
    browser.setIconPreviewsShown(false);
    browser.show();
    turn();
    Probe probe(browser);
    QString error;
    QObject::connect(&browser, &Browser::errorOccurred, &browser, [&](const QString &message) { error = message; });
    QEventLoop listing;
    bool complete = false, timedOut = false;
    QTimer timeout; timeout.setSingleShot(true); timeout.setObjectName(QStringLiteral("finderBenchmarkTimer"));
    QObject::connect(&timeout, &QTimer::timeout, &listing, [&] { timedOut = true; listing.quit(); });
    const auto completion = QObject::connect(browser.directoryModel()->dirLister(), qOverload<>(&KDirLister::completed),
        &listing, [&] { complete = true; listing.quit(); });
    QElapsedTimer elapsed; elapsed.start();
    timeout.start(15000); // hang containment, not a performance target
    browser.navigate(QUrl::fromLocalFile(path));
    const double navigate = elapsed.nsecsElapsed() / 1000000.0;
    if (!complete) listing.exec();
    const double listed = elapsed.nsecsElapsed() / 1000000.0;
    timeout.stop(); QObject::disconnect(completion);
    require(!timedOut && complete && error.isEmpty(), "native KIO listing completed without error");
    require(browser.itemCount() == count && browser.directoryModel()->rowCount() == count, "exact native listing count");
    turn();
    const double queued = elapsed.nsecsElapsed() / 1000000.0;
    settle(250);
    const auto listingHeartbeat = distribution(probe.gaps);
    QList<QUrl> single{QUrl::fromLocalFile(QDir(path).filePath("item00000.txt"))}, batch;
    for (int i = 0; i < 64; ++i) batch.append(QUrl::fromLocalFile(QDir(path).filePath(QString("item%1.txt").arg(i * (count - 1) / 63, 5, 10, QChar('0')))));
    QJsonArray actions;
    const QStringList names{"icons", "list", "columns", "gallery"};
    for (int mode = Browser::Icons; mode <= Browser::Gallery; ++mode) {
        browser.setMode(Browser::Mode(mode)); browser.setIconPreviewsShown(false); settle(50);
        for (int column : {int(Browser::NameColumn), int(Browser::SizeColumn), int(Browser::DateModifiedColumn)}) {
            for (bool ascending : {false, true}) {
                actions.append(operation(names[mode] + QString("/sort-%1-%2").arg(column).arg(ascending ? "asc" : "desc"), probe,
                    [&] { browser.setSort(column, ascending); }, [&] { verifySort(browser, column, ascending, count); }));
            }
        }
        for (const auto &urls : {single, batch}) {
            actions.append(operation(names[mode] + QString("/select-%1").arg(urls.size()), probe,
                [&] { browser.selectUrls(urls); }, [&] { sameSelection(browser, urls); }));
        }
    }
    // Every directed transition, with a nonempty 64-URL selection to exercise
    // the distinct column selection model and retained per-mode options.
    for (int from = Browser::Icons; from <= Browser::Gallery; ++from) {
        browser.setMode(Browser::Mode(from)); browser.setIconPreviewsShown(false);
        browser.selectUrls(batch); settle(50);
        for (int to = Browser::Icons; to <= Browser::Gallery; ++to) {
            if (from == to) continue;
            browser.setMode(Browser::Mode(from)); settle(50);
            actions.append(operation(names[from] + "->" + names[to], probe,
                [&] { browser.setMode(Browser::Mode(to)); }, [&] {
                    require(browser.mode() == Browser::Mode(to) && browser.itemCount() == count, "mode and count retained");
                    sameSelection(browser, batch);
                }));
        }
    }
    browser.setMode(Browser::Icons); browser.selectUrls({}); browser.setIconPreviewsShown(false); settle(500);
    QJsonArray idle;
    idle.append(probe.idle("visible_icons_previews_disabled"));
    browser.setIconPreviewsShown(true); settle(1000);
    idle.append(probe.idle("visible_icons_previews_enabled"));
    require(error.isEmpty(), "no native Browser errors during workload");
    return {{"files", count}, {"navigate_dispatch_ms", navigate}, {"listing_completed_ms", listed},
            {"listing_queued_turn_ms", queued}, {"listing_heartbeat", listingHeartbeat},
            {"operations", actions}, {"idle", idle}, {"whole_case_heartbeat", distribution(probe.gaps)},
            {"checks", "exact listing; all sorted rows; requested selection; all 12 directed modes"}};
}
}

int main(int argc, char **argv) {
    QJsonObject result{{"schema", 1}, {"status", "failed"}, {"utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {"qt_version", QT_VERSION_STR}, {"heartbeat_interval_ms", 10},
        {"method", "Qt Widgets offscreen native Browser; private session; recently-created warm local tiny files; one run per size"},
        {"latency_definition", "dispatch: synchronous API call; queued_turn: call through next zero-timer event turn, not compositor presentation; listing: KDirLister completed, not metadata completion"},
        {"heartbeat_definition", "actual 10ms QTimer delivery gaps; nearest-rank p95; includes workload, correctness scans and idle, no sleeps/polling; operation windows can include interval crossing a boundary"},
        {"idle_definition", "1s process CPU (all process threads, excludes KIO helpers), one-core percent; process timers exclude probe and settle timers; Browser-object snapshot misses unparented and later-created objects; dispatcher wakes and CPU include benchmark heartbeat"}};
    int status = 1;
    const QString output = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    try {
        require(qgetenv("QT_QPA_PLATFORM") == "offscreen" && !qEnvironmentVariableIsSet("DISPLAY") &&
            !qEnvironmentVariableIsSet("WAYLAND_DISPLAY") && qEnvironmentVariableIsSet("DBUS_SESSION_BUS_ADDRESS"), "private headless runner required");
        for (const auto *key : {"XDG_RUNTIME_DIR", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME"})
            require(qEnvironmentVariableIsSet(key), "isolated writable XDG directories required");
        QApplication app(argc, argv);
        app.setOrganizationName("FinderPrivatePerformance"); app.setApplicationName("BrowserPerformance");
        QElapsedTimer total; total.start();
        QTemporaryDir fixture(QDir(qEnvironmentVariable("XDG_CACHE_HOME")).filePath("finder-performance-XXXXXX"));
        require(fixture.isValid(), "temporary fixture root");
        const QString cleanupPath = fixture.path();
        QJsonArray cases;
        for (int count : {1000, 10000}) {
            const QString folder = fixture.filePath(QString::number(count));
            require(QDir().mkdir(folder), "fixture folder");
            QElapsedTimer creation; creation.start();
            for (int i = count - 1; i >= 0; --i) {
                QFile file(QDir(folder).filePath(QString("item%1.txt").arg(i, 5, 10, QChar('0'))));
                const QByteArray bytes(1 + (i * 17) % 31, 'x');
                require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(bytes) == bytes.size(), "tiny fixture creation");
                require(file.setFileTime(QDateTime::fromSecsSinceEpoch(1700000000 + i % 97), QFileDevice::FileModificationTime), "fixture modification time");
            }
            const double created = creation.nsecsElapsed() / 1000000.0;
            auto measurement = benchmark(folder, count);
            measurement.insert("fixture_creation_ms", created);
            cases.append(measurement);
            result.insert("cases", cases);
            require(total.elapsed() < 65000, "bounded benchmark elapsed time");
        }
        require(fixture.remove() && !QDir(cleanupPath).exists(), "bounded fixture cleanup");
        result.insert("fixture_cleanup", true);
        result.insert("total_ms", total.nsecsElapsed() / 1000000.0);
        result.insert("status", "passed"); status = 0;
    } catch (const std::exception &error) {
        result.insert("error", QString::fromUtf8(error.what()));
        std::cerr << "FAIL: " << error.what() << '\n';
    }
    const QByteArray json = QJsonDocument(result).toJson(QJsonDocument::Indented);
    if (!output.isEmpty()) {
        QSaveFile file(output);
        if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size() || !file.commit()) {
            std::cerr << "FAIL: measurements output\n"; status = 1;
        }
    } else std::cout << json.constData();
    if (!status) std::cerr << "PASS native Browser performance correctness and cleanup (diagnostic timings)\n";
    return status;
}
