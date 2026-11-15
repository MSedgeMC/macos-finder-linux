// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
// Standalone target: browser.cpp/.h, tags.cpp and preferences.cpp; AUTOMOC;
// Qt6::Widgets, KF6::KIOCore/KIOGui/KIOWidgets/KIOFileWidgets/ItemViews/FileMetaData.
// Run through tests/run-headless.py dbus-run-session -- <executable> so KIO
// listing uses a private session bus as well as private XDG/offscreen paths.
#include "browser.h"
#include "tags.h"
#include <QAbstractItemView>
#include <QAbstractProxyModel>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QTreeView>
#include <KDirModel>
#include <functional>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace {
void check(bool value, const char *message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
bool waitFor(const std::function<bool()> &condition, int timeout = 10000) {
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < timeout) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return condition();
}
void writeFile(const QString &path, int size) {
    QFile file(path);
    check(file.open(QIODevice::WriteOnly) && file.write(QByteArray(size, 'x')) == size, "file fixture");
}
QModelIndex findUrl(QAbstractItemModel *model, const QUrl &url, const QModelIndex &parent = {}) {
    for (int row = 0; row < model->rowCount(parent); ++row) {
        const auto index = model->index(row, 0, parent);
        if (index.data(KDirModel::FileItemRole).value<KFileItem>().url() == url) return index;
    }
    return {};
}
}

