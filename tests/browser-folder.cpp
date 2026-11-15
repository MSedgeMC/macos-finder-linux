// SPDX-License-Identifier: MIT
#include "browser.h"
#include <QAbstractItemView>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPointer>
#include <QTemporaryDir>
#include <QThread>
#include <QSettings>
#include <QTextEdit>
#include <KDirModel>
#include <KDirLister>
#include <KIO/FileUndoManager>
#include <KIO/Job>
#include <iostream>
#include <functional>
#include <cstdlib>
static void check(bool ok,const char *m){if(!ok){std::cerr<<"FAIL: "<<m<<'\n';std::exit(1);}}
static bool wait(const std::function<bool()> &condition){QElapsedTimer t;t.start();while(!condition()&&t.elapsed()<10000){QApplication::processEvents(QEventLoop::AllEvents,10);QThread::msleep(1);}return condition();}
class UndoErrors final : public KIO::FileUndoManager::UiInterface {
public:
 int errors=0;
 void jobError(KIO::Job *job) override {++errors;std::cerr<<"UNDO ERROR: "<<job->errorString().toStdString()<<'\n';}
};
int main(int argc,char **argv){
 check(qgetenv("QT_QPA_PLATFORM")=="offscreen"&&qEnvironmentVariableIsSet("DBUS_SESSION_BUS_ADDRESS"),"private runner");
 QApplication app(argc,argv);app.setOrganizationName("FinderFolderPrivate");app.setApplicationName("Folder");QSettings().setValue("Advanced/warnExtensionChange",false);
 QTemporaryDir fixture;check(fixture.isValid(),"fixture");
 auto *undoUi=new UndoErrors;undoUi->setShowProgressInfo(false);KIO::FileUndoManager::self()->setUiInterface(undoUi);
 check(QDir().mkdir(fixture.filePath("Untitled Folder")),"occupied folder");
 Browser browser;browser.setIconPreviewsShown(false);browser.resize(900,500);browser.show();browser.navigate(QUrl::fromLocalFile(fixture.path()));
 int recordings=0;QObject::connect(KIO::FileUndoManager::self(),&KIO::FileUndoManager::jobRecordingFinished,&browser,[&](KIO::FileUndoManager::CommandType){++recordings;});
 check(wait([&]{return browser.itemCount()==1;}),"listing");
 for(int mode=0;mode<4;++mode){
  std::cerr<<"MODE "<<mode<<"\n";
  browser.navigate(QUrl::fromLocalFile(fixture.path()));
  check(wait([&]{return browser.itemCount()==mode+1;}),"mode fixture listing");
  browser.setMode(Browser::Mode(mode));browser.selectUrls({});
  const int recordingsBefore=recordings;
  // Simulate a name created after the original lister snapshot; native mkdir
  // must resolve that collision without merging or rewriting existing data.
  const QString created=fixture.filePath(QString("Untitled Folder %1").arg(mode+2));
  browser.newFolder();
  check(wait([&]{return QFileInfo(created).isDir();}),"create unused untitled folder");
  check(wait([&]{return recordings>recordingsBefore;}),"mkdir recording complete");
  QPointer<QWidget> editor;
  check(wait([&]{
   for(auto *candidate : browser.findChildren<QWidget*>())
    if(candidate->isVisible() && (qobject_cast<QLineEdit*>(candidate)||qobject_cast<QTextEdit*>(candidate))){editor=candidate;return true;}
   return false;
  }),"inline editor all modes");
  editor->setFocus();
  if(auto *line=qobject_cast<QLineEdit*>(editor.data())) line->setText(QString("New %1").arg(mode));
  else qobject_cast<QTextEdit*>(editor.data())->setPlainText(QString("New %1").arg(mode));
  QKeyEvent enter(QEvent::KeyPress,Qt::Key_Return,Qt::NoModifier);QApplication::sendEvent(editor,&enter);
  const QString renamed=fixture.filePath(QString("New %1").arg(mode));
  check(wait([&]{return QFileInfo(renamed).isDir()&&!QFileInfo(created).exists();}),"inline rename native operation");
  check(wait([&]{return recordings>recordingsBefore+1;}),"rename recording complete");
  auto *undo=KIO::FileUndoManager::self();check(undo->isUndoAvailable(),"native rename undo available");
  int undoCompletions = 0;
  const auto completed = QObject::connect(undo, &KIO::FileUndoManager::undoJobFinished, &browser, [&]{ ++undoCompletions; });
  std::cerr<<"FIRST "<<undo->undoText().toStdString()<<'\n';
  undo->uiInterface()->setParentWidget(&browser);undo->undo();check(wait([&]{return undoCompletions == 1;}),"rename undo finished");check(!undoUi->errors,"rename undo error");
  check(QFileInfo(created).isDir()&&!QFileInfo(renamed).exists(),"undo rename");
  check(wait([&]{return undo->isUndoAvailable();}),"mkdir undo available");std::cerr<<"SECOND "<<undo->undoText().toStdString()<<'\n';undo->undo();check(wait([&]{return undoCompletions == 2;}),"mkdir undo finished");check(!undoUi->errors,"mkdir undo error");
  check(!QFileInfo(created).exists(),"undo mkdir");QObject::disconnect(completed);
  // Leave occupied names for subsequent numbered creation, preserving every
  // fixture folder and avoiding native same-second removal-watch assumptions.
  check(QDir().mkdir(created),"next occupied name");browser.directoryModel()->dirLister()->openUrl(QUrl::fromLocalFile(fixture.path()),KDirLister::Reload);
  check(wait([&]{return browser.itemCount()==mode+2;}),"reload occupied names");
 }
 check(QFileInfo(fixture.filePath("Untitled Folder")).isDir(),"original folder preserved");
 std::cout<<"PASS all-mode inline new folder, collision naming, rename and two native Undo steps\n";
}
