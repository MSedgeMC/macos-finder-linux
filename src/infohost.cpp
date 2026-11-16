// SPDX-License-Identifier: MIT
#include "infohost.h"
#include "filedetails.h"
#include <KPropertiesDialogPlugin>
#include <KPageWidgetItem>
#include <KPageWidget>
#include <KPageWidgetModel>
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QMimeDatabase>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QTextEdit>

FinderInfoDialog::FinderInfoDialog(const QUrl &url, QWidget *parent) : KPropertiesDialog(url, parent) { initialize(); }
FinderInfoDialog::FinderInfoDialog(const KFileItem &item, QWidget *parent) : KPropertiesDialog(item, parent) { initialize(); }
void FinderInfoDialog::initialize() {
    setAttribute(Qt::WA_DeleteOnClose);
    if (!url().isLocalFile()) return;
    resize(qMax(width(), 500), qMax(height(), 640));
    auto *page = new QScrollArea(this);
    page->setWidgetResizable(true); page->setFrameShape(QFrame::NoFrame);
    m_details = new FileDetailsDialog(page);
    m_details->setWindowFlags(Qt::Widget);
    if (auto *box = m_details->findChild<QDialogButtonBox *>()) {
        if (auto *close = box->button(QDialogButtonBox::Close)) close->hide();
        if (auto *properties = m_details->findChild<QPushButton *>("fileDetailsNativeProperties")) properties->hide();
    }
    m_details->setExtensionPresentation(QMimeDatabase().suffixForFileName(url().fileName()),
        QSettings().value("Advanced/showAllExtensions", false).toBool());
    m_details->setUrl(url()); page->setWidget(m_details);
    auto *item = addPage(page, tr("Name & Comments")); item->setObjectName("finderFileDetailsItem");
    connect(m_details, &FileDetailsDialog::closeRequested, this, &FinderInfoDialog::reject);
}
bool FinderInfoDialog::busy() const { return m_renaming || (m_details && m_details->metadataCache()->isWriting()); }
FileDetailsDialog *FinderInfoDialog::details() const { return m_details; }
bool FinderInfoDialog::selectPage(const QString &name) {
    if (auto *model = qobject_cast<KPageWidgetModel *>(pageWidget()->model()))
        for (int row = 0; row < model->rowCount(); ++row) {
            auto *item = model->item(model->index(row,0));
            if (item && QString(item->name()).remove('&') == name) { setCurrentPage(item); return true; }
        }
    return false;
}
bool FinderInfoDialog::canRename() const {
    if (busy() || !m_details) return false;
    for (const auto *plugin : findChildren<KPropertiesDialogPlugin *>()) if (plugin->isDirty()) return false;
    const auto metadata = m_details->metadataCache()->cached(url());
    const auto *draft = m_details->findChild<QTextEdit *>("fileDetailsComment");
    return metadata && metadata->available() && (!draft || draft->toPlainText() == metadata->comment);
}
void FinderInfoDialog::setRenameBusy(bool busy) {
    m_renaming = busy;
    if (m_details) m_details->setEnabled(!busy);
    if (buttonBox()) buttonBox()->setEnabled(!busy);
}
void FinderInfoDialog::explainBusy() {
    if (m_details) m_details->showOperationMessage(m_renaming ? tr("Wait for the file rename to finish before closing Get Info.")
        : tr("A metadata save is in progress. Wait for its result before closing Get Info."));
}
void FinderInfoDialog::accept() { if (busy()) { explainBusy(); return; } KPropertiesDialog::accept(); }
void FinderInfoDialog::reject() { if (busy()) { explainBusy(); return; } KPropertiesDialog::reject(); }
void FinderInfoDialog::closeEvent(QCloseEvent *event) {
    if (busy()) { explainBusy(); event->ignore(); return; }
    KPropertiesDialog::closeEvent(event);
}
