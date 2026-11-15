// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
// Standalone AUTOMOC target: this file + src/searchresults.{cpp,h}.
// Link Qt6::Widgets, KF6::KIOCore, KF6::KIOGui. Run via run-headless.py.
#include "searchresults.h"

#include <KIO/PreviewJob>
#include <QAbstractItemView>
#include <QApplication>
#include <QContextMenuEvent>
#include <QElapsedTimer>
#include <QIcon>
#include <QImage>
#include <QItemSelectionModel>
#include <QLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListView>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointer>
#include <QSortFilterProxyModel>
#include <QSet>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QTreeView>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>

namespace {
QStringList warnings;
std::mutex warningMutex;
void messages(QtMsgType type, const QMessageLogContext &, const QString &message)
{
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) {
        const std::lock_guard guard(warningMutex);
        warnings.append(message);
    }
    if (type == QtFatalMsg) std::abort();
}
void check(bool value, const char *message)
{
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
bool waitFor(const std::function<bool()> &condition, int timeout = 5000)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < timeout) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return condition();
}
void pump(int ms = 20)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < ms) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
}
void key(QAbstractItemView *view, int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QKeyEvent press(QEvent::KeyPress, code, modifiers);
    QApplication::sendEvent(view, &press);
    QKeyEvent release(QEvent::KeyRelease, code, modifiers);
    QApplication::sendEvent(view, &release);
    pump();
}
void click(QAbstractItemView *view, const QModelIndex &index, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    view->scrollTo(index);
    pump();
    const QPointF point(view->visualRect(index).center());
    check(view->viewport()->rect().contains(point.toPoint()), "isolated click hits visible row");
    QMouseEvent press(QEvent::MouseButtonPress, point, view->viewport()->mapToGlobal(point.toPoint()),
        Qt::LeftButton, Qt::LeftButton, modifiers);
    QApplication::sendEvent(view->viewport(), &press);
    QMouseEvent release(QEvent::MouseButtonRelease, point, view->viewport()->mapToGlobal(point.toPoint()),
        Qt::LeftButton, Qt::NoButton, modifiers);
    QApplication::sendEvent(view->viewport(), &release);
    pump();
}
void fill(QStandardItemModel &model, const QList<QUrl> &urls)
{
    model.setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Kind"), QStringLiteral("Location")});
    for (const auto &url : urls) {
        QPixmap fallback(20, 20); fallback.fill(Qt::magenta);
        auto *name = new QStandardItem(QIcon(fallback), url.fileName());
        name->setData(url, Qt::UserRole);
        auto *kind = new QStandardItem(QStringLiteral("Image"));
        auto *location = new QStandardItem(url.adjusted(QUrl::RemoveFilename).toDisplayString());
        model.appendRow({name, kind, location});
    }
}
QUrl rowUrl(SearchResults &results, int row)
{
    return results.proxyModel()->index(row, 0).data(Qt::UserRole).toUrl();
}
QColor thumbnailColor(SearchResults &results, int row)
{
    const auto icon = results.proxyModel()->index(row, 0).data(Qt::DecorationRole).value<QIcon>();
    const auto image = icon.pixmap(64, 64).toImage();
    return image.isNull() ? QColor() : image.pixelColor(image.width() / 2, image.height() / 2);
}
void saveImage(const QString &path, const QColor &color)
{
    QImage image(80, 80, QImage::Format_ARGB32_Premultiplied); image.fill(color);
    check(image.save(path), "thumbnail fixture saved");
}
}

