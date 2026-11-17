// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#include "batchrename.h"
#include "alerts.h"

#include <KIO/CopyJob>
#include <KIO/FileUndoManager>
#include <KIO/JobUiDelegate>
#include <KIO/StatJob>
#include <KIO/WidgetsAskUserActionHandler>
#include <KJobUiDelegate>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeDatabase>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>
#include <limits>
#include <utility>

using namespace FinderBatchRename;

namespace {
QUrl identity(const QUrl &url)
{
    return url.adjusted(QUrl::NormalizePathSegments | QUrl::StripTrailingSlash);
}
QUrl parentUrl(const QUrl &url)
{
    return identity(url).adjusted(QUrl::RemoveFilename | QUrl::StripTrailingSlash);
}
QUrl namedUrl(const QUrl &source, const QString &name)
{
    auto target = parentUrl(source);
    const auto prefix = target.path().endsWith(QLatin1Char('/')) ? target.path() : target.path() + QLatin1Char('/');
    target.setPath(prefix + name, QUrl::DecodedMode);
    return target;
}
QString nameProblem(const QString &name)
{
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String(".."))
        return QStringLiteral("Enter a filename other than “.” or “..”.");
    if (name.contains(QLatin1Char('/')) || name.contains(QChar::Null))
        return QStringLiteral("A filename cannot contain a slash or a null character.");
    // Linux supports leading dots and colons. Do not silently apply macOS's
    // filesystem restrictions or trim meaningful leading/trailing whitespace.
    if (name.toUtf8().size() > 255)
        return QStringLiteral("The filename exceeds the Linux 255-byte limit.");
    for (qsizetype i = 0; i < name.size(); ++i) {
        if (name.at(i).isHighSurrogate()) {
            if (++i == name.size() || !name.at(i).isLowSurrogate())
                return QStringLiteral("The filename contains invalid Unicode.");
        } else if (name.at(i).isLowSurrogate()) {
            return QStringLiteral("The filename contains invalid Unicode.");
        }
    }
    return {};
}
QString extension(const QString &name, bool directory)
{
    if (directory) return {};
    static const QMimeDatabase database;
    QString suffix = database.suffixForFileName(name);
    if (suffix.isEmpty()) {
        const auto dot = name.lastIndexOf(QLatin1Char('.'));
        if (dot > 0 && dot < name.size() - 1) suffix = name.mid(dot + 1);
    }
    // A dotfile without a stem (e.g. .bashrc) has no filename extension.
    if (suffix.isEmpty() || name.size() <= suffix.size() + 1) return {};
    return name.right(suffix.size() + 1); // Preserve original spelling/case.
}

// CopyJob's KF6.2 conflict overwrite path can fall back to copying/deleting.
// Keep the public native dialog but offer only renaming/skipping/cancellation.
// Also constrain an alternate target to the captured parent and reserved names.
class RenameHandler final : public KIO::WidgetsAskUserActionHandler {
public:
    RenameHandler(QWidget *window, bool allowSkip, std::function<bool(const QUrl &)> allowed)
        : m_window(window), m_allowSkip(allowSkip), m_allowed(std::move(allowed)) { setWindow(window); }
    void askUserRename(KJob *job, const QString &title, const QUrl &src, const QUrl &dest,
                       KIO::RenameDialog_Options options, KIO::filesize_t sizeSrc, KIO::filesize_t sizeDest,
                       const QDateTime &ctimeSrc, const QDateTime &ctimeDest,
                       const QDateTime &mtimeSrc, const QDateTime &mtimeDest) override
    {
        options &= ~(KIO::RenameDialog_Overwrite | KIO::RenameDialog_OverwriteItself | KIO::RenameDialog_Resume);
        if (m_allowSkip) options |= KIO::RenameDialog_MultipleItems | KIO::RenameDialog_Skip;
        auto *native = new KIO::WidgetsAskUserActionHandler(this);
        native->setWindow(m_window);
        const QPointer<KJob> guard(job);
        connect(native, &KIO::AskUserActionInterface::askUserRenameResult, this,
                [this, native, guard, src, dest, options](KIO::RenameDialog_Result result, const QUrl &url, KJob *) {
            native->deleteLater();
            if (!guard) return;
            const bool rename = result == KIO::Result_Rename || result == KIO::Result_AutoRename;
            const bool skip = result == KIO::Result_Skip || result == KIO::Result_AutoSkip;
            if ((rename && (parentUrl(url) != parentUrl(src) || !nameProblem(identity(url).fileName()).isEmpty()
                            || !m_allowed(url)))
                || (skip && !options.testFlag(KIO::RenameDialog_Skip))
                || (!rename && !skip && result != KIO::Result_Cancel)) {
                // Keep the worker idle and ask again using its native dialog.
                QTimer::singleShot(0, this, [this, guard, src, dest, options] {
                    if (guard) askUserRename(guard, QStringLiteral("Choose a Different Filename"), src, dest,
                                            options, KIO::filesize_t(-1), KIO::filesize_t(-1), {}, {}, {}, {});
                });
                return;
            }
            emit askUserRenameResult(result, url, guard);
        });
        native->askUserRename(job, title, src, dest, options, sizeSrc, sizeDest, ctimeSrc, ctimeDest, mtimeSrc, mtimeDest);
    }
private:
    QPointer<QWidget> m_window;
    bool m_allowSkip;
    std::function<bool(const QUrl &)> m_allowed;
};
class RenameDelegate final : public FinderJobUiDelegate {
public:
    RenameDelegate(QWidget *window, RenameHandler *handler)
        : FinderJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, window, {handler}) {}
};
}

