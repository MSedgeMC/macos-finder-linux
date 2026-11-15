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
    explicit Browser(QWidget *parent = nullptr);
    ~Browser() override;
    QUrl url() const;
    QList<QUrl> selectedUrls() const;
    KFileItemList selectedItems() const;
    int itemCount() const;
    Mode mode() const;
    int iconSize() const;
    int sortColumn() const;
    bool sortAscending() const;
    QWidget *galleryPreviewHost() const;
    QWidget *columnPreviewHost() const;
    KDirModel *directoryModel() const;
    QAbstractItemView *activeView() const;
public slots:
    void navigate(const QUrl &url);
    void setMode(Mode mode);
    void setHiddenFiles(bool show);
    void setFilter(const QString &filter);
    void setSort(int column, bool ascending);
    void setIconSize(int size);
    void setFoldersFirst(bool enabled);
    void selectUrls(const QList<QUrl> &urls);
    void renameSelected();
    void newFolder();
    void copySelected(bool cut = false);
    void paste(bool move = false);
    void trashSelected();
    void duplicateSelected();
    void undo();
    void drop(QDropEvent *event, QUrl destination = {});
signals:
    void urlChanged(const QUrl &url);
    void selectionChanged(const KFileItemList &items);
    void countChanged(int count);
    void previewRequested(const QUrl &url);
    void quickLookRequested(const QList<QUrl> &urls);
    void openRequested(const QUrl &url);
    void statusMessage(const QString &message);
    void errorOccurred(const QString &message);
protected:
    bool eventFilter(QObject *object, QEvent *event) override;
private:
    class Private;
    std::unique_ptr<Private> d;
};
