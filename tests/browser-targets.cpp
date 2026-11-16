// SPDX-License-Identifier: MIT
// Provider-contract fixtures reproduce the public recentlyused worker's
// UDS_NAME / UDS_DISPLAY_NAME / UDS_TARGET_URL / UDS_LOCAL_PATH entries.
#include "browser.h"
#include "filetarget.h"
#include <QApplication>
#include <QAbstractItemView>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QElapsedTimer>
#include <QThread>
#include <QKeyEvent>
#include <QMimeData>
#include <QClipboard>
#include <KDirModel>
#include <KDirLister>
#include <KUrlMimeData>
#include <KIO/UDSEntry>
#include <sys/stat.h>
#include <iostream>
#include <functional>
static void check(bool ok, const char *message) { if (!ok) { std::cerr << "FAIL " << message << '\n'; std::exit(1); } }
static bool wait(const std::function<bool()> &ready) { QElapsedTimer timer; timer.start(); while (!ready() && timer.elapsed()<8000) { QApplication::processEvents(QEventLoop::AllEvents,10); QThread::msleep(1); } return ready(); }
int main(int argc, char **argv) {
 QApplication app(argc,argv); app.setOrganizationName("FinderTargetsPrivate"); app.setApplicationName("Targets");
 QTemporaryDir root; check(QDir().mkdir(root.filePath("Listing")),"listing folder"); const auto directory=QUrl::fromLocalFile(root.filePath("Listing"));
 const auto target=QUrl::fromLocalFile(root.filePath("Photo # one.png"));
 QFile file(target.toLocalFile()); check(file.open(QIODevice::WriteOnly),"source"); file.write("source bytes"); file.close();
 Browser browser; browser.resize(800,480); browser.setIconPreviewsShown(false); browser.show(); browser.navigate(directory);
 check(wait([&]{return browser.directoryModel()->dirLister()->isFinished();}),"real native listing");
 KIO::UDSEntry uds;
 uds.fastInsert(KIO::UDSEntry::UDS_NAME,QStringLiteral("Photo # one.png-0"));
 uds.fastInsert(KIO::UDSEntry::UDS_DISPLAY_NAME,QStringLiteral("Photo # one.png"));
 uds.fastInsert(KIO::UDSEntry::UDS_TARGET_URL,target.toString());
 uds.fastInsert(KIO::UDSEntry::UDS_LOCAL_PATH,target.toLocalFile());
 uds.fastInsert(KIO::UDSEntry::UDS_FILE_TYPE,S_IFREG);
 uds.fastInsert(KIO::UDSEntry::UDS_MIME_TYPE,QStringLiteral("image/png"));
 const auto virtualUrl=QUrl(QStringLiteral("recentlyused:/files/Photo%20%23%20one.png-0"));
 KFileItem entry(uds,virtualUrl);
 check(FileTarget::url(entry)==target && FileTarget::item(entry).name()==target.fileName(),"native provider target/name");
 check(FileTarget::url(KFileItem(target))==target,"ordinary files stay native");
 check(FileTarget::url(KFileItem()).isEmpty(),"null entry");
 // KDirModel receives the same public provider signal as a worker listing.
 browser.directoryModel()->dirLister()->itemsAdded(directory,{entry});
 check(wait([&]{return browser.itemCount()==1;}),"virtual provider entry");
 QUrl opened, preview;
 QObject::connect(&browser,&Browser::openRequested,&browser,[&](const QUrl &url){opened=url;});
 QObject::connect(&browser,&Browser::previewRequested,&browser,[&](const QUrl &url){preview=url;});
 for (int mode=0;mode<4;++mode) {
  browser.setMode(Browser::Mode(mode)); browser.navigate(directory);
  check(wait([&]{return browser.directoryModel()->dirLister()->isFinished();}),"mode listing");
  if (!browser.directoryModel()->indexForUrl(virtualUrl).isValid()) browser.directoryModel()->dirLister()->itemsAdded(directory,{entry});
  browser.selectUrls({virtualUrl});
  const bool chosen=wait([&]{return browser.selectedUrls()==QList<QUrl>{target};});
  if(!chosen){std::cerr<<"mode "<<mode<<" rows "<<browser.itemCount()<<" direct "<<browser.directoryModel()->indexForUrl(virtualUrl).isValid()<<" selected ";for(const auto &u:browser.selectedUrls())std::cerr<<u.toString().toStdString()<<" ";std::cerr<<"target "<<target.toString().toStdString()<<"\n";}
  check(chosen,"target URL selection");
  check(browser.primaryUrl()==target && preview==target,"primary and preview target");
  check(browser.selectedItems().first().name()==target.fileName(),"operation filename excludes virtual suffix");
  browser.copySelected();
  check(KUrlMimeData::urlsFromMimeData(QApplication::clipboard()->mimeData(),KUrlMimeData::PreferLocalUrls)==QList<QUrl>{target},"native clipboard target");
  std::unique_ptr<QMimeData> mime(browser.createDragMimeData(QPoint(10,10)));
  check(mime && KUrlMimeData::urlsFromMimeData(mime.get(),KUrlMimeData::PreferLocalUrls)==QList<QUrl>{target},"native drag target");
  browser.setSort(Browser::NameColumn,false);
  check(browser.selectedUrls()==QList<QUrl>{target},"sort retains target selection");
  browser.activeView()->doubleClicked(browser.activeView()->currentIndex());
  check(wait([&]{return opened==target;}),"open target"); opened=QUrl();
 }
 check(browser.directoryModel()->indexForUrl(virtualUrl).isValid(),"source model identity preserved");
 check(file.open(QIODevice::ReadOnly),"source readable");check(file.readAll()=="source bytes","read-only route test preserves source");
 std::cout<<"PASS provider targets in all four modes, primary/preview/open, native MIME, sort, source identity\n";
}
