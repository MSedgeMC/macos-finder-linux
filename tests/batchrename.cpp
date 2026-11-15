// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
// Private standalone target: this file + src/batchrename.cpp/.h, AUTOMOC;
// Qt6::Widgets KF6::KIOCore KF6::KIOGui KF6::KIOWidgets.
// Run with tests/run-headless.py; all data, windows, input and native dialogs
// are private offscreen fixtures. Tests never access the working session.
#include "batchrename.h"

#include <KIO/CopyJob>
#include <KIO/FileUndoManager>
#include <KIO/RenameDialog>
#include <KIO/StatJob>
#include <KJobUiDelegate>
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <functional>
#include <iostream>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

using namespace FinderBatchRename;
namespace {
void check(bool result, const char *message)
{
    if (!result) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
bool waitFor(const std::function<bool()> &condition, int timeout = 15000)
{
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < timeout) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return condition();
}
void write(const QString &path, const QByteArray &data)
{
    QFile file(path);
    check(file.open(QIODevice::WriteOnly) && file.write(data) == data.size(), "write fixture");
}
QByteArray bytes(const QString &path)
{
    QFile file(path); check(file.open(QIODevice::ReadOnly), "read fixture"); return file.readAll();
}
Item item(const QString &path, bool directory = false) { return {QUrl::fromLocalFile(path), directory}; }
KFileItem fileItem(const Item &item)
{
    return KFileItem(item.url, QString(), item.directory ? S_IFDIR : S_IFREG);
}
struct Observer {
    int completed = 0, errorCode = 0, moves = 0, done = 0;
    bool cancelled = false;
    QString validation;
    QList<QUrl> outputs;
    explicit Observer(BatchRenameEngine &engine)
    {
        QObject::connect(&engine, &BatchRenameEngine::finished, &engine, [this](const QList<QUrl> &urls, bool c) {
            outputs = urls; cancelled = c; ++completed;
        });
        QObject::connect(&engine, &BatchRenameEngine::error, &engine, [this](KJob *job) { errorCode = job->error(); });
        QObject::connect(&engine, &BatchRenameEngine::validationFailed, &engine, [this](const QString &message) { validation = message; });
        QObject::connect(&engine, &BatchRenameEngine::progressChanged, &engine, [this](int value, int) { done = value; });
        QObject::connect(&engine, &BatchRenameEngine::jobCreated, &engine, [this](KIO::Job *job) {
            if (qobject_cast<KIO::CopyJob *>(job)) ++moves;
            // Tests inspect errors without a second auto-handling message box.
            if (job->uiDelegate()) job->uiDelegate()->setAutoErrorHandlingEnabled(false);
        });
    }
    void finish(BatchRenameEngine &engine, int expected = 1)
    {
        check(waitFor([&] { return completed == expected && !engine.busy(); }), "bounded asynchronous completion");
    }
};
class UndoErrors final : public KIO::FileUndoManager::UiInterface {
public:
    int errors = 0;
    void jobError(KIO::Job *job) override { ++errors; std::cerr << "Undo: " << job->errorString().toStdString() << '\n'; }
};
void undoSteps(int count)
{
    auto *manager = KIO::FileUndoManager::self();
    auto *ui = new UndoErrors; ui->setShowProgressInfo(false); manager->setUiInterface(ui);
    int finished = 0;
    auto connection = QObject::connect(manager, &KIO::FileUndoManager::undoJobFinished, manager, [&] { ++finished; });
    for (int i = 0; i < count; ++i) {
        check(manager->isUndoAvailable(), "native undo command available");
        manager->undo();
        check(waitFor([&] { return finished == i + 1; }) && ui->errors == 0, "native undo completes without error");
    }
    QObject::disconnect(connection);
}
KIO::RenameDialog *conflictDialog()
{
    for (auto *widget : QApplication::topLevelWidgets())
        if (auto *dialog = qobject_cast<KIO::RenameDialog *>(widget); dialog && dialog->isVisible()) return dialog;
    return nullptr;
}
void chooseConflictName(KIO::RenameDialog *dialog, const QString &name)
{
    auto *edit = dialog->findChild<QLineEdit *>(); check(edit, "native conflict name field");
    edit->setText(name); dialog->renamePressed();
}
}