QList<Item> FinderBatchRename::capture(const KFileItemList &items)
{
    QList<Item> result;
    result.reserve(items.size());
    for (const auto &item : items) result.append({item.url(), item.isDir()});
    return result;
}

Plan FinderBatchRename::previewNames(const QList<Item> &items, const QStringList &names)
{
    Plan plan;
    if (items.isEmpty() || items.size() > MaximumItems || names.size() != items.size()) {
        plan.problem = QStringLiteral("Select between 1 and %1 items and provide one name per item.").arg(MaximumItems);
        return plan;
    }
    QSet<QUrl> sources, targets;
    for (qsizetype i = 0; i < items.size(); ++i) {
        const auto &item = items.at(i);
        const auto source = identity(item.url);
        Entry entry;
        entry.item = item;
        entry.oldName = source.fileName();
        entry.newName = names.at(i);
        entry.destination = namedUrl(item.url, entry.newName);
        entry.changed = source != identity(entry.destination);
        entry.extensionChanged = extension(entry.oldName, item.directory).compare(
            extension(entry.newName, item.directory), Qt::CaseInsensitive) != 0;
        entry.problem = nameProblem(entry.newName);
        if (!source.isValid() || !source.isLocalFile() || !source.host().isEmpty() || source.hasQuery()
            || source.hasFragment() || !source.path().startsWith(QLatin1Char('/')) || entry.oldName.isEmpty())
            entry.problem = QStringLiteral("Batch rename requires local files or folders with a name.");
        else if (sources.contains(source)) entry.problem = QStringLiteral("The selection contains the same item more than once.");
        if (targets.contains(identity(entry.destination))) entry.problem = QStringLiteral("Two selected items would have the same filename in one folder.");
        sources.insert(source);
        targets.insert(identity(entry.destination));
        if (!entry.problem.isEmpty() && plan.problem.isEmpty()) plan.problem = entry.problem;
        if (entry.changed) ++plan.changedCount;
        plan.entries.append(entry);
    }
    // O(n * path depth), without filesystem work. This also rejects selections
    // from different views that include a selected folder and its child.
    for (auto &entry : plan.entries) {
        auto parent = parentUrl(entry.item.url);
        while (!parent.path().isEmpty() && parent.path() != QLatin1String("/")) {
            if (sources.contains(parent)) {
                entry.problem = QStringLiteral("Rename a folder and its contents in separate batches.");
                if (plan.problem.isEmpty()) plan.problem = entry.problem;
                break;
            }
            const auto next = parentUrl(parent);
            if (next == parent) break;
            parent = next;
        }
    }
    if (plan.problem.isEmpty() && plan.changedCount == 0) plan.problem = QStringLiteral("No filenames would change.");
    return plan;
}

