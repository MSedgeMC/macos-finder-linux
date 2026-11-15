// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#pragma once

#include <QWidget>
#include <QUrl>
#include <KFileItem>
#include <memory>

class QAbstractItemView;
class QDropEvent;
class KDirModel;

class Browser : public QWidget {
    Q_OBJECT
public:
    enum Mode { Icons, List, Columns, Gallery };
    Q_ENUM(Mode)
    enum Group { GroupNone, GroupName, GroupKind, GroupDateModified, GroupSize, GroupTags };
    Q_ENUM(Group)
    enum LabelPosition { LabelBelow, LabelRight };
    Q_ENUM(LabelPosition)
    explicit Browser(QWidget *parent = nullptr);
    ~Browser() override;
    QUrl url() const;
    QList<QUrl> selectedUrls() const;
    KFileItemList selectedItems() const;
    int itemCount() const;
    Mode mode() const;
    int iconSize() const;
    Group group() const;
    int gridSpacing() const;
    int textSize() const;
    LabelPosition labelPosition() const;
    bool iconPreviewsShown() const;
    bool previewShown() const;
    int previewWidth() const;
    QByteArray saveViewState() const;
    bool canCompressSelection() const;
    bool canNewFolderWithSelection() const;
    int sortColumn() const;
    bool sortAscending() const;
    QWidget *galleryPreviewHost() const;
    QWidget *columnPreviewHost() const;
    QWidget *previewHost() const;
    KDirModel *directoryModel() const;
    QAbstractItemView *activeView() const;
    static QString confirmedRenameName(const KFileItem &item, const QString &name, QWidget *parent);
public slots:
    void navigate(const QUrl &url);
    void setMode(Mode mode);
    void setHiddenFiles(bool show);
    void setFilter(const QString &filter);
    void setSort(int column, bool ascending);
    void setIconSize(int size);
    void setGroup(Group group);
    void setGridSpacing(int spacing);
    void setTextSize(int pixels);
    void setLabelPosition(LabelPosition position);
    void setIconPreviewsShown(bool shown);
    void setPreviewShown(bool shown);
    void setPreviewWidth(int width);
    void restoreViewState(const QByteArray &state);
    void refreshTags();
    void refreshPreferences();
    void setFoldersFirst(bool enabled);
    void selectUrls(const QList<QUrl> &urls);
    void renameSelected();
    void newFolder();
    void newFolderWithSelection();
    void autoSizeListColumns(int column = -1);
    void copySelected(bool cut = false);
    void paste(bool move = false);
    void trashSelected();
    void duplicateSelected();
    void compressSelected();
    void undo();
    void drop(QDropEvent *event, QUrl destination = {});
signals:
    void urlChanged(const QUrl &url);
    void selectionChanged(const KFileItemList &items);
    void countChanged(int count);
    void previewRequested(const QUrl &url);
    void quickLookRequested(const QList<QUrl> &urls);
    void openRequested(const QUrl &url);
    void folderOpenRequested(const QUrl &url, Qt::KeyboardModifiers modifiers);
    void statusMessage(const QString &message);
    void errorOccurred(const QString &message);
protected:
    bool eventFilter(QObject *object, QEvent *event) override;
private:
    class Private;
    std::unique_ptr<Private> d;
};
