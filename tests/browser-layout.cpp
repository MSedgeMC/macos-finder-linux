// SPDX-License-Identifier: MIT
#include "browser.h"
#include "appearance.h"
#include "glasscontrols.h"
#include <QApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QTreeView>
#include <QListView>
#include <QHeaderView>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QSettings>
#include <QElapsedTimer>
#include <QThread>
#include <QHBoxLayout>
#include <KDirModel>
#include <KDirLister>
#include <functional>
#include <iostream>
static void check(bool ok,const char *message){if(!ok){std::cerr<<"FAIL "<<message<<'\n';std::exit(1);}}
static bool wait(const std::function<bool()> &f){QElapsedTimer t;t.start();while(!f()&&t.elapsed()<7000){QApplication::processEvents(QEventLoop::AllEvents,10);QThread::msleep(1);}return f();}
static void click(QWidget *w,QPoint point,Qt::KeyboardModifiers modifiers={}){
 for(auto type:{QEvent::MouseButtonPress,QEvent::MouseButtonRelease}){QMouseEvent e(type,point,w->mapToGlobal(point),Qt::LeftButton,type==QEvent::MouseButtonPress?Qt::LeftButton:Qt::NoButton,modifiers);QApplication::sendEvent(w,&e);}
}
static void write(const QString &path){QFile f(path);check(f.open(QIODevice::WriteOnly),"fixture");f.write("Native contents");}
int main(int argc,char **argv){
 QApplication app(argc,argv);app.setOrganizationName("FinderLayoutPrivate");app.setApplicationName("Layout");app.setStyle("Fusion");QFont font=app.font();font.setPixelSize(13);app.setFont(font);FinderAppearance::apply(&app);
 QTemporaryDir dir;for(int i=0;i<5;++i)write(dir.filePath(QString("File %1.txt").arg(i)));QDir().mkdir(dir.filePath("Nested"));write(dir.filePath("Nested/Child.txt"));
 Browser b;b.resize(748,381);b.setIconPreviewsShown(false);b.show();b.navigate(QUrl::fromLocalFile(dir.path()));check(wait([&]{return b.itemCount()==6;}),"native listing");
 auto *icons=b.findChild<QListView*>("finderIconView");check(icons&&icons->gridSize().width()==111,"reference icon pitch");
 auto selected=QUrl::fromLocalFile(dir.filePath("File 0.txt"));b.selectUrls({selected});
 for(int mode=0;mode<4;++mode){b.setMode(Browser::Mode(mode));check(b.selectedUrls()==QList<QUrl>{selected},"all-mode selection");}
 b.setMode(Browser::List);auto *tree=b.findChild<QTreeView*>("finderListView");check(tree,"list");
 auto *model=tree->model();auto index=model->index(1,0,tree->rootIndex());check(tree->visualRect(index).height()==20,"compact real row");
 check(tree->header()->logicalIndex(1)==Browser::DateModifiedColumn&&tree->header()->logicalIndex(2)==Browser::SizeColumn,"reference headers");
 b.selectUrls({});QApplication::processEvents();auto image=tree->viewport()->grab().toImage();int stripeY=tree->visualRect(index).center().y();
 check(image.pixelColor(1,stripeY)==image.pixelColor(1,stripeY-20),"stripe outer gutter stays clear");
 check(image.pixelColor(14,stripeY)!=image.pixelColor(14,stripeY-20),"real stripes alternate");
 const int trailing=tree->visualRect(model->index(5,0,tree->rootIndex())).bottom()+1;
 check(!tree->indexAt(QPoint(80,trailing+30)).isValid(),"blank stripe has no file index");click(tree->viewport(),QPoint(80,trailing+30));check(b.selectedUrls().isEmpty(),"blank stripe cannot select a file");
 check(image.pixelColor(14,trailing+30)!=image.pixelColor(14,trailing+10),"stripes extend through blank viewport");
 if(argc>1)check(b.grab().save(QString::fromLocal8Bit(argv[1])+".list.png"),"list artifact");
 auto nested=model->index(5,0,tree->rootIndex());if(!nested.data(KDirModel::FileItemRole).value<KFileItem>().isDir())for(int i=0;i<model->rowCount(tree->rootIndex());++i){auto n=model->index(i,0,tree->rootIndex());if(n.data(KDirModel::FileItemRole).value<KFileItem>().isDir())nested=n;}
 tree->expand(nested);check(wait([&]{return model->rowCount(nested)==1;}),"native nested expansion");auto child=model->index(0,0,nested);click(tree->viewport(),tree->visualRect(child).center());check(b.selectedUrls()==QList<QUrl>{QUrl::fromLocalFile(dir.filePath("Nested/Child.txt"))},"nested selection");tree->collapse(nested);
 b.setGroup(Browser::GroupKind);QApplication::processEvents();b.setGroup(Browser::GroupNone);b.setTextSize(20);check(tree->visualRect(model->index(0,0,tree->rootIndex())).height()>=QFontMetrics(tree->font()).height()+2,"large text row adapts");b.setTextSize(13);
 for(const char *mode:{"light","dark"}){QSettings().setValue("Appearance/mode",mode);FinderAppearance::apply(&app);QApplication::processEvents();b.grab();}
 QWidget host;auto *layout=new QHBoxLayout(&host);auto *capsule=new GlassCapsule(&host);auto *buttons=new QHBoxLayout(capsule);auto *button=new GlassToolButton(capsule);button->setFixedSize(34,36);button->setIcon(QIcon::fromTheme("folder"));button->setAccessibleName("Native action");buttons->addWidget(button);layout->addWidget(capsule);host.show();QApplication::processEvents();int activated=0;QObject::connect(button,&QToolButton::clicked,&host,[&]{++activated;});button->setFocus();QKeyEvent key(QEvent::KeyPress,Qt::Key_Space,{});QApplication::sendEvent(button,&key);QKeyEvent release(QEvent::KeyRelease,Qt::Key_Space,{});QApplication::sendEvent(button,&release);check(activated==1&&button->accessibleName()=="Native action","glass preserves keyboard and accessibility");
 button->setCheckable(true);button->setChecked(true);QSettings().setValue("Appearance/reduceTransparency",true);FinderAppearance::apply(&app);host.grab();QSettings().setValue("Appearance/increaseContrast",true);FinderAppearance::apply(&app);host.grab();
 if(argc>1)check(host.grab().save(QString::fromLocal8Bit(argv[1])+".glass.png"),"glass artifact");
 std::cout<<"PASS compact rows, rounded real/empty stripes, native tree/selection, grid geometry, grouped/large text, appearance and glass keyboard\n";
}
