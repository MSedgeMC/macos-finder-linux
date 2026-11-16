// SPDX-License-Identifier: MIT
#pragma once
#include <KPropertiesDialog>
#include <QPointer>

class FileDetailsDialog;

// Native properties retains its own providers. Explicit metadata saves keep
// this host alive until their asynchronous completion has been delivered.
class FinderInfoDialog : public KPropertiesDialog {
    Q_OBJECT
public:
    explicit FinderInfoDialog(const QUrl &url, QWidget *parent = nullptr);
    explicit FinderInfoDialog(const KFileItem &item, QWidget *parent = nullptr);
    FileDetailsDialog *details() const;
    bool busy() const;
    bool canRename() const;
    void setRenameBusy(bool busy);
    void explainBusy();
    bool selectPage(const QString &name);
public slots:
    void accept() override;
    void reject() override;
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    void initialize();
    QPointer<FileDetailsDialog> m_details;
    bool m_renaming = false;
};