int main(int argc, char **argv) {
    check(qgetenv("QT_QPA_PLATFORM") == "offscreen" && qEnvironmentVariableIsSet("XDG_CONFIG_HOME") &&
          qEnvironmentVariableIsSet("DBUS_SESSION_BUS_ADDRESS"), "must use private offscreen runner and session bus");
    qunsetenv("DISPLAY"); qunsetenv("WAYLAND_DISPLAY"); qunsetenv("SESSION_MANAGER");
    QApplication app(argc, argv);
    app.setOrganizationName("FinderBrowserPrivateTest"); app.setApplicationName("BrowserMetadata");
    QTemporaryDir files;
    check(files.isValid(), "fixture directory");
    check(QDir().mkpath(files.filePath("folder-a/sub")) && QDir().mkpath(files.filePath("folder-z")), "folder fixtures");
    writeFile(files.filePath("small.txt"), 3);
    writeFile(files.filePath("large.txt"), 19);
    writeFile(files.filePath("folder-a/first"), 7);
    writeFile(files.filePath("folder-a/.hidden"), 5);
    writeFile(files.filePath("folder-a/sub/deep"), 11);
    writeFile(files.filePath("folder-z/other"), 31);
    check(::symlink("..", QFile::encodeName(files.filePath("folder-a/loop")).constData()) == 0, "symlink loop fixture");
    check(::mkfifo(QFile::encodeName(files.filePath("folder-a/fifo")).constData(), 0600) == 0, "special file fixture");
    const auto root = QUrl::fromLocalFile(files.path());
    const auto small = QUrl::fromLocalFile(files.filePath("small.txt"));
    const auto large = QUrl::fromLocalFile(files.filePath("large.txt"));
    const auto folder = QUrl::fromLocalFile(files.filePath("folder-a"));
    const auto otherFolder = QUrl::fromLocalFile(files.filePath("folder-z"));
    const bool tagsSupported = FileTags::setTags(small, {"Red", "Project"}).isEmpty() &&
        FileTags::setTags(large, {"Blue"}).isEmpty();
    Browser browser;
    browser.resize(900, 500);
    browser.setMode(Browser::List);
    browser.setIconPreviewsShown(false);
    browser.show();
    browser.navigate(root);
    check(waitFor([&] { return browser.itemCount() == 4; }), "native async listing");
    auto *model = browser.activeView()->model();
    auto *tree = qobject_cast<QTreeView *>(browser.activeView());
    check(tree && browser.listColumnCount() == Browser::ListColumnCount && browser.directoryModel()->columnCount() == Browser::ListColumnCount,
          "extended model keeps native directory API");
    check(browser.visibleListColumns() == QList<int>{Browser::NameColumn, Browser::SizeColumn, Browser::DateModifiedColumn, Browser::KindColumn},
          "original default visible columns preserved");
    browser.setListColumnVisible(Browser::DateCreatedColumn, true);
    browser.setListColumnVisible(Browser::TagsColumn, true);
    browser.setListColumnVisible(Browser::NameColumn, false);
    check(browser.listColumnVisible(Browser::NameColumn) && browser.listColumnVisible(Browser::DateCreatedColumn), "visibility API retains Name");
    check(waitFor([&] { return !browser.metadata(small, Browser::MetadataPendingRole).toBool(); }), "birth metadata delivered");
    const auto birth = QFileInfo(small.toLocalFile()).birthTime();
    const auto cachedBirth = browser.metadata(small, Browser::DateCreatedRole);
    check(birth.isValid() ? cachedBirth.toDateTime() == birth : !cachedBirth.isValid(), "only real filesystem birth time");
    check(!browser.metadata(small, Browser::DateAddedRole).isValid() && !browser.metadata(small, Browser::LastOpenedRole).isValid(),
          "unprovided added/opened dates remain unavailable");
    const auto smallIndex = findUrl(model, small);
    check(smallIndex.siblingAtColumn(Browser::DateAddedColumn).data().toString() == QString::fromUtf8("—"), "unavailable date display");
    check(smallIndex.siblingAtColumn(Browser::DateAddedColumn).data(Qt::ToolTipRole).toString().contains("provider"), "unavailable date explanation");
    check(browser.metadata(small, Browser::LogicalSizeRole).toULongLong() == 3, "raw file size");
    browser.selectUrls({small, large});
    check(browser.selectedUrls().size() == 2, "selection mapping with extra columns");
    browser.setSort(Browser::SizeColumn, false);
    browser.setIconSize(32);
    check(tree->header()->sortIndicatorSection() == Browser::SizeColumn && tree->header()->sortIndicatorOrder() == Qt::DescendingOrder,
          "setSort synchronizes native header");
    check(model->index(0, 0).data(KDirModel::FileItemRole).value<KFileItem>().url() == large, "numeric descending file-size sort");
    auto saved = QJsonDocument::fromJson(browser.saveViewState()).object();
    tree->header()->setSortIndicator(Browser::NameColumn, Qt::DescendingOrder);
    saved["listHeader"] = QString::fromLatin1(tree->header()->saveState().toBase64());
    browser.restoreViewState(QJsonDocument(saved).toJson());
    check(browser.sortColumn() == Browser::SizeColumn && !browser.sortAscending() &&
          tree->header()->sortIndicatorSection() == Browser::SizeColumn && browser.iconSize() == 32 &&
          browser.listColumnVisible(Browser::TagsColumn), "saved sort overrides stale header without losing columns or icon size");
    if (tagsSupported) {
        check(waitFor([&] { return browser.metadata(small, Browser::TagsRole).toStringList().size() == 2; }), "async real tags");
        const auto colors = browser.metadata(small, Browser::TagColorsRole).toList();
        check(colors.size() == 2 && colors.first().value<QColor>() == FileTags::tagColor("Red"), "tag color roles parallel actual values");
        browser.setSort(Browser::TagsColumn, true);
        check(waitFor([&] { return findUrl(model, large).row() < findUrl(model, small).row(); }), "tags sortable by cached values");
        check(FileTags::setTags(small, {"Yellow"}).isEmpty(), "tag update");
        browser.refreshTags();
        check(waitFor([&] { return browser.metadata(small, Browser::TagsRole).toStringList() == QStringList{"Yellow"}; }), "tag cache refresh rejects stale batches");
    }
    // Native expanded children and proxy round trips stay valid with appended columns.
    auto folderIndex = findUrl(model, folder);
    model->fetchMore(folderIndex);
    check(waitFor([&] { return model->rowCount(findUrl(model, folder)) >= 4; }), "expanded native child listing");
    folderIndex = findUrl(model, folder);
    const auto child = model->index(0, Browser::DateCreatedColumn, folderIndex);
    auto *proxy = qobject_cast<QAbstractProxyModel *>(model);
    check(child.isValid() && child.parent() == folderIndex && proxy->mapFromSource(proxy->mapToSource(child)) == child,
          "extended nested indexes preserve proxy and parent mapping");
    int finishCount = 0; bool canceled = false, limited = false;
    QObject::connect(&browser, &Browser::sizeCalculationFinished, [&](bool c, bool l, const QString &) { ++finishCount; canceled = c; limited = l; });
    int heartbeatCount = 0;
    QTimer heartbeat; heartbeat.setInterval(1);
    QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeatCount; });
    heartbeat.start();
    QElapsedTimer dispatch; dispatch.start();
    browser.calculateAllSizes();
    check(dispatch.elapsed() < 200 && browser.sizeCalculationRunning(), "explicit sizes dispatch nonblocking");
    check(waitFor([&] { return !browser.sizeCalculationRunning(); }), "size worker completion");
    check(finishCount == 1 && !canceled && !limited && browser.metadata(folder, Browser::LogicalSizeRole).toULongLong() == 23 &&
          browser.metadata(otherFolder, Browser::LogicalSizeRole).toULongLong() == 31, "nested logical sizes include hidden files and exclude loop/fifo");
    browser.setSort(Browser::SizeColumn, false);
    check(model->index(0, 0).data(KDirModel::FileItemRole).value<KFileItem>().url() == otherFolder, "calculated folder sizes participate in sort");
    browser.calculateAllSizes({1, 64, 15000});
    check(waitFor([&] { return !browser.sizeCalculationRunning(); }), "bounded entry worker completion");
    check(limited && !browser.metadata(folder, Browser::LogicalSizeRole).isValid() &&
          browser.metadata(folder, Browser::DirectorySizeStateRole).toInt() == Browser::SizeIncomplete,
          "partial totals never claim complete logical sizes");
    browser.calculateAllSizes({250000, 0, 15000});
    check(waitFor([&] { return !browser.sizeCalculationRunning(); }), "depth limit worker completion");
    check(limited && browser.metadata(folder, Browser::CalculatedBytesRole).isValid(), "depth limit exposes marked partial bytes");
    browser.calculateAllSizes();
    browser.cancelSizeCalculation();
    check(waitFor([&] { return !browser.sizeCalculationRunning(); }) && canceled, "asynchronous cancellation");
    browser.calculateAllSizes();
    browser.navigate(otherFolder);
    check(waitFor([&] { return !browser.sizeCalculationRunning() && browser.itemCount() == 1; }) && canceled,
          "navigation cancels and rejects stale results");
    check(!browser.metadata(folder, Browser::LogicalSizeRole).isValid(), "old folder cannot receive stale size");
    // Repeated reads use already-delivered caches; no metadata queue/timer is started by paint roles.
    for (int i = 0; i < 1000; ++i) browser.metadata(QUrl::fromLocalFile(files.filePath("folder-z/other")), Browser::DateCreatedRole);
    browser.hide();
    std::cout << "PASS: Browser metadata, List state, proxy mapping, real tags, bounded sizes and cancellation" <<
        (tagsSupported ? "\n" : " (xattr tags unavailable on fixture filesystem)\n");
    return 0;
}