Plan FinderBatchRename::preview(const QList<Item> &items, const Options &options)
{
    if (items.size() > MaximumItems) return previewNames(items, {});
    QStringList names;
    names.reserve(items.size());
    bool overflow = options.startNumber < 0
        || qint64(options.startNumber) + qint64(items.size()) - 1 > std::numeric_limits<int>::max();
    for (qsizetype i = 0; i < items.size(); ++i) {
        const auto old = identity(items.at(i).url).fileName();
        const auto suffix = extension(old, items.at(i).directory);
        const auto stem = old.left(old.size() - suffix.size());
        QString name;
        if (options.mode == Mode::ReplaceText) {
            name = old;
            if (!options.find.isEmpty()) name.replace(options.find, options.replacement, Qt::CaseSensitive);
        } else if (options.mode == Mode::AddText) {
            name = (options.placement == Placement::BeforeName ? options.text + stem : stem + options.text) + suffix;
        } else {
            const auto number = QString::number(qint64(options.startNumber) + i);
            QString token;
            if (options.format == NameFormat::NameAndIndex) token = number;
            else if (options.format == NameFormat::NameAndCounter) token = number.rightJustified(5, QLatin1Char('0'));
            else token = options.date.toString(QStringLiteral("yyyy-MM-dd 'at' HH.mm.ss")) + QLatin1Char(' ') + number;
            const auto custom = options.customName;
            const auto separator = options.format == NameFormat::NameAndCounter ? QString() : QStringLiteral(" ");
            name = (options.placement == Placement::BeforeName ? token + separator + custom : custom + separator + token) + suffix;
        }
        names.append(name);
    }
    auto plan = previewNames(items, names);
    if (options.mode == Mode::Format && overflow) plan.problem = QStringLiteral("The start number must keep every number between 0 and %1.").arg(std::numeric_limits<int>::max());
    if (options.mode == Mode::Format && options.format == NameFormat::NameAndDate && !options.date.isValid())
        plan.problem = QStringLiteral("Choose a valid date for the filenames.");
    return plan;
}

struct BatchRenameEngine::Private {
    explicit Private(BatchRenameEngine *owner, QWidget *window) : q(owner), parent(window) {}
    struct Step { int index; QUrl from, to; bool temporary = false; bool groupEnd = true; };
    enum class ProbeKind { Source, Destination, Parent };
    struct Probe { QUrl url; ProbeKind kind; };
    BatchRenameEngine *q;
    QPointer<QWidget> parent;
    Plan plan;
    QList<QUrl> current;
    QList<Step> steps;
    QList<Probe> probes;
    QSet<KIO::StatJob *> checks;
    QPointer<KIO::CopyJob> move;
    QSet<QUrl> reserved;
    QHash<QUrl, qlonglong> devices;
    int probeIndex = 0, stepIndex = 0, completed = 0;
    bool running = false, cancelled = false, groupActive = false;
    quint64 generation = 0;

