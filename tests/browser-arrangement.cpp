// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
// Same standalone sources/libraries as browser-metadata.cpp. Run exclusively
// through run-headless.py: private offscreen platform, session bus and XDG dirs.
#include "browser.h"
#include <QAbstractItemView>
#include <QApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QListView>
#include <QMimeData>
#include <QScrollBar>
#include <QSet>
#include <QTemporaryDir>
#include <QThread>
#include <KDirModel>
#include <KDirLister>
#include <KIO/FileUndoManager>
#include <functional>
#include <iostream>

namespace {
void check(bool value, const char *message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
bool waitFor(const std::function<bool()> &condition, int timeout = 10000) {
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < timeout) {
        QApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(1);
    }
    return condition();
}
void settle(int milliseconds = 180) {
    QElapsedTimer timer; timer.start();
    waitFor([&] { return timer.elapsed() >= milliseconds; }, milliseconds + 1000);
}
void writeFile(const QString &path, int size = 4) {
    QFile file(path); check(file.open(QIODevice::WriteOnly) && file.write(QByteArray(size, 'x')) == size, "write fixture");
}
QModelIndex findUrl(Browser &browser, const QUrl &url) {
    auto *model = browser.activeView()->model();
    for (int row = 0; row < model->rowCount(); ++row) {
        const auto index = model->index(row, 0);
        if (index.data(KDirModel::FileItemRole).value<KFileItem>().url() == url) return index;
    }
    return {};
}
QPoint displayedPosition(Browser &browser, const QUrl &url) {
    auto *view = browser.activeView();
    return view->visualRect(findUrl(browser, url)).topLeft() +
        QPoint(view->horizontalScrollBar()->value(), view->verticalScrollBar()->value());
}
QSet<QUrl> selection(Browser &browser) {
    const auto urls = browser.selectedUrls(); return QSet<QUrl>(urls.cbegin(), urls.cend());
}
void hover(Browser &browser, const QUrl &folder, QMimeData &mime) {
    auto *view = browser.activeView();
    const auto index = findUrl(browser, folder); check(index.isValid(), "hover folder exists");
    view->scrollTo(index);
    const QPoint point = view->visualRect(index).center();
    QDragEnterEvent enter(point, Qt::CopyAction | Qt::MoveAction | Qt::LinkAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(view->viewport(), &enter);
    if (!enter.isAccepted()) std::cerr << "Rejected hover mode=" << browser.mode() << " url=" << browser.url().toString().toStdString() << " point=" << point.x() << "," << point.y() << " visible=" << view->viewport()->isVisible() << " hasUrls=" << mime.hasUrls() << " drops=" << view->viewport()->acceptDrops() << " enabled=" << view->viewport()->isEnabled() << "\n";
    check(enter.isAccepted(), "URL drag accepted");
    QDragMoveEvent move(point, Qt::CopyAction | Qt::MoveAction | Qt::LinkAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(view->viewport(), &move); check(move.isAccepted(), "URL hover accepted");
}
}

int main(int argc, char **argv) {
    check(qgetenv("QT_QPA_PLATFORM") == "offscreen" && !qEnvironmentVariableIsSet("DISPLAY") &&
        !qEnvironmentVariableIsSet("WAYLAND_DISPLAY") && qEnvironmentVariableIsSet("XDG_CONFIG_HOME") &&
        qEnvironmentVariableIsSet("DBUS_SESSION_BUS_ADDRESS"), "private headless runner required");
    QApplication app(argc, argv);
    app.setOrganizationName("FinderBrowserPrivateTest"); app.setApplicationName("BrowserArrangement");
    QTemporaryDir files; QTemporaryDir elsewhere;
    check(files.isValid() && elsewhere.isValid() && QDir().mkpath(files.filePath("folder/nested")), "temporary folder fixtures");
    writeFile(files.filePath("alpha.txt"), 3); writeFile(files.filePath("zeta.txt"), 19);
    writeFile(files.filePath("folder/inside.txt")); writeFile(elsewhere.filePath("copy.txt"));
    const auto root = QUrl::fromLocalFile(files.path());
    const auto folder = QUrl::fromLocalFile(files.filePath("folder"));
    const auto nested = QUrl::fromLocalFile(files.filePath("folder/nested"));
    const auto alpha = QUrl::fromLocalFile(files.filePath("alpha.txt"));
    const auto zeta = QUrl::fromLocalFile(files.filePath("zeta.txt"));
    const auto external = QUrl::fromLocalFile(elsewhere.filePath("copy.txt"));
    Browser browser; browser.resize(800, 450); browser.setIconPreviewsShown(false); browser.show();
    int navigations = 0, temporaryNavigations = 0;
    QObject::connect(&browser, &Browser::urlChanged, [&](const QUrl &) { ++navigations; });
    QObject::connect(&browser, &Browser::springUrlChanged, [&](const QUrl &) { ++temporaryNavigations; });
    browser.navigateValidated(root);
    check(waitFor([&] { return browser.itemCount() == 3; }), "validated KIO listing");
    browser.setSort(-1, true);
    check(browser.canArrangeIcons() && browser.sortColumn() == -1, "Icons None enables manual arrangement");
    browser.setIconPosition(alpha, {241, 151}); browser.setIconPosition(zeta, {29, 307});
    settle();
    check(browser.iconPosition(alpha) == QPoint(241, 151) && displayedPosition(browser, alpha) == QPoint(241, 151), "actual off-grid coordinate");
    check(displayedPosition(browser, zeta) == QPoint(29, 307), "independent second coordinate");
    browser.setIconPosition(alpha, {167, 91}); browser.setIconPosition(alpha, {241, 151});
    check(displayedPosition(browser, alpha) == QPoint(241, 151), "reversal has no cumulative drift");
    // The offscreen backend ends QDrag immediately. Use the same native URL
    // MIME produced for view dragging and dispatch actual private Qt drops.
    auto *dragView = browser.activeView();
    const auto dragIndex = findUrl(browser, alpha);
    dragView->scrollTo(dragIndex);
    const QPoint origin = dragView->visualRect(dragIndex).center();
    browser.selectUrls({alpha, zeta});
    const auto oldAlpha = browser.iconPosition(alpha), oldZeta = browser.iconPosition(zeta);
    std::unique_ptr<QMimeData> nativeMime(browser.createDragMimeData(origin));
    check(nativeMime && nativeMime->hasUrls() && nativeMime->hasFormat("application/x-finder-icon-arrangement"), "native URL MIME retains arrangement data");
    const QPoint destination = origin + QPoint(190, -23);
    QDragEnterEvent enter(destination, Qt::CopyAction | Qt::MoveAction | Qt::LinkAction, nativeMime.get(), Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(dragView->viewport(), &enter);
    QDropEvent dropped(destination, Qt::CopyAction | Qt::MoveAction | Qt::LinkAction, nativeMime.get(), Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(dragView->viewport(), &dropped);
    settle();
    check(dropped.isAccepted() && dropped.dropAction() == Qt::MoveAction &&
        browser.iconPosition(alpha) == oldAlpha + QPoint(190, -23) &&
        browser.iconPosition(zeta) == oldZeta + QPoint(190, -23), "native multi-icon drop changes positions without transfer");
    check(selection(browser) == QSet<QUrl>{alpha, zeta} && QFileInfo::exists(alpha.toLocalFile()) &&
        QFileInfo::exists(zeta.toLocalFile()), "native arrangement preserves source rows and files");
    browser.setIconPosition(alpha, oldAlpha); browser.setIconPosition(zeta, oldZeta);
    browser.selectUrls({alpha, zeta});
    browser.setSort(Browser::NameColumn, false); settle();
    check(!browser.canArrangeIcons() && selection(browser) == QSet<QUrl>{alpha, zeta}, "sorted selection preserved");
    check(displayedPosition(browser, alpha) != QPoint(241, 151), "sorted surface actually returns to automatic grid");
    browser.setSort(-1, false); settle();
    check(displayedPosition(browser, alpha) == QPoint(241, 151) && displayedPosition(browser, zeta) == QPoint(29, 307), "None restores URL positions after reverse sort");
    browser.setGroup(Browser::GroupKind); settle();
    check(!browser.canArrangeIcons() && selection(browser) == QSet<QUrl>{alpha, zeta}, "grouped native selection");
    browser.setGroup(Browser::GroupNone); settle();
    check(displayedPosition(browser, alpha) == QPoint(241, 151), "ungroup restores manual layout");
    browser.setFilter("alpha"); settle(); browser.setFilter({}); settle();
    check(displayedPosition(browser, alpha) == QPoint(241, 151) && displayedPosition(browser, zeta) == QPoint(29, 307), "filter restores URL identity");
    browser.selectUrls({alpha, zeta});
    for (const auto mode : {Browser::List, Browser::Columns, Browser::Gallery, Browser::Icons}) {
        browser.setMode(mode); settle();
        check(selection(browser) == QSet<QUrl>{alpha, zeta}, "selection crosses all native modes");
    }
    check(browser.sortColumn() == -1 && displayedPosition(browser, alpha) == QPoint(241, 151), "Icons mode retains layout and None");
    browser.setSnapToGrid(true);
    browser.setIconPosition(alpha, {239, 149});
    const auto grid = qobject_cast<QListView *>(browser.activeView())->gridSize();
    check(browser.iconPosition(alpha).x() % grid.width() == 0 && browser.iconPosition(alpha).y() % grid.height() == 0, "Snap uses actual icon geometry");
    const auto fixed = browser.iconPosition(zeta);
    browser.selectUrls({alpha}); browser.cleanUp(); settle();
    check(browser.iconPosition(zeta) == fixed && selection(browser) == QSet<QUrl>{alpha}, "Clean Up respects selection");
    browser.selectUrls({}); browser.cleanUpBy(Browser::NameColumn, true); settle();
    const auto alphaAscending = browser.iconPosition(alpha), zetaAscending = browser.iconPosition(zeta);
    check(alphaAscending.x() < zetaAscending.x() && browser.sortColumn() == -1, "Clean Up By name arranges without persistent sort");
    browser.cleanUpBy(Browser::NameColumn, false); settle();
    check(browser.iconPosition(zeta).x() < browser.iconPosition(alpha).x(), "Clean Up By reversal changes positions");
    browser.cleanUpBy(Browser::SizeColumn, true); settle();
    check(browser.iconPosition(alpha).x() < browser.iconPosition(zeta).x(), "Clean Up By uses native raw numeric metadata");
    const QByteArray saved = browser.saveViewState();
    const auto savedPositions = QJsonDocument::fromJson(saved).object().value("iconPositions").toObject();
    check(savedPositions.contains(alpha.toString(QUrl::FullyEncoded)) && browser.snapToGrid(), "JSON stores complete URLs and snap");
    {
        Browser reopened; reopened.resize(800, 450); reopened.setIconPreviewsShown(false); reopened.show();
        reopened.restoreViewState(saved); reopened.navigateValidated(root);
        check(waitFor([&] { return reopened.itemCount() == 3; }), "reopened listing"); settle();
        check(reopened.snapToGrid() && reopened.sortColumn() == -1 && displayedPosition(reopened, alpha) == browser.iconPosition(alpha), "state survives fresh Browser before navigation");
        reopened.navigateValidated(QUrl::fromLocalFile(elsewhere.path()));
        check(waitFor([&] { return reopened.itemCount() == 1; }), "unrelated folder listing");
        reopened.restoreViewState(saved); settle();
        check(reopened.iconPosition(alpha) == QPoint(-1, -1), "defaults do not import another folder's coordinates");
    }
    browser.setSnapToGrid(false); browser.selectUrls({alpha, zeta});
    const auto names = QDir(files.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
    browser.setSpringLoadingDelay(100);
    QMimeData mime; mime.setUrls({external});
    const int beforeHover = navigations;
    hover(browser, folder, mime); browser.cancelSpringLoading(); settle(150);
    check(browser.url() == root && !browser.springLoadingActive(), "cancel before dwell never navigates");
    hover(browser, folder, mime);
    QDragMoveEvent away({790, 440}, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(browser.activeView()->viewport(), &away); settle(150);
    check(browser.url() == root, "moving off folder cancels dwell"); browser.cancelSpringLoading();
    hover(browser, folder, mime);
    check(waitFor([&] { return browser.url() == folder && browser.itemCount() == 2; }), "folder spring-opens after dwell");
    check(navigations == beforeHover && temporaryNavigations > 0, "spring navigation bypasses history");
    hover(browser, nested, mime);
    check(waitFor([&] { return browser.url() == nested; }), "nested spring navigation");
    QDragLeaveEvent leave; QApplication::sendEvent(browser.activeView()->viewport(), &leave);
    check(waitFor([&] { return browser.url() == root && browser.itemCount() == 3 && !browser.springLoadingActive(); }), "abandoned drag rolls back root");
    check(waitFor([&] { return selection(browser) == QSet<QUrl>{alpha, zeta}; }), "rollback restores native multi-selection");
    check(navigations == beforeHover && QDir(files.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot) == names &&
        !QFileInfo::exists(files.filePath("folder/copy.txt")), "arrangement and hover never mutate files");
    browser.selectUrls({});
    const auto keptAlpha = browser.iconPosition(alpha), keptZeta = browser.iconPosition(zeta);
    writeFile(files.filePath("new.txt"));
    const auto added = QUrl::fromLocalFile(files.filePath("new.txt"));
    browser.directoryModel()->dirLister()->updateDirectory(root);
    check(waitFor([&] { return browser.itemCount() == 4 && browser.iconPosition(added).x() >= 0; }), "new native rows get vacant persistent positions");
    check(browser.iconPosition(alpha) == keptAlpha && browser.iconPosition(zeta) == keptZeta &&
        browser.iconPosition(added) != keptAlpha && browser.iconPosition(added) != keptZeta, "listing refresh does not rearrange existing files");
    browser.resize(640, 400); settle();
    check(displayedPosition(browser, alpha) == keptAlpha, "manual positions survive viewport resize");
    QFile::remove(added.toLocalFile()); browser.directoryModel()->dirLister()->updateDirectory(root);
    check(waitFor([&] { return browser.itemCount() == 3; }), "removed fixture refresh");
    for (const auto mode : {Browser::List, Browser::Columns, Browser::Gallery, Browser::Icons}) {
        browser.setMode(mode); settle(); browser.selectUrls({alpha, zeta});
        const int prior = navigations;
        hover(browser, folder, mime);
        check(waitFor([&] { return browser.url() == folder && browser.itemCount() == 2; }), "all primary modes spring-open folders");
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(browser.activeView(), &escape);
        check(waitFor([&] { return browser.url() == root && browser.itemCount() == 3 && selection(browser) == QSet<QUrl>{alpha, zeta}; }),
            "Escape rolls back native selection in all primary modes");
        check(navigations == prior, "spring rollback never records history in any mode");
    }
    {
        auto canceledMime = std::make_unique<QMimeData>(); canceledMime->setUrls({external});
        hover(browser, folder, *canceledMime);
        check(waitFor([&] { return browser.url() == folder && browser.itemCount() == 2; }), "spring opens before native MIME cancellation");
        canceledMime.reset();
        check(waitFor([&] { return browser.url() == root && browser.itemCount() == 3 && !browser.springLoadingActive(); }),
            "destroyed native drag cancels spring navigation");
    }
    const int beforeRealDrop = navigations;
    hover(browser, folder, mime);
    check(waitFor([&] { return browser.url() == folder && browser.itemCount() == 2; }), "spring opens for real drop");
    QDropEvent copy({700, 400}, Qt::CopyAction | Qt::MoveAction | Qt::LinkAction, &mime, Qt::LeftButton, Qt::AltModifier);
    browser.drop(&copy);
    check(copy.isAccepted() && copy.dropAction() == Qt::CopyAction && !browser.springLoadingActive() &&
        navigations == beforeRealDrop + 1, "Option Drop commits hover once with native Copy");
    check(waitFor([&] { return QFileInfo::exists(files.filePath("folder/copy.txt")); }) && QFileInfo::exists(external.toLocalFile()), "KIO copies only at Drop");
    settle(); check(KIO::FileUndoManager::self()->isUndoAvailable(), "DropJob records native Undo");
    browser.undo(); check(waitFor([&] { return !QFileInfo::exists(files.filePath("folder/copy.txt")); }), "native copy undo");
    writeFile(elsewhere.filePath("move.txt"));
    QMimeData moveMime; moveMime.setUrls({QUrl::fromLocalFile(elsewhere.filePath("move.txt"))});
    QDropEvent move({700, 400}, Qt::CopyAction | Qt::MoveAction | Qt::LinkAction, &moveMime, Qt::LeftButton, Qt::MetaModifier);
    browser.drop(&move);
    check(move.dropAction() == Qt::MoveAction && waitFor([&] { return QFileInfo::exists(files.filePath("folder/move.txt")) && !QFileInfo::exists(elsewhere.filePath("move.txt")); }), "Command native move");
    settle(); browser.undo(); check(waitFor([&] { return QFileInfo::exists(elsewhere.filePath("move.txt")); }), "native move undo");
    QDropEvent link({700, 400}, Qt::CopyAction | Qt::MoveAction | Qt::LinkAction, &mime, Qt::LeftButton, Qt::MetaModifier | Qt::AltModifier);
    browser.drop(&link);
    check(link.dropAction() == Qt::LinkAction && waitFor([&] { return QFileInfo(files.filePath("folder/copy.txt")).isSymLink(); }), "Command Option native link");
    settle(); browser.undo(); check(waitFor([&] { return !QFileInfo::exists(files.filePath("folder/copy.txt")); }), "native link undo");
    browser.navigate(root); check(waitFor([&] { return browser.itemCount() == 3; }), "ordinary navigation retained");
    browser.setSpringLoadingEnabled(false); hover(browser, folder, mime); settle(180);
    check(browser.url() == root && !browser.springLoadingActive(), "disabled spring loading has no idle dwell");
    browser.hide();
    std::cout << "PASS: persistent icon positions, sort/group reversal, selection, spring cancellation/rollback and native KIO copy/move/link Undo\n";
}
