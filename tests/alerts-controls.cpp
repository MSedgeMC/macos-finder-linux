// SPDX-License-Identifier: MIT
#include "alerts.h"
#include "windowcontrols.h"
#include <QApplication>
#include <QSettings>
#include <QKeyEvent>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <KIO/CopyJob>
#include <KIO/AskUserActionInterface>
#include <KIO/JobUiDelegateFactory>
#include <iostream>
static void check(bool ok,const char *m){if(!ok){std::cerr<<"FAIL "<<m<<'\n';std::exit(1);}}
class Failure final:public KJob {public:void start()override{setError(KIO::ERR_DOES_NOT_EXIST);setErrorText("Missing <file> & details");emitResult();}};
int main(int argc,char **argv){
 QApplication app(argc,argv);app.setOrganizationName("FinderChromePrivate");app.setApplicationName("Chrome");app.setStyle("Fusion");FinderAppearance::apply(&app);
 QWidget host;host.resize(420,200);host.show();
 auto *alert=new FinderAlert("“Photo.png” couldn’t be opened.","The item may have been moved or deleted, or its drive may be disconnected.","Native <file> & details",&host);
 alert->open();QApplication::processEvents();check(alert->width()==320&&alert->height()>140,"content-fit alert");
 check(alert->windowModality()==Qt::WindowModal&&alert->testAttribute(Qt::WA_TranslucentBackground),"parented rounded alert");
 auto *detail=alert->findChild<QPlainTextEdit*>("finderAlertDetails");auto *toggle=alert->findChild<QToolButton*>("finderAlertDetailsToggle");auto *ok=alert->findChild<QPushButton*>("finderAlertOK");
 check(detail&&toggle&&ok&&ok->isDefault()&&detail->isReadOnly()&&!detail->isVisible(),"details and default action");
 int before=alert->height();toggle->click();QApplication::processEvents();check(detail->isVisible()&&alert->height()>before,"details expand");
 toggle->click();QApplication::processEvents();check(!detail->isVisible()&&alert->height()<=before+4,"details collapse");
 check(FinderAlert::plain("<p>Text &amp; native detail</p>")=="Text & native detail","native HTML plain conversion");
 if(argc>1)check(alert->grab().save(QString::fromLocal8Bit(argv[1])+".alert.png"),"alert artifact");
 QPointer<FinderAlert> guard(alert);QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);QApplication::sendEvent(alert,&escape);QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);check(!guard,"Escape closes safely");
 auto *job=new Failure;job->setProperty("finderOpenUrl",QUrl::fromLocalFile("/missing/Photo.png"));auto *delegate=new FinderJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled,&host);job->setUiDelegate(delegate);
 check(KIO::delegateExtension<KIO::AskUserActionInterface *>(job),"native ask-user conflict interface preserved");job->start();QApplication::processEvents();
 auto *failure=host.findChild<QDialog*>("finderErrorAlert");check(failure&&failure->isVisible()&&failure->windowTitle().contains("Photo.png"),"native job adapted error");
 check(failure->findChild<QLabel*>("finderAlertMessage")->text().contains("moved or deleted"),"truthful missing-file summary");failure->reject();QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
 for(const char *mode:{"dark","light"})for(bool contrast:{false,true}){
  QSettings().setValue("Appearance/mode",mode);QSettings().setValue("Appearance/increaseContrast",contrast);FinderAppearance::apply(&app);
  QWidget chrome;auto *layout=new QHBoxLayout(&chrome);for(const auto &kind:{"close","minimize","maximize"})layout->addWidget(new WindowControl(kind,&chrome));chrome.show();chrome.activateWindow();QApplication::processEvents();
  auto *close=chrome.findChild<QToolButton*>("window-close");check(close&&close->accessibleName()=="Close"&&close->size()==QSize(20,28),"unchanged native control hitbox/accessibility");
  const auto pixels=close->grab().toImage();check(pixels.pixelColor(8,8)!=pixels.pixelColor(8,19)||contrast,"gloss gradient");
  if(argc>1)check(chrome.grab().save(QString::fromLocal8Bit(argv[1])+QString(".controls-%1-%2.png").arg(mode).arg(contrast)),"control artifact");
  close->setFocus();QApplication::processEvents();close->setDown(true);if(argc>1)chrome.grab().save(QString::fromLocal8Bit(argv[1])+QString(".controls-pressed-%1-%2.png").arg(mode).arg(contrast));
 }
 {auto *owner=new QWidget;FinderAlert::showError(owner,"Error","Owned alert");
  FinderAlert::showError(owner,"Error","Owned alert");check(owner->findChildren<QDialog*>("finderErrorAlert").size()==1,"same error deduplicated");
  FinderAlert::showError(owner,"Error","Different error");check(owner->findChildren<QDialog*>("finderErrorAlert").size()==2,"different errors retained");
  FinderAlert::showError(owner,"Long error",QString(3000,'x'));
  auto *longError=owner->findChildren<QDialog*>("finderErrorAlert").last();
  check(longError->height()<600&&longError->findChild<QPlainTextEdit*>("finderAlertDetails")->toPlainText().size()==3000,"long diagnostics bounded and retained");
  delete owner;QApplication::processEvents();}
 std::cout<<"PASS alert detail/default/Escape/parent/native delegate and active controls/contrast/pressed/lifetime\n";
}