int main(int argc, char **argv)
{
    check(qgetenv("QT_QPA_PLATFORM") == "offscreen" && !qEnvironmentVariableIsSet("DISPLAY")
          && !qEnvironmentVariableIsSet("WAYLAND_DISPLAY") && qEnvironmentVariableIsSet("DBUS_SESSION_BUS_ADDRESS")
          && qEnvironmentVariableIsSet("XDG_CONFIG_HOME"), "private offscreen runner and bus required");
    QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("FinderBatchRenamePrivateTest"));
    app.setApplicationName(QStringLiteral("BatchRename"));
    QTemporaryDir fixture; check(fixture.isValid(), "fixture directory");
    const auto path = [&](const QString &name) { return fixture.filePath(name); };
    const QList<Item> sample{item(path("alpha.TXT")), item(path("archive.tar.gz")), item(path(".bashrc")),
                            item(path("notes.weird")), item(path("Folder.name"), true), item(path(".prefs.json"))};
    Options options;
    options.mode = Mode::AddText; options.text = QStringLiteral(" edited");
    auto plan = preview(sample, options);
    check(plan.canRename() && plan.entries[0].newName == "alpha edited.TXT" && plan.entries[1].newName == "archive edited.tar.gz"
          && plan.entries[2].newName == ".bashrc edited" && plan.entries[3].newName == "notes edited.weird"
          && plan.entries[4].newName == "Folder.name edited" && plan.entries[5].newName == ".prefs edited.json",
          "Add after preserves compound/unknown/case extensions and treats dots in folders/dotfiles correctly");
    options.placement = Placement::BeforeName; options.text = QStringLiteral("Pré-");
    plan = preview(sample, options);
    check(plan.entries[0].newName == QStringLiteral("Pré-alpha.TXT") && plan.entries[2].newName == QStringLiteral("Pré-.bashrc"), "Add before Unicode");
    options.mode = Mode::ReplaceText; options.find = QStringLiteral("TXT"); options.replacement = QStringLiteral("png");
    plan = preview(sample, options);
    check(plan.changedCount == 1 && plan.entries[0].newName == "alpha.png" && plan.entries[0].extensionChanged
          && !plan.entries[1].changed, "Replace actual filename, case-sensitive, unchanged rows and extension care");
    options.find.clear(); check(!preview(sample, options).canRename(), "empty find never inserts replacement everywhere");
    options.find = "tar"; options.replacement = "zip";
    check(preview(sample, options).entries[1].newName == "archive.zip.gz", "Replace recognized compound extension");
    options.mode = Mode::Format; options.customName = "Vacation"; options.startNumber = 7;
    options.placement = Placement::AfterName;
    plan = preview(sample, options);
    check(plan.entries[0].newName == "Vacation 7.TXT" && plan.entries[1].newName == "Vacation 8.tar.gz"
          && plan.entries[4].newName == "Vacation 11", "Format index preserves original file extensions");
    options.format = NameFormat::NameAndCounter;
    check(preview(sample, options).entries[0].newName == "Vacation00007.TXT", "Counter minimum five digits");
    options.startNumber = 123456; options.placement = Placement::BeforeName;
    check(preview(sample, options).entries[0].newName == "123456Vacation.TXT", "Counter is not truncated and supports before");
    options.format = NameFormat::NameAndDate; options.startNumber = 4;
    options.date = QDateTime(QDate(2026, 11, 15), QTime(10, 20, 30));
    check(preview(sample, options).entries[0].newName == "2026-11-15 at 10.20.30 4 Vacation.TXT", "fixed captured date and unique sequence");
    options.placement = Placement::AfterName;
    check(preview(sample, options).entries[0].newName == "Vacation 2026-11-15 at 10.20.30 4.TXT", "Date after");
    options.startNumber = std::numeric_limits<int>::max(); check(!preview(sample, options).canRename(), "sequence overflow rejected");
    check(!previewNames(sample, {"x"}).canRename(), "name count mismatch rejected");
    for (const auto &invalid : {QString(), QString("."), QString(".."), QString("a/b"), QString(QChar::Null), QString(256, 'x'), QString(100, QChar(0x20ac))})
        check(!previewNames({sample.first()}, {invalid}).canRename(), "invalid name or UTF8 byte limit rejected");
    check(previewNames({sample.first()}, {" .colon: "}).canRename(), "Linux names preserve whitespace, leading dots and colon");
    check(!previewNames({sample[0], sample[1]}, {"same", "same"}).canRename(), "intra-selection collision rejected");
    check(!previewNames({sample[0], sample[0]}, {"x", "y"}).canRename(), "duplicate source rejected");
    check(!previewNames({item(path("folder"), true), item(path("folder/child"))}, {"renamed", "new"}).canRename(), "ancestor selection rejected");
    check(!previewNames({{QUrl("sftp://host/a.txt"), false}}, {"b.txt"}).canRename(), "remote rename rejected rather than copy fallback");
    const auto literal = previewNames({item(path("x.txt"))}, {QStringLiteral("100% #? Café.txt")});
    check(literal.canRename() && literal.entries[0].destination.fileName() == QStringLiteral("100% #? Café.txt")
          && !literal.entries[0].destination.hasQuery() && !literal.entries[0].destination.hasFragment(), "URL filename escaping");
    QList<Item> many; QStringList manyNames;
    for (int i = 0; i < MaximumItems; ++i) { many.append(item(path(QString::number(i)))); manyNames.append(QStringLiteral("new-%1").arg(i)); }
    check(previewNames(many, manyNames).canRename(), "4096-item bound accepts limit");
    many.append(item(path("extra"))); manyNames.append("new-extra");
    check(!previewNames(many, manyNames).canRename(), "4097-item batch rejected");
    std::cout << "PASS: Replace/Add/Format preview, extensions, Unicode/URLs, validation and bounds\n";

    // Full operation across different parents, directories and link entries.
    check(QDir().mkpath(path("one")) && QDir().mkpath(path("two")) && QDir().mkpath(path("one/Dir.ext")), "directory fixtures");
    write(path("one/a.txt"), "A"); write(path("two/b.tar.gz"), "B"); write(path("one/Dir.ext/child"), "child");
    check(::symlink("missing", QFile::encodeName(path("two/broken.link")).constData()) == 0, "broken symlink fixture");
    QList<Item> mixed{item(path("one/a.txt")), item(path("two/b.tar.gz")), item(path("one/Dir.ext"), true), item(path("two/broken.link"))};
    options = {}; options.mode = Mode::AddText; options.text = "-new";
    struct stat originalMetadata {};
    check(::stat(QFile::encodeName(path("one/a.txt")).constData(), &originalMetadata) == 0, "capture original inode and metadata");
    BatchRenameEngine mixedEngine; Observer mixedResult(mixedEngine);
    int heartbeat = 0; QTimer timer; timer.setInterval(1);
    QObject::connect(&timer, &QTimer::timeout, [&] { ++heartbeat; }); timer.start();
    QElapsedTimer dispatch; dispatch.start(); mixedEngine.start(preview(mixed, options));
    check(dispatch.elapsed() < 100 && mixedEngine.busy(), "start nonblocking");
    mixedResult.finish(mixedEngine);
    check(mixedResult.moves == 4 && mixedResult.done == 4 && mixedResult.outputs.size() == 4 && mixedResult.errorCode == 0 && mixedResult.validation.isEmpty(), "native rename mixed parents succeeds");
    check(bytes(path("one/a-new.txt")) == "A" && bytes(path("two/b-new.tar.gz")) == "B" && bytes(path("one/Dir.ext-new/child")) == "child"
          && QFileInfo(path("two/broken-new.link")).isSymbolicLink(), "contents and broken symlink preserved");
    check(mixedEngine.originalUrls()[0] == mixed[0].url && heartbeat > 0, "immutable original URLs and responsive event loop");
    struct stat renamedMetadata {};
    check(::stat(QFile::encodeName(path("one/a-new.txt")).constData(), &renamedMetadata) == 0
          && originalMetadata.st_ino == renamedMetadata.st_ino && originalMetadata.st_dev == renamedMetadata.st_dev
          && originalMetadata.st_mode == renamedMetadata.st_mode && originalMetadata.st_mtim.tv_sec == renamedMetadata.st_mtim.tv_sec
          && originalMetadata.st_mtim.tv_nsec == renamedMetadata.st_mtim.tv_nsec, "rename preserves inode, mode and content modification time");
    undoSteps(4);
    check(bytes(path("one/a.txt")) == "A" && QFileInfo(path("two/broken.link")).isSymbolicLink(), "native Undo restores mixed original names");

    // Chain scheduling must rename b->c before a->b; reverse native Undo
    // restores both contents without a collision or data replacement.
    write(path("chain-a"), "first"); write(path("chain-b"), "second");
    const QList<Item> chain{item(path("chain-a")), item(path("chain-b"))};
    BatchRenameEngine chainEngine; Observer chainResult(chainEngine);
    chainEngine.start(previewNames(chain, {"chain-b", "chain-c"})); chainResult.finish(chainEngine);
    check(chainResult.moves == 2 && bytes(path("chain-b")) == "first" && bytes(path("chain-c")) == "second", "cycle-free dependency ordering");
    undoSteps(2); check(bytes(path("chain-a")) == "first" && bytes(path("chain-b")) == "second", "chain native Undo");

    write(path("cycle-a"), "A"); write(path("cycle-b"), "B"); write(path("cycle-c"), "C");
    const QList<Item> cycle{item(path("cycle-a")), item(path("cycle-b")), item(path("cycle-c"))};
    BatchRenameEngine cycleEngine; Observer cycleResult(cycleEngine);
    cycleEngine.start(previewNames(cycle, {"cycle-b", "cycle-c", "cycle-a"})); cycleResult.finish(cycleEngine);
    check(cycleResult.moves == 4 && cycleResult.outputs.size() == 3 && bytes(path("cycle-a")) == "C"
          && bytes(path("cycle-b")) == "A" && bytes(path("cycle-c")) == "B", "three-way cycle preserves all data");
    check(QDir(fixture.path()).entryList({".finder-rename-*"}, QDir::AllEntries | QDir::Hidden).isEmpty(), "cycle leaves no temporary name");
    undoSteps(4); check(bytes(path("cycle-a")) == "A" && bytes(path("cycle-b")) == "B" && bytes(path("cycle-c")) == "C", "cycle native Undo reverse steps");

    write(path("collision-source"), "source"); write(path("occupied"), "target");
    BatchRenameEngine collisionEngine; Observer collisionResult(collisionEngine);
    collisionEngine.start(previewNames({item(path("collision-source"))}, {"occupied"})); collisionResult.finish(collisionEngine);
    check(!collisionResult.validation.isEmpty() && collisionResult.moves == 0 && bytes(path("collision-source")) == "source"
          && bytes(path("occupied")) == "target", "external collision rejected without writes");
    BatchRenameEngine missingEngine; Observer missingResult(missingEngine);
    missingEngine.start(previewNames({item(path("missing-source"))}, {"new-name"})); missingResult.finish(missingEngine);
    check(missingResult.errorCode == KIO::ERR_DOES_NOT_EXIST && missingResult.moves == 0, "native source error reported");

    // Immediate cancel and reuse must ignore all queued callbacks from the old
    // operation. Cancel doesn't write or make native Undo entries.
    write(path("cancel-a"), "a");
    BatchRenameEngine immediate; Observer immediateResult(immediate);
    immediate.start(previewNames({item(path("cancel-a"))}, {"cancel-b"})); immediate.cancel();
    check(immediateResult.completed == 1 && immediateResult.cancelled && immediateResult.moves == 0, "immediate cancel");
    immediate.start(previewNames({item(path("cancel-a"))}, {"cancel-c"})); immediateResult.finish(immediate, 2);
    check(immediateResult.moves == 1 && bytes(path("cancel-c")) == "a", "reuse rejects stale queued dispatch"); undoSteps(1);

    // Cancel after first successful independent item retains the real partial
    // subset and its Undo. The currently dispatched item always finishes.
    write(path("partial-a"), "a"); write(path("partial-b"), "b");
    BatchRenameEngine partial; Observer partialResult(partial);
    QObject::connect(&partial, &BatchRenameEngine::itemRenamed, &partial, [&] { partial.cancel(); });
    partial.start(previewNames({item(path("partial-a")), item(path("partial-b"))}, {"partial-new-a", "partial-new-b"})); partialResult.finish(partial);
    check(partialResult.cancelled && partialResult.outputs.size() == 1 && partialResult.moves == 1
          && bytes(path("partial-new-a")) == "a" && bytes(path("partial-b")) == "b", "partial cancellation reports real successes only"); undoSteps(1);
    BatchRenameEngine cancelCycle; Observer cancelCycleResult(cancelCycle);
    QObject::connect(&cancelCycle, &BatchRenameEngine::itemRenamed, &cancelCycle, [&] { cancelCycle.cancel(); });
    cancelCycle.start(previewNames(cycle, {"cycle-b", "cycle-c", "cycle-a"})); cancelCycleResult.finish(cancelCycle);
    check(cancelCycleResult.cancelled && cancelCycleResult.outputs.size() == 3 && cancelCycleResult.moves == 4
          && QDir(fixture.path()).entryList({".finder-rename-*"}, QDir::AllEntries | QDir::Hidden).isEmpty(), "cancel completes active cycle without stranding temporary names"); undoSteps(4);

    // Remove a later source after all preflight checks. Earlier success remains
    // recorded even though the next native job reports an error.
    write(path("error-a"), "a"); write(path("error-b"), "b");
    BatchRenameEngine errorEngine; Observer errorResult(errorEngine);
    QObject::connect(&errorEngine, &BatchRenameEngine::itemRenamed, &errorEngine, [&](const QUrl &, const QUrl &) { QFile::remove(path("error-b")); });
    errorEngine.start(previewNames({item(path("error-a")), item(path("error-b"))}, {"error-new-a", "error-new-b"})); errorResult.finish(errorEngine);
    check(errorResult.errorCode == KIO::ERR_DOES_NOT_EXIST && errorResult.outputs.size() == 1 && bytes(path("error-new-a")) == "a", "later native error retains earlier partial rename"); undoSteps(1);
    std::cout << "PASS: asynchronous KIO renames, different parents, folders/links, chains/cycles, native Undo, errors and cancellation\n";

    // Conflict occurs after preflight: create target immediately before the
    // public native rename job dispatch. All GUI stays inside offscreen runner.
    write(path("race-source"), "source");
    BatchRenameEngine race; Observer raceResult(race);
    QObject::connect(&race, &BatchRenameEngine::jobCreated, &race, [&](KIO::Job *job) {
        if (qobject_cast<KIO::CopyJob *>(job) && !QFileInfo::exists(path("race-target"))) write(path("race-target"), "racer");
    });
    race.start(previewNames({item(path("race-source"))}, {"race-target"}));
    check(waitFor([] { return conflictDialog(); }), "native KIO race conflict dialog");
    auto *nativeConflict = conflictDialog();
    // A native alternate name must not overwrite another captured source or
    // occupy a planned destination. The wrapper keeps its native dialog.
    QPointer<KIO::RenameDialog> originalConflict(nativeConflict);
    chooseConflictName(nativeConflict, QStringLiteral("race-source"));
    check(waitFor([&] { return !originalConflict && conflictDialog(); }), "native conflict rejects reserved source name");
    chooseConflictName(conflictDialog(), QStringLiteral("race-alternate")); raceResult.finish(race);
    check(raceResult.outputs == QList<QUrl>{QUrl::fromLocalFile(path("race-alternate"))} && bytes(path("race-alternate")) == "source"
          && bytes(path("race-target")) == "racer" && !QFileInfo::exists(path("two/invalid-retarget")), "native conflict alternate actual URL reported and other data retained"); undoSteps(1);

    write(path("race-cancel-source"), "source");
    BatchRenameEngine raceCancel; Observer raceCancelResult(raceCancel);
    QObject::connect(&raceCancel, &BatchRenameEngine::jobCreated, &raceCancel, [&](KIO::Job *job) {
        if (qobject_cast<KIO::CopyJob *>(job)) write(path("race-cancel-target"), "racer");
    });
    raceCancel.start(previewNames({item(path("race-cancel-source"))}, {"race-cancel-target"}));
    check(waitFor([] { return conflictDialog(); }), "native cancel conflict dialog");
    conflictDialog()->cancelPressed(); raceCancelResult.finish(raceCancel);
    check(raceCancelResult.cancelled && raceCancelResult.outputs.isEmpty() && bytes(path("race-cancel-source")) == "source", "native conflict cancel");
    write(path("race-skip-source"), "source");
    BatchRenameEngine raceSkip; Observer raceSkipResult(raceSkip);
    QObject::connect(&raceSkip, &BatchRenameEngine::jobCreated, &raceSkip, [&](KIO::Job *job) {
        if (qobject_cast<KIO::CopyJob *>(job)) write(path("race-skip-target"), "racer");
    });
    raceSkip.start(previewNames({item(path("race-skip-source"))}, {"race-skip-target"}));
    check(waitFor([] { return conflictDialog(); }), "native skip conflict dialog");
    conflictDialog()->skipPressed(); raceSkipResult.finish(raceSkip);
    check(!raceSkipResult.cancelled && raceSkipResult.outputs.isEmpty() && bytes(path("race-skip-source")) == "source"
          && bytes(path("race-skip-target")) == "racer", "native Skip preserves both files");

    // Failure after cycle staging reports the recoverable temporary URL. Do
    // not invent rollback: native Undo restores it after the missing slot is
    // repaired (the same condition any native Undo job requires).
    write(path("fail-cycle-a"), "A"); write(path("fail-cycle-b"), "B");
    BatchRenameEngine failCycle; Observer failCycleResult(failCycle);
    QObject::connect(&failCycle, &BatchRenameEngine::jobCreated, &failCycle, [&](KIO::Job *job) {
        if (qobject_cast<KIO::CopyJob *>(job) && failCycleResult.moves == 2) QFile::remove(path("fail-cycle-b"));
    });
    failCycle.start(previewNames({item(path("fail-cycle-a")), item(path("fail-cycle-b"))}, {"fail-cycle-b", "fail-cycle-a"}));
    failCycleResult.finish(failCycle);
    check(failCycleResult.errorCode == KIO::ERR_DOES_NOT_EXIST && failCycleResult.outputs.size() == 1
          && failCycleResult.outputs.first().fileName().startsWith(".finder-rename-")
          && bytes(failCycleResult.outputs.first().toLocalFile()) == "A", "cycle failure reports preserved temporary item");
    undoSteps(1); check(bytes(path("fail-cycle-a")) == "A", "native Undo recovers cycle staging after failure");
    std::cout << "PASS: native KIO race conflict, reserved names, chosen target, cancel/Skip and cycle failure recovery\n";

    // Dialog fixture: normal modeless public integration, live controls, stable
    // selection, extension warning and actual async completion. Screenshot is
    // an optional artifact captured offscreen, never opened on the desktop.
    write(path("dialog-a.TXT"), "first"); write(path("dialog-b.TXT"), "second");
    auto *dialog = new BatchRenameDialog({fileItem(item(path("dialog-a.TXT"))), fileItem(item(path("dialog-b.TXT")))});
    QPointer<BatchRenameDialog> dialogGuard(dialog);
    dialog->show();
    check(!dialog->isModal() && dialog->windowModality() == Qt::NonModal && dialog->testAttribute(Qt::WA_DeleteOnClose), "modeless delete-on-close dialog");
    auto *rename = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
    auto *table = dialog->findChild<QTableWidget *>(QStringLiteral("batchRenamePreview"));
    check(table && table->rowCount() == 2 && !rename->isEnabled(), "initial unchanged preview disables rename");
    auto *mode = dialog->findChild<QComboBox *>(QStringLiteral("batchRenameMode"));
    mode->setCurrentIndex(1);
    dialog->findChild<QLineEdit *>(QStringLiteral("batchAddText"))->setText("-added");
    check(rename->isEnabled() && table->item(0, 1)->text() == "dialog-a-added.TXT", "live add preview");
    mode->setCurrentIndex(2);
    dialog->findChild<QSpinBox *>(QStringLiteral("batchStartNumber"))->setValue(23);
    check(table->item(0, 1)->text() == "File 23.TXT", "live format/start number preview");
    const auto original = dialog->originalUrls();
    options = {}; options.mode = Mode::ReplaceText; options.find = "TXT"; options.replacement = "md";
    dialog->setOptions(options);
    if (qEnvironmentVariableIsSet("FINDER_BATCHRENAME_SCREENSHOT")) {
        QApplication::processEvents();
        check(dialog->grab().save(qEnvironmentVariable("FINDER_BATCHRENAME_SCREENSHOT")), "save isolated dialog screenshot");
    }
    int dialogFinished = 0; QList<QUrl> dialogOutputs;
    QObject::connect(dialog, &BatchRenameDialog::renamingFinished, &app, [&](const QList<QUrl> &urls) { ++dialogFinished; dialogOutputs = urls; });
    rename->click();
    check(waitFor([&] { return dialog->findChild<QMessageBox *>("finderBatchExtensionWarning"); }), "Replace extension confirmation appears");
    auto *warning = dialog->findChild<QMessageBox *>("finderBatchExtensionWarning");
    check(warning->defaultButton()->text() == "Review Names" && QFileInfo::exists(path("dialog-a.TXT")), "extension confirmation defaults to review before writes");
    warning->defaultButton()->click();
    check(waitFor([&] { return !dialog->busy(); }) && dialogFinished == 0 && QFileInfo::exists(path("dialog-b.TXT")), "extension review cancels without work");
    rename->click();
    check(waitFor([&] { return dialog->findChild<QMessageBox *>("finderBatchExtensionWarning"); }), "extension confirmation can be reopened");
    warning = dialog->findChild<QMessageBox *>("finderBatchExtensionWarning");
    for (auto *button : warning->buttons()) if (button->text() == "Use New Extensions") button->click();
    check(waitFor([&] { return dialogFinished == 1 && !dialogGuard; }), "dialog asynchronously completes and closes");
    check(dialogOutputs.size() == 2 && bytes(path("dialog-a.md")) == "first" && bytes(path("dialog-b.md")) == "second"
          && original.first() == QUrl::fromLocalFile(path("dialog-a.TXT")), "dialog actual outputs and immutable originals"); undoSteps(2);
    // Real close request during execution waits for a dependency-safe stop,
    // emits partial results once, and cleans up after completion.
    write(path("close-a.txt"), "a"); write(path("close-b.txt"), "b");
    auto *closing = new BatchRenameDialog({fileItem(item(path("close-a.txt"))), fileItem(item(path("close-b.txt")))});
    QPointer<BatchRenameDialog> closingGuard(closing);
    closing->show(); options = {}; options.mode = Mode::AddText; options.text = "-renamed";
    closing->setOptions(options);
    auto *closingEngine = closing->findChild<BatchRenameEngine *>();
    int closeSignals = 0; QList<QUrl> closeOutputs;
    QObject::connect(closing, &BatchRenameDialog::renamingFinished, &app, [&](const QList<QUrl> &urls) { ++closeSignals; closeOutputs = urls; });
    QObject::connect(closingEngine, &BatchRenameEngine::itemRenamed, closing, [closing] { closing->close(); });
    closing->accept();
    check(waitFor([&] { return !closingGuard; }) && closeSignals == 1 && closeOutputs.size() == 1
          && bytes(path("close-a-renamed.txt")) == "a" && bytes(path("close-b.txt")) == "b", "close during work cancels after current item and emits partial subset");
    undoSteps(1);

    // A preflight collision is left visible with a Close action; an attempted
    // operation cannot accidentally retry immutable originals after a failure.
    write(path("dialog-conflict.txt"), "source"); write(path("dialog-conflict-new.txt"), "target");
    auto *failed = new BatchRenameDialog({fileItem(item(path("dialog-conflict.txt")))});
    QPointer<BatchRenameDialog> failedGuard(failed); failed->show(); failed->setOptions(options);
    // Use an explicitly matching suffix in this fixture.
    options.text = "-new"; failed->setOptions(options);
    int failedSignals = 0;
    QObject::connect(failed, &BatchRenameDialog::renamingFinished, &app, [&](const QList<QUrl> &urls) { ++failedSignals; check(urls.isEmpty(), "failed dialog emits no renamed items"); });
    failed->accept();
    check(waitFor([&] { return failedSignals == 1 && !failed->busy(); }) && failedGuard && failed->isVisible()
          && failed->findChild<QLabel *>("batchRenameStatus")->text().contains("already exists"), "failed dialog remains readable");
    auto *failedButtons = failed->findChild<QDialogButtonBox *>();
    check(!failedButtons->button(QDialogButtonBox::Ok)->isEnabled() && failedButtons->button(QDialogButtonBox::Cancel)->text() == "Close", "failed dialog cannot retry stale originals");
    failedButtons->button(QDialogButtonBox::Cancel)->click(); check(waitFor([&] { return !failedGuard; }), "failed dialog cleanup");
    auto *empty = new BatchRenameDialog({}); QPointer<BatchRenameDialog> emptyGuard(empty);
    empty->show(); empty->reject(); check(waitFor([&] { return !emptyGuard; }), "idle dialog cancel cleans up");
    std::cout << "PASS: modeless live dialog, extension confirmation/review, async results and cleanup\n";
    return 0;
}
