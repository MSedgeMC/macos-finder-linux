// SPDX-License-Identifier: MIT
#pragma once
#include <QDialog>
#include "preferences.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QCloseEvent;
class QShowEvent;


class FinderSettings : public QDialog {
    Q_OBJECT
public:
    explicit FinderSettings(QWidget *parent = nullptr);
public slots:
    void reject() override;
    void reloadPreferences();
    // Call on the GUI thread after applying oldName -> newName (empty means
    // deletion) to known local URLs through FileTags. A nonempty error keeps
    // the registry unchanged; include partial-write details in that message.
    void completeTagMetadataChange(const QString &requestId, const QString &error);
signals:
    void preferencesChanged();
    void tagMetadataChangeRequested(const QString &requestId, const QString &tagId,
                                    const QString &oldName, const QString &newName);
protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;
private:
    void reloadTags();
    void updateTagSelection();
    void requestTagMetadataChange(bool remove);
    QListWidget *m_tagList = nullptr;
    QListWidget *m_favoriteList = nullptr;
    QLineEdit *m_tagName = nullptr;
    QComboBox *m_tagColor = nullptr;
    QLabel *m_tagError = nullptr;
    QPushButton *m_addTag = nullptr;
    QPushButton *m_renameTag = nullptr;
    QPushButton *m_deleteTag = nullptr;
    QPushButton *m_assignFavorite = nullptr;
    QPushButton *m_clearFavorite = nullptr;
    QPushButton *m_favoriteUp = nullptr;
    QPushButton *m_favoriteDown = nullptr;
    QString m_pendingRequest;
    QString m_pendingTagId;
    QString m_pendingOldName;
    QString m_pendingNewName;
};
