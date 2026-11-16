// SPDX-License-Identifier: MIT
#include "browser.h"
#include "appearance.h"
#include <QApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QAbstractItemView>
#include <QElapsedTimer>
#include <QThread>
#include <QSettings>
#include <QDialog>
#include <KDirModel>
#include <KDirLister>
#include <functional>
#include <iostream>
static void check(bool ok,const char *m){if(!ok){std::cerr<<"FAIL "<<m<<'\n';std::exit(1);}}
static bool wait(const std::function<bool()> &f){QElapsedTimer t;t.start();while(!f()&&t.elapsed()<6000){QApplication::processEvents(QEventLoop::AllEvents,10);QThread::msleep(1);}return f();}
static QWidget *visibleState(Browser &b){for(auto *s:b.findChildren<QWidget*>("finderDirectoryState"))if(s->isVisible())return s;return nullptr;}
int main(int argc,char **argv){
 QApplication app(argc,argv);app.setOrganizationName("FinderStatesPrivate");app.setApplicationName("States");FinderAppearance::apply(&app);
 QTemporaryDir fixture;check(fixture.isValid(),"fixture");const auto url=QUrl::fromLocalFile(fixture.path());
 Browser b;b.resize(820,500);b.setIconPreviewsShown(false);b.show();
 for(int mode=0;mode<4;++mode){
  b.setMode(Browser::Mode(mode));b.navigate(url);
  check(wait([&]{return visibleState(b)&&visibleState(b)->property("title")=="This folder is empty";}),"all-mode successful empty state");
  auto *s=visibleState(b);check(s->testAttribute(Qt::WA_TransparentForMouseEvents)&&s->focusPolicy()==Qt::NoFocus,"native input surface preserved");
  check(s->width()>200&&s->height()>70,"viewport geometry");
  if(argc>1)check(b.grab().save(QString::fromLocal8Bit(argv[1])+QString(".empty-%1.png").arg(mode)),"artifact");
  b.setFilter("not present");check(wait([&]{return visibleState(b)&&visibleState(b)->property("state")=="filter";}),"filter no matches");
  b.setFilter("");
 }
 QFile hidden(fixture.filePath(".hidden"));check(hidden.open(QIODevice::WriteOnly),"hidden file");hidden.write("hidden");hidden.close();
 b.directoryModel()->dirLister()->openUrl(url,KDirLister::Reload);
 check(wait([&]{return visibleState(b)&&visibleState(b)->property("title")=="No visible items";}),"hidden-only truthful state");
 b.setHiddenFiles(true);check(wait([&]{return b.itemCount()==1&&!visibleState(b);}),"revealed content removes state");
 b.setHiddenFiles(false);QFile shown(fixture.filePath("Shown.txt"));check(shown.open(QIODevice::WriteOnly),"visible file");shown.write("content");shown.close();
 b.directoryModel()->dirLister()->openUrl(url,KDirLister::Reload);check(wait([&]{return b.itemCount()==1&&!visibleState(b);}),"population transition");
 b.selectUrls({QUrl::fromLocalFile(shown.fileName())});check(b.selectedUrls().size()==1,"selection after overlay");
 b.setMode(Browser::Columns);b.navigate(url);check(wait([&]{return b.itemCount()==1&&!visibleState(b);}),"column populated");
 check(QDir().mkdir(fixture.filePath("Empty child")),"nested fixture");b.directoryModel()->dirLister()->openUrl(url,KDirLister::Reload);check(wait([&]{return b.itemCount()==2;}),"nested listing");
 b.selectUrls({QUrl::fromLocalFile(fixture.filePath("Empty child"))});b.activeView()->doubleClicked(b.activeView()->currentIndex());
 bool childEmpty=wait([&]{return visibleState(b)&&visibleState(b)->property("title")=="This folder is empty";});
 if(!childEmpty){for(auto *s:b.findChildren<QWidget*>("finderDirectoryState"))std::cerr<<"STATE "<<s->isVisible()<<" "<<s->property("title").toString().toStdString()<<" parent "<<s->parent()->objectName().toStdString()<<"\n";for(auto *v:b.findChildren<QAbstractItemView*>())if(v->isVisible())std::cerr<<"VIEW "<<v->objectName().toStdString()<<" root "<<v->rootIndex().data().toString().toStdString()<<" rows "<<(v->model()?v->model()->rowCount(v->rootIndex()):-1)<<"\n";}
 check(childEmpty,"empty child column state");
 // Deliberately skip stat validation so KDirLister supplies the authoritative
 // asynchronous error. It must never become a successful empty-folder state.
 b.setMode(Browser::Icons);b.navigateValidated(QUrl::fromLocalFile(fixture.filePath("Missing")));
 check(wait([&]{return visibleState(b)&&visibleState(b)->property("state")=="error";}),"failed listing distinct from empty");
 if(argc>1)check(b.grab().save(QString::fromLocal8Bit(argv[1])+".error.png"),"error artifact");
 for(auto *dialog:app.topLevelWidgets())if(auto *d=qobject_cast<QDialog*>(dialog))d->reject();
 b.navigate(url);check(wait([&]{return b.itemCount()==2&&!visibleState(b);}),"error recovery");
 b.resize(370,200);b.setFilter("absent");check(visibleState(b),"narrow state");if(argc>1)b.grab().save(QString::fromLocal8Bit(argv[1])+".narrow.png");
 // Destroy an active column hierarchy after state callbacks have been installed.
 {Browser temporary;temporary.setMode(Browser::Columns);temporary.navigate(url);QApplication::processEvents();}
 std::cout<<"PASS all-mode empty/filter/hidden/loading error/recovery, nested columns, native input and lifetime\n";
}