int main(int argc, char **argv)
{
    // A parent window suppresses offscreen's unsupported top-level layout-hint
    // propagation; all input is sent directly inside this private Qt session.
    QApplication app(argc, argv);
    qInstallMessageHandler(messages);
    QTemporaryDir fixture;
    check(fixture.isValid(), "temporary fixture");
    QList<QUrl> urls;
    for (const QString &name : {QStringLiteral("zeta one.png"), QStringLiteral("alpha#.png"),
                               QStringLiteral("same.png"), QStringLiteral("beta-雪.png")})
        urls.append(QUrl::fromLocalFile(fixture.filePath(name)));
    for (const auto &url : std::as_const(urls)) saveImage(url.toLocalFile(), Qt::red);
    // Distinct exact URLs with the same display name, including remote URLs.
    urls.append(QUrl(QStringLiteral("sftp://example.invalid/other/same.png")));
    urls.append(QUrl(QStringLiteral("sftp://example.invalid/another/same.png")));
    QStandardItemModel model;
    fill(model, urls);
    QWidget window;
    window.resize(900, 600);
    SearchResults results(&window);
    results.setGeometry(0, 0, 900, 600);
    results.setModel(&model);
    auto *selection = results.selectionModel();
    check(results.sourceModel() == &model && results.proxyModel()->sourceModel() == &model, "borrowed source model exposed");
    check(results.primaryView()->objectName() == QStringLiteral("finderSearchResults"), "legacy primary view discoverable");
    check(results.primaryView()->model() == results.proxyModel(), "compatibility tree uses shared proxy");
    check(results.proxyModel()->headerData(2, Qt::Horizontal).toString() == QStringLiteral("Location"), "source headers preserved");
    check(model.item(0)->text() == QStringLiteral("zeta one.png"), "proxy sorting leaves source order alone");
    check(rowUrl(results, 0) == urls[1], "initial name sort shared by all modes");

    int selectionSignals = 0, renameSignals = 0, activatedSignals = 0, contextSignals = 0;
    QList<QUrl> quickLook;
    QUrl activated;
    QPoint context;
    QObject::connect(&results, &SearchResults::selectionChanged, &app, [&] { ++selectionSignals; });
    QObject::connect(&results, &SearchResults::renameRequested, &app, [&] { ++renameSignals; });
    QObject::connect(&results, &SearchResults::quickLookRequested, &app, [&](const QList<QUrl> &selectionUrls) { quickLook = selectionUrls; });
    QObject::connect(&results, &SearchResults::activated, &app, [&](const QUrl &url) { activated = url; ++activatedSignals; });
    QObject::connect(&results, &SearchResults::contextMenuRequested, &app, [&](const QPoint &global) { context = global; ++contextSignals; });
    results.setUrls({urls[4], urls[0], urls[4], QUrl(QStringLiteral("file:///not-a-hit"))}, urls[4]);
    check(results.selectedUrls().size() == 2 && results.selectedUrls().contains(urls[4]), "URL selection exact/deduplicated");
    check(selection->currentIndex().data(Qt::UserRole).toUrl() == urls[4], "explicit primary selection");
    const auto beforeSort = results.selectedUrls();
    results.sort(2, Qt::DescendingOrder);
    const auto afterSort = results.selectedUrls();
    check(QSet<QUrl>(beforeSort.cbegin(), beforeSort.cend()) == QSet<QUrl>(afterSort.cbegin(), afterSort.cend()), "sort preserves exact selection");
    results.sort(0);
    window.show();
    pump();

    for (int mode = 0; mode < 4; ++mode) {
        results.setMode(mode);
        auto *view = results.activeView();
        check(view->selectionModel() == selection && view->model() == results.proxyModel(), "all modes share proxy and selection");
        check(results.mode() == mode, "mode accepted");
        view->setFocus();
        click(view, results.proxyModel()->index(0, 0));
        click(view, results.proxyModel()->index(2, 0), Qt::MetaModifier);
        check(results.selectedUrls() == QList<QUrl>({rowUrl(results, 0), rowUrl(results, 2)}), "Meta-click toggles exact rows");
        click(view, results.proxyModel()->index(2, 0), Qt::MetaModifier);
        check(results.selectedUrls() == QList<QUrl>({rowUrl(results, 0)}), "Meta-click deselects");
        click(view, results.proxyModel()->index(1, 0));
        click(view, results.proxyModel()->index(3, 0), Qt::ShiftModifier);
        check(results.selectedUrls() == QList<QUrl>({rowUrl(results, 1), rowUrl(results, 2), rowUrl(results, 3)}), "Shift-click range");
        click(view, results.proxyModel()->index(1, 0));
        const int arrow = mode == SearchResults::Gallery || mode == SearchResults::Icons ? Qt::Key_Right : Qt::Key_Down;
        key(view, arrow, Qt::ShiftModifier);
        check(results.selectedUrls() == QList<QUrl>({rowUrl(results, 1), rowUrl(results, 2)}), "Shift keyboard range");
        check(selection->currentIndex().data(Qt::UserRole).toUrl() == rowUrl(results, 2), "keyboard range primary follows endpoint");
        key(view, Qt::Key_A, Qt::MetaModifier);
        check(results.selectedUrls().size() == urls.size(), "Meta-A selects all");
        results.setUrls({urls[5]}, urls[5]);
        key(view, Qt::Key_Space);
        check(quickLook == QList<QUrl>{urls[5]}, "Space requests exact URL Quick Look");
        const int previousRename = renameSignals;
        key(view, Qt::Key_Return);
        check(renameSignals == previousRename + 1, "Return requests host rename");
        key(view, Qt::Key_Return, Qt::MetaModifier);
        check(renameSignals == previousRename + 1, "modified Return does not rename");
        auto hit = results.proxyModel()->index(0, 0);
        view->scrollTo(hit); pump();
        const auto point = view->visualRect(hit).center();
        QContextMenuEvent menu(QContextMenuEvent::Mouse, point, view->viewport()->mapToGlobal(point));
        QApplication::sendEvent(view->viewport(), &menu);
        check(contextSignals == mode + 1 && context == view->viewport()->mapToGlobal(point), "native context coordinates global");
        check(results.selectedUrls() == QList<QUrl>{rowUrl(results, 0)}, "context menu selects exact clicked hit");
        check(QMetaObject::invokeMethod(view, "doubleClicked", Qt::DirectConnection, Q_ARG(QModelIndex, hit)), "native view activation invoked");
        check(activatedSignals == mode + 1 && activated == rowUrl(results, 0), "activation forwarded by exact URL");
        std::unique_ptr<QMimeData> mime(results.mimeData());
        check(mime->urls() == results.selectedUrls() && mime->hasFormat(QStringLiteral("text/uri-list")), "native URL MIME drag");
        check(!mime->hasFormat(QStringLiteral("application/x-qabstractitemmodeldatalist")), "drag is portable URL data");
        check(results.proxyModel()->flags(hit).testFlag(Qt::ItemIsDragEnabled), "source non-draggable items gain URL drag");
        check(!results.proxyModel()->flags(hit).testFlag(Qt::ItemIsDropEnabled), "search hit drop mutation stays host-owned");
    }
    check(selectionSignals > 0, "shared selection notifications forwarded");
    results.setMode(-1); results.setMode(4);
    check(results.mode() == SearchResults::Gallery, "invalid modes ignored");
    check(results.galleryHost() == results.host() && results.host()->layout(), "Gallery host already laid out");
    QWidget embedded(results.host());
    results.host()->layout()->addWidget(&embedded);
    results.setMode(SearchResults::Columns);
    results.setUrls({urls[5]}, urls[5]);
    check(results.activeView()->model()->rowCount() == urls.size(), "Columns remains flat search hits");
    const auto *location = results.findChild<QLabel *>(QStringLiteral("finderSearchLocationUrl"));
    check(location && location->text() == urls[5].toDisplayString(QUrl::RemovePassword), "Columns detail exposes real location URL");
    {
        QContextMenuEvent menu(QContextMenuEvent::Keyboard, {-1, -1}, {-1, -1});
        QApplication::sendEvent(results.activeView(), &menu);
        const auto current = selection->currentIndex();
        check(contextSignals == 5 && context == results.activeView()->viewport()->mapToGlobal(results.activeView()->visualRect(current).center()), "keyboard context uses current hit globally");
        check(results.selectedUrls() == QList<QUrl>{urls[5]}, "keyboard context preserves selection");
    }
    click(results.activeView(), results.proxyModel()->index(1, 0));
    results.setMode(SearchResults::List);
    key(results.activeView(), Qt::Key_Down, Qt::ShiftModifier);
    check(results.selectedUrls() == QList<QUrl>({rowUrl(results, 1), rowUrl(results, 2)}), "range anchor survives mode switch");
    {
        const auto names = results.selectedUrls();
        results.primaryView()->sortByColumn(0, Qt::DescendingOrder);
        const auto sorted = results.selectedUrls();
        check(QSet<QUrl>(names.cbegin(), names.cend()) == QSet<QUrl>(sorted.cbegin(), sorted.cend()), "compatibility tree sorting preserves all mode selection");
    }
    results.setUrls({});
    check(!selection->currentIndex().isValid() && results.selectedUrls().isEmpty(), "empty URL selection clears current");
    {
        QStandardItemModel appended;
        fill(appended, {urls[0], urls[1]});
        results.setModel(&appended);
        results.sort(0);
        results.setUrls({urls[0]});
        fill(appended, {urls[2]});
        check(results.selectedUrls() == QList<QUrl>{urls[0]} && results.proxyModel()->rowCount() == 3, "sorted appends preserve URL selection");
        appended.item(0)->setText(QStringLiteral("aardvark"));
        check(results.selectedUrls() == QList<QUrl>{urls[0]} && rowUrl(results, 0) == urls[0], "source data change resorts exact selected URL");
        std::unique_ptr<QMimeData> mime(results.proxyModel()->mimeData({results.proxyModel()->index(0, 0), results.proxyModel()->index(0, 1), results.proxyModel()->index(0, 2)}));
        check(mime->urls() == QList<QUrl>{urls[0]}, "multi-column MIME deduplicated");
        appended.removeRow(0);
        check(results.selectedUrls().isEmpty(), "source removal clears removed URL selection");
        appended.clear();
        check(results.selectedUrls().isEmpty() && !selection->currentIndex().isValid(), "source reset clears selection/current");
    }

    auto replacement = std::make_unique<QStandardItemModel>();
    fill(*replacement, {urls[2]});
    results.setModel(replacement.get());
    check(results.selectionModel() == selection && results.selectedUrls().isEmpty(), "model replacement keeps selection object and clears hits");
    model.clear(); // previously borrowed model can change without affecting us
    check(results.proxyModel()->rowCount() == 1, "old model disconnected");
    results.setUrls({urls[2]});
    replacement.reset();
    pump();
    check(results.sourceModel() == nullptr && results.proxyModel()->rowCount() == 0 && results.selectedUrls().isEmpty(), "destroyed borrowed model safely detached");
    results.setModel(nullptr);

    // Exercise real installed thumbnail plugins and source generation switches.
    // A bounded batch may finish after switching: it must never decorate a hit
    // of the replacement model just because the row number happens to match.
    const auto green = QUrl::fromLocalFile(fixture.filePath(QStringLiteral("green.png")));
    const auto blue = QUrl::fromLocalFile(fixture.filePath(QStringLiteral("blue.png")));
    saveImage(green.toLocalFile(), Qt::green);
    saveImage(blue.toLocalFile(), Qt::blue);
    QStandardItemModel images, newImages;
    fill(images, {green}); fill(newImages, {blue});
    results.setModel(&images);
    results.setMode(SearchResults::Icons);
    if (!KIO::PreviewJob::defaultPlugins().isEmpty()) {
        pump(60); // first visible batch dispatched; replace while it may run
        results.setModel(&newImages);
        check(waitFor([&] { return thumbnailColor(results, 0).blue() > 200 && thumbnailColor(results, 0).red() < 30; }, 15000), "native plugin thumbnail reaches new generation");
        int decorationUpdates = 0;
        const auto updates = QObject::connect(results.proxyModel(), &QAbstractItemModel::dataChanged, &app,
            [&](const QModelIndex &, const QModelIndex &, const QList<int> &roles) {
                if (roles.contains(Qt::DecorationRole)) ++decorationUpdates;
            });
        pump(120);
        check(decorationUpdates == 0, "static thumbnail cached without refresh loop");
        QObject::disconnect(updates);
        results.setMode(SearchResults::Gallery);
        check(thumbnailColor(results, 0).blue() > 200 && thumbnailColor(results, 0).red() < 30, "thumbnail cache shared across modes");
        results.refreshThumbnails();
        check(waitFor([&] { return thumbnailColor(results, 0).blue() > 200 && thumbnailColor(results, 0).red() < 30; }, 15000), "explicit thumbnail refresh");
    }
    results.setModel(&images);
    for (int i = 0; i < 4; ++i) {
        auto pending = std::make_unique<SearchResults>(&window);
        pending->setGeometry(0, 0, 400, 300);
        pending->setModel(&images);
        pending->setMode(SearchResults::Icons);
        pending->show();
        pump(45);
        pending.reset(); // no late callback may dereference its Private
    }
    results.hide();
    pump(300);
    {
        const std::lock_guard guard(warningMutex);
        for (const auto &warning : warnings) std::cerr << "Qt warning: " << warning.toStdString() << '\n';
        check(warnings.isEmpty(), "isolated native search-results tests warning-clean");
    }
    std::cout << "PASS: search-results modes, URL identity, selection, sort, MIME, context, generations and lifecycle\n";
}
