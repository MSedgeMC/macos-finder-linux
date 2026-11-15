// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#pragma once

#include <QList>
#include <QUrl>
#include <QWidget>
#include <memory>

class QAbstractItemModel;
class QAbstractItemView;
class QItemSelectionModel;
class QMimeData;
class QSortFilterProxyModel;
class QTreeView;

// Flat search hits: column 0 Name/Decoration/Qt::UserRole QUrl, 1 Kind,
// 2 Location. The model is borrowed; it is never edited or reparented here.
// All view indexes and the shared selection belong to proxyModel(). Hosts
// needing source indexes must use proxyModel()->mapToSource().
class SearchResults : public QWidget {
    Q_OBJECT
public:
    enum Mode { Icons = 0, List = 1, Columns = 2, Gallery = 3 };
    explicit SearchResults(QWidget *parent = nullptr);
    ~SearchResults() override;

    void setModel(QAbstractItemModel *model);
    QAbstractItemModel *sourceModel() const;
    QSortFilterProxyModel *proxyModel() const;
    void setMode(int mode);
    int mode() const;
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder);
    QAbstractItemView *activeView() const;
    QTreeView *primaryView() const; // finderSearchResults compatibility view
    QItemSelectionModel *selectionModel() const; // stable across model/mode changes
    QList<QUrl> selectedUrls() const; // visual row order; exact, deduplicated URLs
    void setUrls(const QList<QUrl> &urls, const QUrl &primary = QUrl());
    QMimeData *mimeData() const; // caller owns; native text/uri-list only

    // Empty, laid-out host for an existing PreviewPane, owned by this widget.
    // The host connects selectionChanged() and forwards selectedUrls() itself.
    QWidget *galleryHost() const;
    QWidget *host() const;
    // Call on external source-content changes not signalled by the model.
    void refreshThumbnails();

signals:
    void activated(const QUrl &url);
    void quickLookRequested(const QList<QUrl> &urls);
    void contextMenuRequested(const QPoint &globalPos);
    void selectionChanged();
    void renameRequested();

protected:
    bool eventFilter(QObject *object, QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    class Private;
    std::unique_ptr<Private> d;
};
