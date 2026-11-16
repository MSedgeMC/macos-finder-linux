// SPDX-License-Identifier: MIT
#include "infohost.h"
#include "filedetails.h"
#include <KPageWidget>
#include <KPageWidgetModel>
#include <KPageWidgetItem>
#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPointer>
#include <QTemporaryDir>
#include <QThread>
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/xattr.h>

namespace {
std::mutex mutex;
std::condition_variable wake;
std::atomic_int entered{0};
bool released = true;
quint64 blockedInode = 0;
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
template<class F> void waitFor(F predicate) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < 10000) { QApplication::processEvents(); QThread::msleep(1); }
    require(predicate(), "timed out waiting for private operation");
}
void release() { std::lock_guard lock(mutex); released = true; wake.notify_all(); }
void showDetails(FinderInfoDialog *host) {
    auto *pages = host->findChild<KPageWidget *>(); require(pages, "native pages");
    auto *model = qobject_cast<KPageWidgetModel *>(pages->model()); require(model, "native page model");
    for (int i = 0; i < model->rowCount(); ++i) {
        auto *item = model->item(model->index(i,0));
        if (item->objectName() == "finderFileDetailsItem") { host->setCurrentPage(item); return; }
    }
    require(false, "metadata page");
}
}
extern "C" ssize_t __real_fgetxattr(int, const char *, void *, size_t);
extern "C" ssize_t __wrap_fgetxattr(int fd, const char *name, void *value, size_t size) {
    struct stat info{};
    if (::fstat(fd,&info) == 0) {
        std::unique_lock lock(mutex);
        if (!released && quint64(info.st_ino) == blockedInode) { ++entered; wake.wait(lock, [] { return released; }); }
    }
    return __real_fgetxattr(fd,name,value,size);
}
int main(int argc, char **argv) {
    try {
        require(qgetenv("QT_QPA_PLATFORM") == "offscreen" && !qEnvironmentVariableIsSet("WAYLAND_DISPLAY") && !qEnvironmentVariableIsSet("DISPLAY"), "private offscreen only");
        QApplication app(argc,argv);
        QTemporaryDir folder; require(folder.isValid(), "fixture");
        const auto path = folder.filePath("File.txt"); QFile file(path); require(file.open(QIODevice::WriteOnly), "fixture write"); file.write("original"); file.close();
        const auto url = QUrl::fromLocalFile(path);
        QPointer<FinderInfoDialog> host = new FinderInfoDialog(url); host->show(); showDetails(host);
        auto *details = host->details(); require(details, "editor");
        waitFor([&] { return !details->metadataCache()->isLoading(); });
        const auto metadata = details->metadataCache()->cached(url); require(metadata && metadata->available(), "metadata");
        {
            std::lock_guard lock(mutex); blockedInode = metadata->identity.inode; released = false; entered = 0;
        }
        FileDetailsCache blockerA, blockerB; blockerA.setUrls({url}); blockerB.setUrls({url});
        waitFor([] { return entered.load() >= 2; });
        require(details->metadataCache()->setComment(url,"queued comment") != 0, "save admitted");
        require(host->busy(), "pending save keeps native host busy");
        host->accept(); host->reject(); require(!host->close() && host && host->isVisible(), "native close paths preserve queued save");
        auto *name = details->findChild<QLineEdit *>("fileDetailsName"); require(name,"name editor");
        QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier); QApplication::sendEvent(details,&escape);
        require(host && host->isVisible() && !details->isHidden(), "embedded Escape while saving does not blank editor");
        release(); waitFor([&] { return host && !host->busy(); });
        const auto saved = details->metadataCache()->cached(url); require(saved && saved->comment == "queued comment", "accepted save delivered");
        QPointer<FinderInfoDialog> fresh = new FinderInfoDialog(host->item()); fresh->show();
        require(fresh->selectPage("General"), "native page selected");
        require(fresh->currentPage() && QString(fresh->currentPage()->name()).remove('&') == "General", "fresh native properties has General page");
        fresh->reject();
        char text[64]{}; const auto length = ::getxattr(QFile::encodeName(path).constData(), FileDetails::commentAttribute,text,sizeof(text));
        require(length == 14 && QByteArray(text,int(length)) == "queued comment", "real xattr saved after blocked close");
        host->setRenameBusy(true); host->reject(); require(host && host->isVisible(), "rename guards close"); host->setRenameBusy(false);
        QApplication::sendEvent(details,&escape); waitFor([&] { return !host; });
        std::cout << "PASS native Get Info queued save, all close paths, Escape and rename guard\n";
        return 0;
    } catch (const std::exception &error) { release(); std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
