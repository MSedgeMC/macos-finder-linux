// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#pragma once

#include <QWidget>
#include <QUrl>
#include <QVariant>
#include <KFileItem>
#include <memory>

class QAbstractItemView;
class QDropEvent;
class QMenu;
class QMimeData;
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
    // The first seven IDs deliberately match KDirModel::ModelColumns. Use
    // these APIs, rather than KDirModel::ColumnCount, for List view controls.
    enum ListColumn {
        NameColumn = 0, SizeColumn, DateModifiedColumn, PermissionsColumn,
        OwnerColumn, GroupColumn, KindColumn, DateCreatedColumn, DateAddedColumn,
        LastOpenedColumn, TagsColumn, ListColumnCount
    };
    Q_ENUM(ListColumn)
    // Available on activeView()->model() indexes in every view. Invalid
    // QVariant means unavailable/pending, never a substituted timestamp.
    // RawValueRole: QDateTime for dates, qulonglong for complete logical byte
    // sizes, QStringList for Tags; native columns retain their existing roles.
    // AccessTimeRole is filesystem access time, explicitly NOT Last Opened.
    // TagColorsRole is QVariantList of QColor, parallel to TagsRole.
    enum MetadataRole {
        TagsRole = Qt::UserRole + 131, TagColorsRole = Qt::UserRole + 134,
        RawValueRole, DateCreatedRole, DateAddedRole, LastOpenedRole,
        LogicalSizeRole, ModificationTimeRole, AccessTimeRole,
        MetadataPendingRole, MetadataErrorRole, DirectorySizeStateRole,
        CalculatedBytesRole
    };
    Q_ENUM(MetadataRole)
    enum SizeState { SizeNotCalculated, SizeCalculating, SizeComplete, SizeIncomplete, SizeUnavailable };
    Q_ENUM(SizeState)
    struct SizeCalculationLimits {
        quint64 maxEntries = 250000;
        int maxDepth = 64;
        int maxMilliseconds = 15000;
    };
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
    // Icons accepts sort column -1 (None). Grouped/sorted surfaces retain
    // the manual layout and restore it when Icons returns to None/ungrouped.
    bool snapToGrid() const;
    bool canArrangeIcons() const;
    QPoint iconPosition(const QUrl &url) const;
    // Caller owns the native URL MIME. Origin is in active viewport coordinates;
    // includes the same private arrangement payload as native view dragging.
    QMimeData *createDragMimeData(const QPoint &origin) const;
    bool springLoadingActive() const;
    bool sortAscending() const;
    int listColumnCount() const;
    QString listColumnTitle(int column) const;
    bool listColumnVisible(int column) const;
    // Returns visible logical IDs in their current visual header order.
    QList<int> visibleListColumns() const;
    bool sizeCalculationRunning() const;
    // Cached lookup only. An invalid value is unavailable or still pending.
    QVariant metadata(const QUrl &url, int role) const;
    QWidget *galleryPreviewHost() const;
    QWidget *columnPreviewHost() const;
    QWidget *previewHost() const;
    KDirModel *directoryModel() const;
    QAbstractItemView *activeView() const;
    static QString confirmedRenameName(const KFileItem &item, const QString &name, QWidget *parent);
public slots:
    void navigate(const QUrl &url);
    // For an already worker-validated location. KDirLister remains the final
    // authority and reports listing errors; this skips GUI-thread local stat.
    void navigateValidated(const QUrl &url);
    void setMode(Mode mode);
    void setHiddenFiles(bool show);
    void setFilter(const QString &filter);
    void setSort(int column, bool ascending);
    void setSnapToGrid(bool enabled);
    void setIconPosition(const QUrl &url, const QPoint &position);
    void cleanUp();
    void cleanUpBy(int column, bool ascending = true);
    void setSpringLoadingEnabled(bool enabled);
    void setSpringLoadingDelay(int milliseconds);
    void cancelSpringLoading();
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
    // Refresh bounded asynchronous per-inode extension/comment metadata.
    void refreshFileDetails();
    void setFoldersFirst(bool enabled);
    void selectUrls(const QList<QUrl> &urls);
    void renameSelected();
    void newFolder();
    void newFolderWithSelection();
    void autoSizeListColumns(int column = -1);
    // Name always remains visible. Visibility/order/width persist in view state.
    void setListColumnVisible(int column, bool visible);
    // Explicit, one-worker scan of loaded direct child folders in the current
    // location (including filtered/hidden loaded rows), at most 4096 roots.
    // Sums regular-file logical lengths, including hidden files, without
    // following symlinks or crossing devices. Incomplete totals remain marked
    // partial and are not exposed as LogicalSizeRole. Bounds apply to the whole
    // request; navigation/real listing changes/destruction cancel stale work.
    // Calling while running is a no-op; cancel is asynchronous and nonblocking.
    void calculateAllSizes();
    void calculateAllSizes(SizeCalculationLimits limits);
    void cancelSizeCalculation();
    void refreshMetadata();
    void copySelected(bool cut = false);
    void paste(bool move = false);
    void trashSelected();
    void duplicateSelected();
    void compressSelected();
    void undo();
    void drop(QDropEvent *event, QUrl destination = {});
signals:
    // Emitted synchronously before exec(); the menu is stack-owned and valid
    // only during this call. Connect directly to append shared app actions.
    void contextMenuRequested(QMenu *menu, const KFileItemList &items);
    void listColumnsChanged();
    void iconArrangementChanged();
    void springLoadingChanged(bool active);
    // Temporary hover/rollback navigation: update path/status only. Do not
    // record history or resolve per-folder state until ordinary urlChanged.
    void springUrlChanged(const QUrl &url);
    void sizeCalculationRunningChanged(bool running);
    void sizeCalculationProgress(quint64 entriesVisited, int foldersCompleted, int totalFolders);
    void sizeCalculationFinished(bool canceled, bool limited, const QString &message);
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