    void finish(bool wasCancelled)
    {
        if (!running) return;
        for (auto *check : std::as_const(checks)) { check->disconnect(q); check->kill(KJob::Quietly); }
        checks.clear();
        QList<QUrl> outputs;
        for (qsizetype i = 0; i < current.size(); ++i)
            if (identity(current.at(i)) != identity(plan.entries.at(i).item.url)) outputs.append(current.at(i));
        running = false;
        emit q->busyChanged(false);
        emit q->finished(outputs, wasCancelled);
    }
    void failValidation(const QString &message)
    {
        emit q->validationFailed(message);
        finish(false);
    }
    void inspect()
    {
        if (!running) return;
        if (cancelled) { finish(true); return; }
        while (checks.size() < 4 && probeIndex < probes.size()) {
            const auto probe = probes.at(probeIndex++);
            const bool destination = probe.kind == ProbeKind::Destination;
            auto *check = KIO::stat(probe.url, destination ? KIO::StatJob::DestinationSide : KIO::StatJob::SourceSide,
                                    destination ? KIO::StatDetails(KIO::StatBasic) : KIO::StatDetails(KIO::StatBasic | KIO::StatInode),
                                    KIO::HideProgressInfo);
            check->setUiDelegate(nullptr);
            checks.insert(check);
            QObject::connect(check, &KJob::result, q, [this, check, probe, destination](KJob *) {
                checks.remove(check);
                if (!running) return;
                if (!destination && check->error()) {
                    emit q->error(check);
                    finish(check->error() == KIO::ERR_USER_CANCELED);
                    return;
                }
                if (destination && !check->error()) {
                    failValidation(QStringLiteral("“%1” already exists in its folder. Choose a different name.").arg(probe.url.fileName()));
                    return;
                }
                if (destination && check->error() != KIO::ERR_DOES_NOT_EXIST) {
                    emit q->error(check);
                    finish(check->error() == KIO::ERR_USER_CANCELED);
                    return;
                }
                if (!destination) {
                    const auto &entry = check->statResult();
                    if (!entry.contains(KIO::UDSEntry::UDS_DEVICE_ID)) {
                        failValidation(QStringLiteral("Cannot verify that “%1” can be renamed without copying its contents.").arg(probe.url.fileName()));
                        return;
                    }
                    devices.insert(identity(probe.url), entry.numberValue(KIO::UDSEntry::UDS_DEVICE_ID));
                }
                inspect();
            });
            emit q->jobCreated(check);
        }
        if (checks.isEmpty() && probeIndex == probes.size()) {
            // KF6.2 moveAs falls back to copying for EXDEV. A selected mount
            // point can hit that even when its name stays in the same parent.
            // Check devices asynchronously and refuse that case before dispatch.
            for (const auto &entry : plan.entries) {
                if (entry.changed && devices.value(identity(entry.item.url)) != devices.value(parentUrl(entry.item.url))) {
                    failValidation(QStringLiteral("“%1” is on a different filesystem from its parent. Rename its volume through the system tools.").arg(entry.oldName));
                    return;
                }
            }
            emit q->statusChanged(QStringLiteral("Renaming…"));
            const auto scheduledGeneration = generation;
            QTimer::singleShot(0, q, [this, scheduledGeneration] {
                if (generation == scheduledGeneration) next();
            });
        }
    }
    void schedule()
    {
        // Each connected component of a one-to-one filename mapping is a
        // chain or cycle. Reverse chains free each next target; a cycle uses
        // a UUID sibling, then the same ordered renames. O(n) scheduling.
        QHash<QUrl, int> sources;
        QHash<int, int> predecessor;
        QSet<int> pending;
        for (int i = 0; i < plan.entries.size(); ++i)
            if (plan.entries.at(i).changed) { sources.insert(identity(plan.entries.at(i).item.url), i); pending.insert(i); }
        for (int i : std::as_const(pending)) {
            const auto next = sources.constFind(identity(plan.entries.at(i).destination));
            if (next != sources.cend()) predecessor.insert(next.value(), i);
        }
        for (int i = 0; i < plan.entries.size(); ++i) {
            if (!pending.contains(i) || sources.contains(identity(plan.entries.at(i).destination))) continue;
            int index = i;
            while (pending.remove(index)) {
                steps.append({index, plan.entries.at(index).item.url, plan.entries.at(index).destination, false, false});
                if (!predecessor.contains(index)) break;
                index = predecessor.value(index);
            }
            steps.last().groupEnd = true;
        }
        for (int i = 0; i < plan.entries.size(); ++i) {
            if (!pending.contains(i)) continue;
            QUrl temporary;
            do { temporary = namedUrl(plan.entries.at(i).item.url,
                    QStringLiteral(".finder-rename-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces))); }
            while (reserved.contains(identity(temporary)));
            reserved.insert(identity(temporary));
            probes.append({temporary, ProbeKind::Destination});
            steps.append({i, plan.entries.at(i).item.url, temporary, true, false});
            pending.remove(i);
            int index = predecessor.value(i, -1);
            while (index != i && pending.remove(index)) {
                steps.append({index, plan.entries.at(index).item.url, plan.entries.at(index).destination, false, false});
                index = predecessor.value(index, -1);
            }
            steps.append({i, temporary, plan.entries.at(i).destination, false, true});
        }
    }
    void next()
    {
        if (!running || move) return;
        if ((cancelled && !groupActive) || stepIndex == steps.size()) { finish(cancelled); return; }
        const auto step = steps.at(stepIndex);
        const bool allowSkip = !groupActive && step.groupEnd;
        groupActive = true;
        auto *handler = new RenameHandler(parent, allowSkip, [this, step](const QUrl &url) {
            return identity(url) == identity(step.to) || !reserved.contains(identity(url));
        });
        auto *job = KIO::moveAs(step.from, step.to, KIO::HideProgressInfo);
        move = job;
        job->setUiDelegate(new RenameDelegate(parent, handler));
        // Recorded per step, never a synthetic FileUndoManager command. This
        // ensures successful steps remain undoable if later work fails/cancels.
        KIO::FileUndoManager::self()->recordJob(KIO::FileUndoManager::Rename, {step.from}, step.to, job);
        QObject::connect(job, &KIO::CopyJob::copyingDone, q,
            [this, step](KIO::Job *, const QUrl &, const QUrl &actual, const QDateTime &, bool, bool) {
                current[step.index] = actual;
                reserved.insert(identity(actual));
                if (!step.temporary) {
                    ++completed;
                    emit q->itemRenamed(plan.entries.at(step.index).item.url, actual);
                    emit q->progressChanged(completed, plan.changedCount);
                }
            });
        QObject::connect(job, &KJob::result, q, [this, job, step](KJob *) {
            move.clear();
            if (!running) return;
            if (job->error()) {
                if (job->error() != KIO::ERR_USER_CANCELED) emit q->error(job);
                finish(cancelled || job->error() == KIO::ERR_USER_CANCELED);
                return;
            }
            // Only independent items offer native Skip. Skipping must continue
            // with the next item, while dependent groups never expose Skip.
            if (identity(current.at(step.index)) == identity(step.from)) {
                if (!step.groupEnd || step.temporary) {
                    finish(cancelled);
                    return;
                }
                emit q->statusChanged(QStringLiteral("An item was skipped. Continuing with the remaining items…"));
            }
            ++stepIndex;
            groupActive = !step.groupEnd;
            const auto scheduledGeneration = generation;
            QTimer::singleShot(0, q, [this, scheduledGeneration] {
                if (generation == scheduledGeneration) next();
            });
        });
        emit q->jobCreated(job);
    }
};

BatchRenameEngine::BatchRenameEngine(QWidget *dialogParent, QObject *parent)
    : QObject(parent), d(std::make_unique<Private>(this, dialogParent)) {}
BatchRenameEngine::~BatchRenameEngine()
{
    for (auto *check : std::as_const(d->checks)) { check->disconnect(this); check->kill(KJob::Quietly); }
    // Successful steps were already recorded natively. Destruction (as opposed
    // to cooperative Cancel) cannot wait on work or keep a parent window alive.
    if (d->move) { d->move->disconnect(this); d->move->kill(KJob::Quietly); }
}
bool BatchRenameEngine::busy() const { return d->running; }
QList<QUrl> BatchRenameEngine::originalUrls() const
{
    QList<QUrl> result;
    for (const auto &entry : d->plan.entries) result.append(entry.item.url);
    return result;
}
QList<QUrl> BatchRenameEngine::currentUrls() const { return d->current; }
void BatchRenameEngine::start(const Plan &plan)
{
    if (d->running) return;
    QList<Item> items;
    QStringList names;
    for (const auto &entry : plan.entries) { items.append(entry.item); names.append(entry.newName); }
    d->plan = previewNames(items, names); // Never trust caller-supplied target URLs/counts.
    if (!plan.problem.isEmpty()) d->plan.problem = plan.problem;
    d->current.clear(); d->steps.clear(); d->probes.clear(); d->reserved.clear(); d->devices.clear();
    d->probeIndex = d->stepIndex = d->completed = 0;
    d->cancelled = d->groupActive = false;
    for (const auto &entry : d->plan.entries) {
        d->current.append(entry.item.url);
        d->reserved.insert(identity(entry.item.url)); d->reserved.insert(identity(entry.destination));
    }
    d->running = true;
    const auto generation = ++d->generation;
    emit busyChanged(true);
    QTimer::singleShot(0, this, [this, generation] {
        if (!d->running || d->generation != generation) return;
        if (!d->plan.canRename()) { d->failValidation(d->plan.problem); return; }
        QSet<QUrl> sources;
        for (const auto &entry : d->plan.entries) sources.insert(identity(entry.item.url));
        QSet<QUrl> checkedParents;
        for (const auto &entry : d->plan.entries) {
            if (!entry.changed) continue;
            d->probes.append({entry.item.url, Private::ProbeKind::Source});
            const auto parent = parentUrl(entry.item.url);
            if (!checkedParents.contains(parent)) {
                checkedParents.insert(parent);
                d->probes.append({parent, Private::ProbeKind::Parent});
            }
            if (!sources.contains(identity(entry.destination))) d->probes.append({entry.destination, Private::ProbeKind::Destination});
        }
        d->schedule();
        emit statusChanged(QStringLiteral("Checking filenames…"));
        emit progressChanged(0, d->plan.changedCount);
        d->inspect();
    });
}
void BatchRenameEngine::cancel()
{
    if (!d->running) return;
    d->cancelled = true;
    emit statusChanged(QStringLiteral("Stopping after the current rename group…"));
    if (!d->move && !d->groupActive) d->finish(true);
}

struct BatchRenameDialog::Private {
    explicit Private(BatchRenameDialog *owner, const KFileItemList &selection)
        : q(owner), items(capture(selection)) {}
    BatchRenameDialog *q;
    const QList<Item> items;
    Options values;
    Plan plan;
    BatchRenameEngine *engine = nullptr;
    QComboBox *mode = nullptr, *placement = nullptr, *format = nullptr;
    QLineEdit *find = nullptr, *replacement = nullptr, *text = nullptr, *custom = nullptr;
    QSpinBox *start = nullptr;
    QStackedWidget *pages = nullptr;
    QWidget *controls = nullptr;
    QLabel *summary = nullptr, *status = nullptr, *example = nullptr;
    QTableWidget *table = nullptr;
    QProgressBar *progress = nullptr;
    QDialogButtonBox *buttons = nullptr;
    QPointer<QMessageBox> warning;
    bool setting = false, attempted = false, failed = false;

    void read()
    {
        values.mode = Mode(mode->currentIndex());
        values.find = find->text(); values.replacement = replacement->text(); values.text = text->text();
        values.placement = Placement(placement->currentIndex()); values.format = NameFormat(format->currentIndex());
        values.customName = custom->text(); values.startNumber = start->value();
    }
    void refresh()
    {
        if (setting || attempted) return;
        read();
        pages->setCurrentIndex(mode->currentIndex());
        placement->setVisible(values.mode != Mode::ReplaceText);
        const auto placementLabel = controls->findChild<QLabel *>(QStringLiteral("batchPlacementLabel"));
        placementLabel->setVisible(values.mode != Mode::ReplaceText);
        plan = FinderBatchRename::preview(items, values);
        table->setRowCount(int(plan.entries.size()));
        for (int i = 0; i < plan.entries.size(); ++i) {
            const auto &entry = plan.entries.at(i);
            QString state = entry.problem;
            if (state.isEmpty()) state = entry.changed ? QStringLiteral("Ready") : QStringLiteral("Unchanged");
            for (int column = 0; column < 3; ++column) {
                auto *cell = new QTableWidgetItem(column == 0 ? entry.oldName : column == 1 ? entry.newName : state);
                cell->setToolTip(column < 2 ? (column == 0 ? entry.item.url : entry.destination).toDisplayString(QUrl::PreferLocalFile) : state);
                table->setItem(i, column, cell);
            }
        }
        summary->setText(QStringLiteral("%1 of %2 items will be renamed.").arg(plan.changedCount).arg(items.size()));
        status->setText(plan.problem);
        example->setText(plan.entries.isEmpty() ? QString() : QStringLiteral("Example: %1").arg(plan.entries.first().newName));
        buttons->button(QDialogButtonBox::Ok)->setEnabled(plan.canRename());
    }
    void begin(const Plan &operation)
    {
        attempted = true;
        controls->setEnabled(false);
        buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
        buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Cancel"));
        progress->setRange(0, operation.changedCount);
        progress->setValue(0);
        progress->show();
        engine->start(operation);
    }
};

BatchRenameDialog::BatchRenameDialog(const KFileItemList &items, QWidget *parent)
    : QDialog(parent), d(std::make_unique<Private>(this, items))
{
    setObjectName(QStringLiteral("finderBatchRename"));
    setWindowTitle(QStringLiteral("Rename Finder Items"));
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose);
    resize(660, 440);
    auto *layout = new QVBoxLayout(this);
    auto *heading = new QLabel(QStringLiteral("Rename %1 Items").arg(items.size()), this);
    QFont headingFont = heading->font(); headingFont.setBold(true); heading->setFont(headingFont);
    layout->addWidget(heading);
    d->controls = new QWidget(this);
    auto *controlsLayout = new QFormLayout(d->controls);
    d->mode = new QComboBox(d->controls);
    d->mode->setObjectName(QStringLiteral("batchRenameMode"));
    d->mode->addItems({QStringLiteral("Replace Text"), QStringLiteral("Add Text"), QStringLiteral("Format")});
    controlsLayout->addRow(QStringLiteral("Rename using:"), d->mode);
    d->pages = new QStackedWidget(d->controls);
    auto *replacePage = new QWidget(d->pages);
    auto *replaceForm = new QFormLayout(replacePage); replaceForm->setContentsMargins(0, 0, 0, 0);
    d->find = new QLineEdit(replacePage); d->find->setObjectName(QStringLiteral("batchFindText"));
    d->replacement = new QLineEdit(replacePage); d->replacement->setObjectName(QStringLiteral("batchReplacementText"));
    replaceForm->addRow(QStringLiteral("Find:"), d->find);
    replaceForm->addRow(QStringLiteral("Replace with:"), d->replacement);
    d->pages->addWidget(replacePage);
    auto *addPage = new QWidget(d->pages);
    auto *addForm = new QFormLayout(addPage); addForm->setContentsMargins(0, 0, 0, 0);
    d->text = new QLineEdit(addPage); d->text->setObjectName(QStringLiteral("batchAddText"));
    addForm->addRow(QStringLiteral("Add:"), d->text);
    d->pages->addWidget(addPage);
    auto *formatPage = new QWidget(d->pages);
    auto *formatForm = new QFormLayout(formatPage); formatForm->setContentsMargins(0, 0, 0, 0);
    d->format = new QComboBox(formatPage); d->format->setObjectName(QStringLiteral("batchNameFormat"));
    d->format->addItems({QStringLiteral("Name and Index"), QStringLiteral("Name and Counter"), QStringLiteral("Name and Date")});
    d->custom = new QLineEdit(d->values.customName, formatPage); d->custom->setObjectName(QStringLiteral("batchCustomFormat"));
    d->start = new QSpinBox(formatPage); d->start->setObjectName(QStringLiteral("batchStartNumber"));
    d->start->setRange(0, std::numeric_limits<int>::max()); d->start->setValue(1);
    formatForm->addRow(QStringLiteral("Name Format:"), d->format);
    formatForm->addRow(QStringLiteral("Custom Format:"), d->custom);
    formatForm->addRow(QStringLiteral("Start numbers at:"), d->start);
    d->pages->addWidget(formatPage);
    controlsLayout->addRow(d->pages);
    d->placement = new QComboBox(d->controls); d->placement->setObjectName(QStringLiteral("batchTextPlacement"));
    d->placement->addItems({QStringLiteral("Before name"), QStringLiteral("After name")}); d->placement->setCurrentIndex(1);
    auto *placementLabel = new QLabel(QStringLiteral("Where:"), d->controls);
    placementLabel->setObjectName(QStringLiteral("batchPlacementLabel")); placementLabel->setBuddy(d->placement);
    controlsLayout->addRow(placementLabel, d->placement);
    layout->addWidget(d->controls);
    d->example = new QLabel(this); d->example->setTextFormat(Qt::PlainText); d->example->setWordWrap(true);
    layout->addWidget(d->example);
    d->table = new QTableWidget(0, 3, this); d->table->setObjectName(QStringLiteral("batchRenamePreview"));
    d->table->setHorizontalHeaderLabels({QStringLiteral("Current Name"), QStringLiteral("New Name"), QStringLiteral("Status")});
    d->table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    d->table->setSelectionBehavior(QAbstractItemView::SelectRows);
    d->table->setSelectionMode(QAbstractItemView::SingleSelection);
    d->table->setAlternatingRowColors(true); d->table->verticalHeader()->hide();
    d->table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    d->table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    d->table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    d->table->setAccessibleName(QStringLiteral("Filename changes preview"));
    layout->addWidget(d->table, 1);
    d->summary = new QLabel(this); layout->addWidget(d->summary);
    d->status = new QLabel(this); d->status->setObjectName(QStringLiteral("batchRenameStatus"));
    d->status->setTextFormat(Qt::PlainText); d->status->setWordWrap(true); d->status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(d->status);
    d->progress = new QProgressBar(this); d->progress->setObjectName(QStringLiteral("batchRenameProgress"));
    d->progress->hide(); layout->addWidget(d->progress);
    d->buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    d->buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Rename"));
    d->buttons->button(QDialogButtonBox::Ok)->setDefault(true);
    layout->addWidget(d->buttons);
    d->engine = new BatchRenameEngine(this, this);
    connect(d->buttons, &QDialogButtonBox::accepted, this, &BatchRenameDialog::accept);
    connect(d->buttons, &QDialogButtonBox::rejected, this, &BatchRenameDialog::reject);
    for (auto *edit : {d->find, d->replacement, d->text, d->custom})
        connect(edit, &QLineEdit::textChanged, this, [this] { d->refresh(); });
    for (auto *combo : {d->mode, d->placement, d->format})
        connect(combo, &QComboBox::currentIndexChanged, this, [this] { d->refresh(); });
    connect(d->start, &QSpinBox::valueChanged, this, [this] { d->refresh(); });
    connect(d->engine, &BatchRenameEngine::statusChanged, d->status, &QLabel::setText);
    connect(d->engine, &BatchRenameEngine::validationFailed, this, [this](const QString &message) {
        d->failed = true; d->status->setText(message);
    });
    connect(d->engine, &BatchRenameEngine::progressChanged, this, [this](int count, int) { d->progress->setValue(count); });
    connect(d->engine, &BatchRenameEngine::error, this, [this](KJob *job) {
        d->failed = true; d->status->setText(job->errorString()); emit error(job);
    });
    connect(d->engine, &BatchRenameEngine::finished, this, [this](const QList<QUrl> &urls, bool cancelled) {
        emit renamingFinished(urls);
        const auto current = d->engine->currentUrls();
        int actual = 0;
        for (int i = 0; i < d->plan.entries.size(); ++i) {
            const auto &entry = d->plan.entries.at(i);
            if (i >= current.size()) break;
            d->table->item(i, 1)->setText(identity(current.at(i)).fileName());
            const bool changed = identity(current.at(i)) != identity(entry.item.url);
            if (changed) ++actual;
            const bool temporary = identity(current.at(i)).fileName().startsWith(QLatin1String(".finder-rename-"));
            d->table->item(i, 2)->setText(temporary ? QStringLiteral("Temporary name — use Undo")
                : changed ? QStringLiteral("Renamed") : entry.changed ? QStringLiteral("Not renamed") : QStringLiteral("Unchanged"));
        }
        if (cancelled || (!d->failed && actual == d->plan.changedCount)) {
            if (cancelled) QDialog::reject(); else QDialog::accept();
        } else {
            // Keep a failed/partial result readable. Reusing captured original
            // URLs would risk renaming replacements, so another attempt needs a
            // new dialog/selection. The successful subset is emitted above.
            d->progress->hide();
            d->summary->setText(QStringLiteral("%1 of %2 items were renamed. Close this dialog before starting another batch.").arg(actual).arg(d->plan.changedCount));
            d->buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Close"));
        }
    });
    d->refresh();
    d->find->setFocus();
}
BatchRenameDialog::~BatchRenameDialog() = default;
QList<QUrl> BatchRenameDialog::originalUrls() const
{
    QList<QUrl> result; for (const auto &item : d->items) result.append(item.url); return result;
}
Options BatchRenameDialog::options() const { return d->values; }
Plan BatchRenameDialog::preview() const { return d->plan; }
bool BatchRenameDialog::busy() const { return d->engine->busy() || !d->warning.isNull(); }
void BatchRenameDialog::setOptions(const Options &options)
{
    if (d->attempted || d->warning) return;
    d->setting = true; d->values = options;
    d->mode->setCurrentIndex(int(options.mode)); d->placement->setCurrentIndex(int(options.placement));
    d->format->setCurrentIndex(int(options.format)); d->find->setText(options.find); d->replacement->setText(options.replacement);
    d->text->setText(options.text); d->custom->setText(options.customName); d->start->setValue(options.startNumber);
    d->setting = false; d->refresh();
}
void BatchRenameDialog::accept()
{
    if (d->attempted || d->warning || !d->plan.canRename()) return;
    int changedExtensions = 0;
    for (const auto &entry : d->plan.entries) if (entry.changed && entry.extensionChanged) ++changedExtensions;
    if (d->values.mode == Mode::ReplaceText && changedExtensions > 0
        && QSettings().value(QStringLiteral("Advanced/warnExtensionChange"), true).toBool()) {
        const auto operation = d->plan;
        auto *warning = new QMessageBox(QMessageBox::Warning, QStringLiteral("Change Filename Extensions?"),
            QStringLiteral("Replacing this text changes the filename extension of %1 items. This may change which application opens them.").arg(changedExtensions),
            QMessageBox::NoButton, this);
        warning->setObjectName(QStringLiteral("finderBatchExtensionWarning"));
        warning->setAttribute(Qt::WA_DeleteOnClose);
        auto *change = warning->addButton(QStringLiteral("Use New Extensions"), QMessageBox::AcceptRole);
        auto *review = warning->addButton(QStringLiteral("Review Names"), QMessageBox::RejectRole);
        warning->setDefaultButton(review);
        warning->addButton(QMessageBox::Cancel);
        d->warning = warning; d->controls->setEnabled(false);
        connect(warning, &QDialog::finished, this, [this, warning, change, operation] {
            const bool confirmed = warning->clickedButton() == change;
            d->warning.clear(); d->controls->setEnabled(true);
            if (confirmed) d->begin(operation);
        });
        warning->open();
        return;
    }
    d->begin(d->plan);
}
void BatchRenameDialog::reject()
{
    if (d->engine->busy()) { d->engine->cancel(); return; }
    if (d->warning) { d->warning->reject(); return; }
    QDialog::reject();
}
void BatchRenameDialog::closeEvent(QCloseEvent *event)
{
    if (d->engine->busy()) { d->engine->cancel(); event->ignore(); return; }
    if (d->warning) { d->warning->reject(); event->ignore(); return; }
    QDialog::closeEvent(event);
}
