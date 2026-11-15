// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#include "browser.h"
#include "places.h"
#include "preview.h"
#include "search.h"
#include "tags.h"
#include <KDirLister>
#include <KDirModel>
#include <KFilePlacesModel>
#include <KFilePlacesView>
#include <KIO/ApplicationLauncherJob>
#include <KIO/CopyJob>
#include <KIO/FileUndoManager>
#include <KIO/JobUiDelegateFactory>
#include <KIO/OpenUrlJob>
#include <KOpenWithDialog>
#include <KPropertiesDialog>
#include <KService>
#include <KWindowEffects>
#include <QAbstractItemView>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QColumnView>
#include <QComboBox>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGraphicsEffect>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QMimeDatabase>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRegion>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTabBar>
#include <QTabWidget>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWindow>
#include <cmath>
#include <mutex>

// Cached with widget repaints; no timer or wallpaper capture is required.
static QPainterPath windowContour(QRectF rect, qreal radius) {
  QPainterPath path;
  if (radius <= 0) {
    path.addRect(rect);
    return path;
  }
  const qreal r = qMin(radius, qMin(rect.width(), rect.height()) / 2);
  const QPointF centers[] = {{rect.right() - r, rect.top() + r},
                             {rect.right() - r, rect.bottom() - r},
                             {rect.left() + r, rect.bottom() - r},
                             {rect.left() + r, rect.top() + r}};
  for (int corner = 0; corner < 4; ++corner)
    for (int step = 0; step <= 32; ++step) {
      const double angle = (-90 + corner * 90 + step * 90.0 / 32) * M_PI / 180;
      const double c = std::cos(angle), s = std::sin(angle);
      QPointF point = centers[corner] +
                      QPointF(r * std::copysign(std::pow(std::abs(c), .62), c),
                              r * std::copysign(std::pow(std::abs(s), .62), s));
      if (corner == 0 && step == 0)
        path.moveTo(point);
      else
        path.lineTo(point);
    }
  path.closeSubpath();
  return path;
}
class ContourEffect : public QGraphicsEffect {
public:
  using QGraphicsEffect::QGraphicsEffect;

protected:
  void draw(QPainter *p) override {
    QPoint origin;
    QPixmap source = sourcePixmap(Qt::LogicalCoordinates, &origin, NoPad);
    if (source.isNull())
      return;
    QPixmap mask(source.size());
    mask.setDevicePixelRatio(source.devicePixelRatio());
    mask.fill(Qt::transparent);
    QPainter m(&mask);
    m.setRenderHint(QPainter::Antialiasing);
    m.setPen(Qt::NoPen);
    m.setBrush(Qt::white);
    const auto *widget = qobject_cast<QWidget *>(parent());
    m.drawPath(
        windowContour(QRectF(QPointF(0, 0), source.deviceIndependentSize()),
                      widget && widget->window()->isMaximized() ? 0 : 16));
    m.end();
    QPainter clipped(&source);
    clipped.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    clipped.drawPixmap(0, 0, mask);
    clipped.end();
    p->drawPixmap(origin, source);
  }
};

static QIcon symbol(const QString &name) {
  QPixmap pix(40, 40);
  pix.fill(Qt::transparent);
  pix.setDevicePixelRatio(2);
  QPainter p(&pix);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(
      QPen(QColor("#e8e8ed"), 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  if (name == "back" || name == "forward") {
    int a = name == "back" ? 13 : 7, b = name == "back" ? 7 : 13;
    p.drawPolyline(QPolygonF{{qreal(a), 4}, {qreal(b), 10}, {qreal(a), 16}});
  } else if (name == "icons")
    for (int y : {4, 12})
      for (int x : {4, 12})
        p.drawRoundedRect(QRectF(x, y, 4, 4), .7, .7);
  else if (name == "list")
    for (int y : {5, 10, 15}) {
      p.drawPoint(4, y);
      p.drawLine(8, y, 16, y);
    }
  else if (name == "columns")
    for (int x : {3, 8, 13})
      p.drawRoundedRect(QRectF(x, 4, 4, 12), .5, .5);
  else if (name == "gallery") {
    p.drawRoundedRect(QRectF(3, 4, 14, 9), 1, 1);
    for (int x : {4, 9, 14})
      p.drawRect(QRectF(x, 15, 2, 2));
  } else if (name == "sort") {
    for (int y : {5, 10, 15}) {
      p.drawRect(QRectF(3, y, 3, 2));
      p.drawLine(9, y + 1, 16, y + 1);
    }
  } else if (name == "search") {
    p.drawEllipse(QRectF(3, 3, 10, 10));
    p.drawLine(12, 12, 17, 17);
  } else if (name == "share") {
    p.drawPolyline(QPolygonF{{5, 9}, {5, 16}, {15, 16}, {15, 9}});
    p.drawLine(10, 12, 10, 2);
    p.drawPolyline(QPolygonF{{6, 6}, {10, 2}, {14, 6}});
  } else if (name == "tags") {
    p.drawPolygon(QPolygonF{{3, 4}, {11, 4}, {17, 10}, {10, 17}, {3, 10}});
    p.drawEllipse(QPointF(6, 7), 1, 1);
  } else if (name == "more") {
    p.setBrush(QColor("#e8e8ed"));
    for (int x : {4, 10, 16})
      p.drawEllipse(QPointF(x, 10), .8, .8);
  } else if (name == "sidebar") {
    p.drawRoundedRect(QRectF(3, 4, 14, 12), 1, 1);
    p.drawLine(8, 4, 8, 16);
  } else if (name == "minimize")
    p.drawLine(4, 10, 16, 10);
  else if (name == "maximize")
    p.drawRect(QRectF(4, 4, 12, 12));
  else if (name == "close") {
    p.drawLine(5, 5, 15, 15);
    p.drawLine(15, 5, 5, 15);
  }
  return QIcon(pix);
}

class Material : public QWidget {
public:
  bool blur = false;
  using QWidget::QWidget;

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.fillRect(rect(), QColor(38, 38, 42, blur ? 178 : 250));
    if (objectName() == "finderHeader") {
      p.setPen(QColor(255, 255, 255, 26));
      p.drawLine(0, 0, width(), 0);
      p.setPen(QColor(0, 0, 0, 60));
      p.drawLine(0, height() - 1, width(), height() - 1);
    }
  }
};
class Header : public Material {
public:
  using Material::Material;

protected:
  void mousePressEvent(QMouseEvent *e) override {
    if (e->button() == Qt::LeftButton && window()->windowHandle())
      window()->windowHandle()->startSystemMove();
  }
  void mouseDoubleClickEvent(QMouseEvent *e) override {
    if (e->button() == Qt::LeftButton) {
      if (window()->isMaximized())
        window()->showNormal();
      else
        window()->showMaximized();
    }
  }
};

struct TabState {
  QList<QUrl> history;
  int historyIndex = -1;
  bool navigatingHistory = false;
  bool viewRestored = false;
  PreviewPane *gallery = nullptr, *column = nullptr;
};
class CapacityProbe : public QThread {
  Q_OBJECT
public:
  QUrl url;
  explicit CapacityProbe(QUrl u) : url(u) {}
signals:
  void available(QUrl, qint64);

protected:
  void run() override {
    QFile f("/proc/self/mountinfo");
    if (!f.open(QIODevice::ReadOnly))
      return;
    QByteArray path = QFile::encodeName(url.toLocalFile());
    int longest = -1;
    QByteArray type;
    for (auto line : f.readAll().split('\n')) {
      int cut = line.indexOf(" - ");
      if (cut < 0)
        continue;
      auto fields = line.left(cut).split(' ');
      if (fields.size() < 5)
        continue;
      QByteArray mount = fields[4];
      mount.replace("\\040", " ");
      if ((mount == "/" || path == mount || path.startsWith(mount + '/')) &&
          mount.size() > longest) {
        longest = mount.size();
        type = line.mid(cut + 3).split(' ').value(0);
      }
    }
    if (!QList<QByteArray>{"ext2", "ext3", "ext4", "btrfs", "xfs", "f2fs",
                           "tmpfs", "overlay", "vfat", "exfat", "ntfs3", "ntfs",
                           "fuseblk"}
             .contains(type))
      return;
    QStorageInfo storage(url.toLocalFile());
    if (storage.isValid())
      emit available(url, storage.bytesAvailable());
  }
};
class TagsJob : public QThread {
  Q_OBJECT
public:
  QList<QUrl> urls;
  QString tag;
  int operation = 0;
signals:
  void done(QString error, QStringList tags);

protected:
  void run() override {
    static std::mutex writes;
    std::unique_lock<std::mutex> lock(writes);
    QStringList errors, result;
    for (auto u : urls) {
      QString error;
      if (operation == 0)
        result = FileTags::tags(u, &error);
      else if (operation == 1)
        error = FileTags::addTag(u, tag);
      else if (operation == 2)
        error = FileTags::removeTag(u, tag);
      else
        error = FileTags::setTags(u, {});
      if (!error.isEmpty() && !errors.contains(error))
        errors << error;
    }
    emit done(errors.join('\n'), result);
  }
};

class FinderWindow : public QMainWindow {
  Q_OBJECT
public:
  explicit FinderWindow(QUrl initial) {
    setObjectName("finderWindow");
    setWindowTitle("Finder");
    setWindowIcon(QIcon::fromTheme("system-file-manager"));
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    resize(1100, 720);
    setMinimumSize(700, 440);
    auto *base = new QWidget;
    base->setGraphicsEffect(new ContourEffect(base));
    setCentralWidget(base);
    auto *outer = new QVBoxLayout(base);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    header = new Header;
    header->setObjectName("finderHeader");
    header->setFixedHeight(52);
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(12, 8, 12, 8);
    hl->setSpacing(8);
    headerSidebar = new QWidget(header);
    auto *hs = new QHBoxLayout(headerSidebar);
    hs->setContentsMargins(0, 0, 0, 0);
    auto *sidebarButton = button("sidebar", "Show Sidebar", headerSidebar);
    hs->addWidget(sidebarButton);
    hs->addStretch();
    headerSidebar->setFixedWidth(244);
    hl->addWidget(headerSidebar);
    connect(sidebarButton, &QToolButton::clicked, this, [this] {
      sidebar->setVisible(!sidebar->isVisible());
      updateMaterial();
    });
    auto *nav = new QWidget;
    nav->setObjectName("navigationCapsule");
    auto *nl = new QHBoxLayout(nav);
    nl->setContentsMargins(2, 0, 2, 0);
    nl->setSpacing(0);
    back = button("back", "Back", nav);
    forward = button("forward", "Forward", nav);
    nl->addWidget(back);
    nl->addWidget(forward);
    hl->addWidget(nav);
    connect(back, &QToolButton::clicked, this, [this] { history(-1); });
    connect(forward, &QToolButton::clicked, this, [this] { history(1); });
    folderTitle = new QLabel;
    folderTitle->setObjectName("finderFolderTitle");
    folderTitle->setTextFormat(Qt::PlainText);
    folderTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    folderTitle->setMinimumWidth(50);
    hl->addWidget(folderTitle, 1);
    modeBox = new QWidget;
    modeBox->setObjectName("modeCapsule");
    auto *ml = new QHBoxLayout(modeBox);
    ml->setContentsMargins(2, 0, 2, 0);
    ml->setSpacing(0);
    modeActions = new QActionGroup(this);
    modeActions->setExclusive(true);
    for (int i = 0; i < 4; ++i) {
      QStringList names{"icons", "list", "columns", "gallery"};
      auto *a = new QAction(
          symbol(names[i]),
          QStringList{"Icons", "List", "Columns", "Gallery"}[i], this);
      a->setCheckable(true);
      a->setData(i);
      modeActions->addAction(a);
      auto *b = button(names[i], a->text(), modeBox);
      b->setDefaultAction(a);
      ml->addWidget(b);
      connect(a, &QAction::triggered, this, [this, i] { setMode(i); });
    }
    hl->addWidget(modeBox);
    modePopup = button("icons", "Change View", header);
    auto *vm = new QMenu(modePopup);
    for (auto *a : modeActions->actions())
      vm->addAction(a);
    modePopup->setMenu(vm);
    modePopup->setPopupMode(QToolButton::InstantPopup);
    modePopup->hide();
    hl->addWidget(modePopup);
    arrange = button("sort", "Sort By", header);
    auto *sortMenu = new QMenu(arrange);
    sortGroup = new QActionGroup(this);
    for (auto entry :
         QList<QPair<QString, int>>{{"Name", KDirModel::Name},
                                    {"Date Modified", KDirModel::ModifiedTime},
                                    {"Size", KDirModel::Size},
                                    {"Kind", KDirModel::Type}}) {
      auto *a = sortMenu->addAction(entry.first);
      a->setCheckable(true);
      a->setData(entry.second);
      sortGroup->addAction(a);
      if (entry.second == 0)
        a->setChecked(true);
      connect(a, &QAction::triggered, this, [this, entry] {
        if (browser())
          browser()->setSort(entry.second, !descending->isChecked());
        saveView();
      });
    }
    sortMenu->addSeparator();
    descending = sortMenu->addAction("Reverse Order");
    descending->setCheckable(true);
    connect(descending, &QAction::toggled, this, [this](bool on) {
      if (browser())
        browser()->setSort(sortGroup->checkedAction()->data().toInt(), !on);
      saveView();
    });
    auto *folders = sortMenu->addAction("Keep Folders on Top");
    folders->setCheckable(true);
    folders->setChecked(QSettings().value("View/foldersFirst", false).toBool());
    connect(folders, &QAction::toggled, this, [this](bool on) {
      browser()->setFoldersFirst(on);
      QSettings().setValue("View/foldersFirst", on);
    });
    arrange->setMenu(sortMenu);
    arrange->setPopupMode(QToolButton::InstantPopup);
    hl->addWidget(arrange);
    share = button("share", "Share", header);
    auto *shareMenu = new QMenu(share);
    shareMenu->addAction("Copy File Locations", this, [this] {
      QStringList u;
      for (const auto &url : selectedUrls())
        u << url.toDisplayString();
      QApplication::clipboard()->setText(u.join('\n'));
    });
    shareMenu->addAction("Open with…", this, [this] {
      for (const auto &u : selectedUrls())
        openWith(u);
    });
    share->setMenu(shareMenu);
    share->setPopupMode(QToolButton::InstantPopup);
    hl->addWidget(share);
    tagButton = button("tags", "Tags", header);
    tagMenu = new QMenu(tagButton);
    tagButton->setMenu(tagMenu);
    tagButton->setPopupMode(QToolButton::InstantPopup);
    hl->addWidget(tagButton);
    for (auto tag : FileTags::standardTags()) {
      QPixmap pix(20, 20);
      pix.fill(Qt::transparent);
      QPainter paint(&pix);
      paint.setRenderHint(QPainter::Antialiasing);
      paint.setPen(Qt::NoPen);
      paint.setBrush(FileTags::tagColor(tag));
      paint.drawEllipse(QRectF(4, 4, 12, 12));
      auto *a = tagMenu->addAction(QIcon(pix), tag);
      a->setObjectName("tag" + tag);
      namedActions["tag" + tag] = a;
      a->setCheckable(true);
      tagActions << a;
      connect(a, &QAction::triggered, this,
              [this, tag](bool checked) { changeTags(tag, checked ? 1 : 2); });
    }
    tagMenu->addSeparator();
    tagMenu->addAction("Add Tag…", this, [this] {
      bool ok = false;
      auto tag = QInputDialog::getText(
          this, "Tags", "Tag name:", QLineEdit::Normal, QString(), &ok);
      if (ok && !tag.trimmed().isEmpty())
        changeTags(tag, 1);
    });
    tagMenu->addAction("Remove All Tags", this,
                       [this] { changeTags(QString(), 3); });
    connect(tagMenu, &QMenu::aboutToShow, this, [this] { updateTags(); });
    auto *more = button("more", "Actions", header);
    actionsMenu = new QMenu(more);
    more->setMenu(actionsMenu);
    more->setPopupMode(QToolButton::InstantPopup);
    hl->addWidget(more);
    search = new QLineEdit;
    search->setObjectName("finderSearch");
    search->setPlaceholderText("Search this folder");
    search->setClearButtonEnabled(true);
    search->setMaximumWidth(190);
    search->hide();
    hl->addWidget(search);
    connect(search, &QLineEdit::textChanged, this, [this](const QString &t) {
      Q_UNUSED(t);
      if (searchTimer)
        searchTimer->start();
    });
    auto *searchButton = button("search", "Search", header);
    connect(searchButton, &QToolButton::clicked, this,
            [this] { showSearch(); });
    hl->addWidget(searchButton);
    for (auto name : {"minimize", "maximize", "close"}) {
      auto *b = button(name, QString(name), header);
      b->setObjectName("window-" + QString(name));
      b->setFixedSize(26, 28);
      hl->addWidget(b);
      connect(b, &QToolButton::clicked, this, [this, name] {
        if (QString(name) == "minimize")
          showMinimized();
        else if (QString(name) == "maximize") {
          if (isMaximized())
            showNormal();
          else
            showMaximized();
        } else
          close();
      });
    }
    outer->addWidget(header);
    location = new QLineEdit;
    location->setObjectName("finderLocation");
    location->setPlaceholderText("Go to folder");
    location->hide();
    outer->addWidget(location);
    connect(location, &QLineEdit::returnPressed, this, [this] {
      navigate(QUrl::fromUserInput(location->text(), QDir::homePath(),
                                   QUrl::AssumeLocalFile));
      location->hide();
    });
    body = new QSplitter(Qt::Horizontal);
    body->setObjectName("finderBody");
    body->setHandleWidth(1);
    sidebar = new Material;
    sidebar->setObjectName("finderSidebar");
    auto *sl = new QVBoxLayout(sidebar);
    sl->setContentsMargins(8, 6, 8, 6);
    sl->setSpacing(0);
    places = new FinderPlacesView;
    places->setObjectName("finderPlaces");
    places->setModel(new KFilePlacesModel(places));
    places->setAutoResizeItemsEnabled(false);
    places->setDropOnPlaceEnabled(true);
    places->setDragAutoActivationDelay(650);
    sl->addWidget(places);
    connect(places, &KFilePlacesView::placeActivated, this,
            [this](const QUrl &u) { navigate(u); });
    connect(places, &KFilePlacesView::tabRequested, this,
            [this](const QUrl &u) { addTab(u, false); });
    connect(places, &KFilePlacesView::activeTabRequested, this,
            [this](const QUrl &u) { addTab(u, true); });
    connect(places, &KFilePlacesView::newWindowRequested, this,
            [this](const QUrl &u) { emit newWindowRequested(u); });
    connect(places, &KFilePlacesView::urlsDropped, this,
            [this](const QUrl &u, QDropEvent *e, QWidget *) {
              browser()->drop(e, u);
            });
    tabs = new QTabWidget;
    tabs->setObjectName("finderTabs");
    tabs->setDocumentMode(true);
    tabs->setTabsClosable(true);
    tabs->setMovable(true);
    tabs->tabBar()->setAutoHide(true);
    connect(tabs, &QTabWidget::currentChanged, this, [this](int) {
      if (fileSearch)
        stopSearch();
      if (browser()) {
        auto &s = states[browser()];
        if (!s.viewRestored) {
          restoreView();
          s.viewRestored = true;
        }
      }
      refresh();
    });
    connect(tabs, &QTabWidget::tabCloseRequested, this, [this](int i) {
      if (tabs->count() == 1) {
        close();
        return;
      }
      auto *w = tabs->widget(i);
      w->disconnect(this);
      states.remove(qobject_cast<Browser *>(w));
      tabs->removeTab(i);
      w->deleteLater();
      refresh();
    });
    body->addWidget(sidebar);
    contentStack = new QStackedWidget;
    contentStack->addWidget(tabs);
    searchPage = new QWidget;
    auto *resultsLayout = new QVBoxLayout(searchPage);
    resultsLayout->setContentsMargins(0, 0, 0, 0);
    searchSummary = new QLabel;
    searchSummary->setContentsMargins(14, 8, 14, 8);
    resultsLayout->addWidget(searchSummary);
    searchResults = new QTreeView;
    searchResults->setObjectName("finderSearchResults");
    searchResults->setRootIsDecorated(false);
    searchResults->setSelectionMode(QAbstractItemView::ExtendedSelection);
    searchResults->setAlternatingRowColors(true);
    searchResults->setIconSize({20, 20});
    searchResults->setSortingEnabled(true);
    searchModel = new QStandardItemModel(this);
    searchModel->setHorizontalHeaderLabels({"Name", "Kind", "Location"});
    searchResults->setModel(searchModel);
    connect(searchResults->selectionModel(),
            &QItemSelectionModel::selectionChanged, this,
            [this] { refresh(); });
    searchResults->setColumnWidth(0, 260);
    searchResults->setColumnWidth(1, 140);
    searchResults->setEditTriggers(QAbstractItemView::NoEditTriggers);
    searchResults->installEventFilter(this);
    resultsLayout->addWidget(searchResults, 1);
    contentStack->addWidget(searchPage);
    body->addWidget(contentStack);
    connect(searchResults, &QTreeView::doubleClicked, this,
            [this](const QModelIndex &i) {
              openUrl(i.siblingAtColumn(0).data(Qt::UserRole).toUrl());
            });
    searchResults->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(searchResults, &QWidget::customContextMenuRequested, this,
            [this](const QPoint &p) {
              QMenu menu;
              menu.addAction("Open", this, [this] {
                for (auto u : searchSelection())
                  openUrl(u);
              });
              menu.addAction("Quick Look", this,
                             [this] { quickLook(searchSelection()); });
              menu.addAction("Show in Enclosing Folder", this, [this] {
                const auto u = searchSelection();
                if (!u.isEmpty())
                  showItems({u.first()});
              });
              menu.addAction("Get Info", this, [this] {
                const auto u = searchSelection();
                if (!u.isEmpty()) {
                  auto *d = new KPropertiesDialog(u.first(), this);
                  d->setAttribute(Qt::WA_DeleteOnClose);
                  d->show();
                }
              });
              menu.exec(searchResults->viewport()->mapToGlobal(p));
            });
    fileSearch = new LocalFileSearch(this);
    searchTimer = new QTimer(this);
    searchTimer->setSingleShot(true);
    searchTimer->setInterval(250);
    connect(searchTimer, &QTimer::timeout, this, [this] { beginSearch(); });
    connect(fileSearch, &LocalFileSearch::filesFound, this,
            [this](quint64, const QList<QUrl> &urls) {
              for (auto u : urls) {
                auto mime = QMimeDatabase().mimeTypeForFile(
                    u.fileName(), QMimeDatabase::MatchExtension);
                auto *name = new QStandardItem(
                    QIcon::fromTheme(mime.iconName()), u.fileName());
                name->setData(u, Qt::UserRole);
                auto *kind = new QStandardItem(mime.comment());
                auto *path = new QStandardItem(QDir::toNativeSeparators(
                    QFileInfo(u.toLocalFile()).absolutePath()));
                searchModel->appendRow({name, kind, path});
              }
              searchSummary->setText(QString("Searching %1 — %2 results")
                                         .arg(title(searchRoot))
                                         .arg(searchModel->rowCount()));
            });
    connect(fileSearch, &LocalFileSearch::finished, this,
            [this](quint64, int count, bool limited, int skipped) {
              searchSummary->setText(
                  QString("%1 results in %2%3%4")
                      .arg(count)
                      .arg(title(searchRoot))
                      .arg(limited ? " · Search limit reached" : "")
                      .arg(skipped
                               ? QString(" · %1 unavailable folders skipped")
                                     .arg(skipped)
                               : QString()));
            });
    connect(fileSearch, &LocalFileSearch::error, this,
            [this](quint64, const QString &e) { searchSummary->setText(e); });
    body->setSizes({256, 844});
    body->setStretchFactor(1, 1);
    outer->addWidget(body, 1);
    connect(body, &QSplitter::splitterMoved, this, [this] {
      layoutHeader();
      updateMaterial();
    });
    bottom = new QWidget;
    bottom->setObjectName("finderFooter");
    auto *bl = new QVBoxLayout(bottom);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(0);
    crumbs = new QWidget;
    crumbLayout = new QHBoxLayout(crumbs);
    crumbLayout->setContentsMargins(12, 0, 12, 0);
    crumbLayout->setSpacing(2);
    crumbs->setFixedHeight(24);
    bl->addWidget(crumbs);
    auto *statusRow = new QWidget;
    auto *sr = new QHBoxLayout(statusRow);
    sr->setContentsMargins(12, 0, 12, 0);
    sr->setSpacing(8);
    statusRow->setFixedHeight(24);
    status = new QLabel;
    status->setObjectName("finderStatus");
    sr->addWidget(status, 1);
    auto *size = iconSlider = new QSlider(Qt::Horizontal);
    size->setObjectName("finderIconSize");
    size->setRange(40, 160);
    size->setValue(96);
    size->setFixedWidth(100);
    size->setToolTip("Icon size");
    connect(size, &QSlider::valueChanged, this, [this](int px) {
      if (browser())
        browser()->setIconSize(px);
      saveView();
    });
    sr->addWidget(size);
    bl->addWidget(statusRow);
    outer->addWidget(bottom);
    makeActions();
    applyStyle();
    addTab(initial, true);
    qApp->installEventFilter(this);
    QSettings settings;
    restoreGeometry(settings.value("Window/geometry").toByteArray());
    auto sizes = settings.value("Window/sidebar", 256).toInt();
    body->setSizes({sizes, width() - sizes});
    QTimer::singleShot(0, this, [this] { layoutHeader(); });
  }
  Browser *browser() const {
    return qobject_cast<Browser *>(tabs->currentWidget());
  }
  bool showingSearch() const {
    return contentStack && contentStack->currentWidget() == searchPage;
  }
  QList<QUrl> selectedUrls() const {
    return showingSearch() ? searchSelection() : browser()->selectedUrls();
  }
  void copy(bool cut = false) {
    auto *focus = QApplication::focusWidget();
    if (auto *line = qobject_cast<QLineEdit *>(focus)) {
      if (cut)
        line->cut();
      else
        line->copy();
      return;
    }
    if (auto *text = qobject_cast<QTextEdit *>(focus)) {
      if (!cut)
        text->copy();
      return;
    }
    auto urls = selectedUrls();
    if (urls.isEmpty())
      return;
    auto *mime = new QMimeData;
    mime->setUrls(urls);
    mime->setData("application/x-kde-cutselection", cut ? "1" : "0");
    QApplication::clipboard()->setMimeData(mime);
  }
  void selectedOperation(const QString &operation) {
    if (!showingSearch()) {
      if (operation == "rename")
        browser()->renameSelected();
      else if (operation == "trash")
        browser()->trashSelected();
      else if (operation == "duplicate")
        browser()->duplicateSelected();
      return;
    }
    auto urls = selectedUrls();
    if (urls.isEmpty())
      return;
    KIO::CopyJob *job = nullptr;
    if (operation == "trash")
      job = KIO::trash(urls);
    else if (operation == "rename" && urls.size() == 1) {
      bool ok = false;
      QString name =
          QInputDialog::getText(this, "Rename", "Name:", QLineEdit::Normal,
                                urls.first().fileName(), &ok);
      if (!ok || name == urls.first().fileName())
        return;
      if (name.isEmpty() || name == "." || name == ".." || name.contains('/') ||
          name.contains(QChar::Null)) {
        status->setText("Enter a valid filename without a slash.");
        return;
      }
      auto dest = urls.first();
      dest.setPath(dest.path().left(dest.path().lastIndexOf('/') + 1) + name);
      job = KIO::moveAs(urls.first(), dest);
    } else if (operation == "duplicate") {
      for (auto u : urls) {
        QFileInfo info(u.toLocalFile());
        QString suffix = info.isDir() || info.suffix().isEmpty()
                             ? QString()
                             : "." + info.suffix();
        QString stem =
            suffix.isEmpty() ? info.fileName() : info.completeBaseName();
        QString path = info.dir().filePath(stem + " copy" + suffix);
        int n = 2;
        while (QFileInfo::exists(path))
          path = info.dir().filePath(stem + " copy " + QString::number(n++) +
                                     suffix);
        runCopyJob(KIO::copyAs(u, QUrl::fromLocalFile(path)));
      }
      return;
    }
    if (job)
      runCopyJob(job);
  }
  void runCopyJob(KIO::CopyJob *job) {
    KIO::FileUndoManager::self()->recordCopyJob(job);
    job->setUiDelegate(KIO::createDefaultJobUiDelegate(
        KJobUiDelegate::AutoHandlingEnabled, this));
    connect(job, &KJob::result, this, [this](KJob *) {
      if (showingSearch())
        beginSearch();
    });
  }
  void navigate(const QUrl &u) {
    if (browser()) {
      stopSearch();
      saveView();
      explicitNavigation = true;
      browser()->navigate(u);
      explicitNavigation = false;
    }
  }
  void showItems(const QList<QUrl> &urls) {
    if (urls.isEmpty())
      return;
    navigate(urls.first().adjusted(QUrl::RemoveFilename));
    browser()->selectUrls(urls);
  }
  void quickLook(const QList<QUrl> &urls) {
    if (urls.isEmpty())
      return;
    auto *w = new QMainWindow(this, Qt::Window);
    w->setAttribute(Qt::WA_DeleteOnClose);
    w->setWindowTitle("Quick Look");
    w->resize(840, 620);
    auto *p = new PreviewPane(w);
    QList<QUrl> ordered = urls;
    const auto current = browser()->activeView()->currentIndex();
    const auto item = current.data(KDirModel::FileItemRole).value<KFileItem>();
    if (ordered.removeOne(item.url()))
      ordered.prepend(item.url());
    p->setUrls(ordered);
    w->setCentralWidget(p);
    connect(p, &PreviewPane::openRequested, w,
            [this](const QUrl &u) { openUrl(u); });
    connect(p, &PreviewPane::closeRequested, w, &QWidget::close);
    w->show();
    p->setFocus();
  }
  void setMode(int m) {
    if (!browser())
      return;
    stopSearch();
    m = qBound(0, m, 3);
    browser()->setMode(Browser::Mode(m));
    modeActions->actions()[m]->setChecked(true);
    modePopup->setIcon(
        symbol(QStringList{"icons", "list", "columns", "gallery"}[m]));
    saveView();
    refresh();
  }
  void prepareTagMenu() { updateTags(); }
  void addTab(const QUrl &u, bool activate) {
    auto *b = new Browser;
    if (namedActions.contains("hidden"))
      b->setHiddenFiles(namedActions["hidden"]->isChecked());
    TabState s;
    auto attach = [&](QWidget *host) {
      auto *l = new QVBoxLayout(host);
      l->setContentsMargins(0, 0, 0, 0);
      auto *p = new PreviewPane(host);
      p->setCompact(true);
      l->addWidget(p);
      connect(p, &PreviewPane::openRequested, this,
              [this](const QUrl &url) { openUrl(url); });
      return p;
    };
    s.gallery = attach(b->galleryPreviewHost());
    s.gallery->setGallery(true);
    s.column = attach(b->columnPreviewHost());
    states.insert(b, s);
    const int index = tabs->addTab(b, "Folder");
    connect(b, &Browser::urlChanged, this, [this, b](const QUrl &url) {
      auto &s = states[b];
      if (!s.navigatingHistory) {
        while (s.history.size() > s.historyIndex + 1)
          s.history.removeLast();
        if (s.history.isEmpty() || s.history.last() != url) {
          s.history << url;
          s.historyIndex = s.history.size() - 1;
        }
      }
      tabs->setTabText(tabs->indexOf(b), title(url));
      if (b == browser()) {
        if (explicitNavigation || b->mode() != Browser::Columns)
          restoreView();
        refresh();
      }
    });
    connect(b, &Browser::selectionChanged, this,
            [this, b](const KFileItemList &items) {
              QList<QUrl> urls;
              for (auto i : items)
                urls << i.url();
              auto &s = states[b];
              s.gallery->setUrls(urls);
              s.column->setUrls(urls);
              if (b == browser())
                refresh();
            });
    connect(b, &Browser::quickLookRequested, this, &FinderWindow::quickLook);
    connect(b, &Browser::openRequested, this,
            [this](const QUrl &url) { openUrl(url); });
    connect(b, &Browser::countChanged, this, [this, b](int) {
      if (b == browser())
        refresh();
    });
    connect(b, &Browser::statusMessage, this, [this, b](const QString &t) {
      if (b == browser() && !t.isEmpty())
        status->setText(t);
    });
    connect(b, &Browser::errorOccurred, this,
            [this](const QString &t) { status->setText(t); });
    explicitNavigation = true;
    b->navigate(u);
    explicitNavigation = false;
    if (activate) {
      tabs->setCurrentIndex(index);
      restoreView();
      states[b].viewRestored = true;
    }
    b->activeView()->setFocus();
    refresh();
  }
  QJsonObject diagnostics() const {
    QJsonArray selected, columns, hiddenColumns;
    if (auto *tree = browser()->findChild<QTreeView *>("finderListView"))
      for (int i = 0; i < int(KDirModel::ColumnCount); ++i)
        if (tree->isColumnHidden(i))
          hiddenColumns << i;
    for (auto u : selectedUrls())
      selected << u.toString();
    for (auto *view : browser()->findChildren<QListView *>())
      if (view->isVisible())
        columns << QJsonObject{
            {"name", view->objectName()},
            {"width", view->width()},
            {"rows",
             view->model() ? view->model()->rowCount(view->rootIndex()) : 0}};
    auto *f = QApplication::focusWidget();
    return {
        {"url", browser()->url().toString()},
        {"searchResults", searchModel->rowCount()},
        {"searchRunning", fileSearch->isRunning()},
        {"searchVisible", contentStack->currentWidget() == searchPage},
        {"mode", int(browser()->mode())},
        {"iconSize", browser()->iconSize()},
        {"sortColumn", browser()->sortColumn()},
        {"sortAscending", browser()->sortAscending()},
        {"tagsBusy", writingTags},
        {"tagsReady", !tagActions.isEmpty() && tagActions.first()->isEnabled()},
        {"hiddenColumns", hiddenColumns},
        {"count", browser()->itemCount()},
        {"selected", selected},
        {"tabs", tabs->count()},
        {"headerHeight", header->height()},
        {"sidebarWidth", sidebar->width()},
        {"visibleLists", columns},
        {"focus", f ? f->metaObject()->className() : ""},
        {"search", search->text()},
        {"width", width()},
        {"height", height()},
        {"blur", header->blur}};
  }
  bool key(int key, int modifiers, const QString &text = {}) {
    auto *f = QApplication::focusWidget();
    if (auto *modal = QApplication::activeModalWidget())
      f = modal->focusWidget() ? modal->focusWidget() : modal;
    if (!f)
      f = browser()->activeView();
    QKeyEvent over(QEvent::ShortcutOverride, key,
                   Qt::KeyboardModifiers(modifiers), text);
    QApplication::sendEvent(f, &over);
    QKeyEvent press(QEvent::KeyPress, key, Qt::KeyboardModifiers(modifiers),
                    text);
    QApplication::sendEvent(f, &press);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::KeyboardModifiers(modifiers),
                      text);
    QApplication::sendEvent(f, &release);
    return true;
  }
  QAction *action(const QString &name) const {
    return namedActions.value(name);
  }
  void revealLocation() {
    location->setText(browser()->url().toDisplayString(QUrl::PreferLocalFile));
    location->show();
    location->setFocus();
    location->selectAll();
  }
  void openUrl(const QUrl &url) {
    if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isDir()) {
      navigate(url);
      return;
    }
    auto *job = new KIO::OpenUrlJob(url);
    job->setUiDelegate(KIO::createDefaultJobUiDelegate(
        KJobUiDelegate::AutoHandlingEnabled, this));
    job->start();
  }
  void openWith(const QUrl &url) {
    auto *dialog = new KOpenWithDialog(QList<QUrl>{url}, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::accepted, this, [this, dialog, url] {
      auto service = dialog->service();
      if (!service)
        service = KService::Ptr(
            new KService(dialog->text(), dialog->text(), QString()));
      auto *job = new KIO::ApplicationLauncherJob(service);
      job->setUrls({url});
      job->setUiDelegate(KIO::createDefaultJobUiDelegate(
          KJobUiDelegate::AutoHandlingEnabled, this));
      job->start();
    });
    dialog->open();
  }
signals:
  void newWindowRequested(const QUrl &url);

protected:
  void resizeEvent(QResizeEvent *e) override {
    QMainWindow::resizeEvent(e);
    layoutHeader();
    updateMaterial();
  }
  void closeEvent(QCloseEvent *e) override {
    saveView();
    QSettings s;
    s.setValue("Window/geometry", saveGeometry());
    s.setValue("Window/sidebar", sidebar->width());
    QMainWindow::closeEvent(e);
  }
  void showEvent(QShowEvent *e) override {
    QMainWindow::showEvent(e);
    QTimer::singleShot(0, this, [this] { updateMaterial(); });
  }
  bool eventFilter(QObject *o, QEvent *e) override {
    if (e->type() == QEvent::MouseButtonPress && !isMaximized()) {
      auto *w = qobject_cast<QWidget *>(o);
      auto *m = static_cast<QMouseEvent *>(e);
      if (w && w->window() == this && m->button() == Qt::LeftButton &&
          windowHandle()) {
        QPoint point = mapFromGlobal(m->globalPosition().toPoint());
        Qt::Edges edges;
        if (point.x() < 5)
          edges |= Qt::LeftEdge;
        if (point.x() >= width() - 5)
          edges |= Qt::RightEdge;
        if (point.y() < 5)
          edges |= Qt::TopEdge;
        if (point.y() >= height() - 5)
          edges |= Qt::BottomEdge;
        if (edges && windowHandle()->startSystemResize(edges))
          return true;
      }
    }
    if (e->type() == QEvent::KeyPress) {
      auto *w = qobject_cast<QWidget *>(o);
      if (w && w->window() == this) {
        auto *k = static_cast<QKeyEvent *>(e);
        if (k->key() == Qt::Key_Escape &&
            (search->isVisible() || location->isVisible())) {
          search->clear();
          search->hide();
          location->hide();
          stopSearch();
          layoutHeader();
          browser()->activeView()->setFocus();
          return true;
        }
        if (o == searchResults && k->modifiers() == Qt::NoModifier &&
            (k->key() == Qt::Key_Space || k->key() == Qt::Key_Return)) {
          quickLook(searchSelection());
          return true;
        }
      }
    }
    return QMainWindow::eventFilter(o, e);
  }

private:
  Header *header;
  Material *sidebar;
  QSplitter *body;
  QTabWidget *tabs;
  QStackedWidget *contentStack;
  QWidget *searchPage;
  QTreeView *searchResults;
  QStandardItemModel *searchModel;
  LocalFileSearch *fileSearch = nullptr;
  QTimer *searchTimer = nullptr;
  QLabel *searchSummary;
  QUrl searchRoot;
  QUrl capacityUrl;
  QString capacityText;
  QWidget *bottom, *crumbs, *modeBox, *headerSidebar;
  QHBoxLayout *crumbLayout;
  QLabel *folderTitle, *status;
  KFilePlacesView *places;
  QLineEdit *location, *search;
  QToolButton *back, *forward, *modePopup, *arrange, *share;
  QSlider *iconSlider;
  QActionGroup *modeActions, *sortGroup;
  QAction *descending;
  QMenu *actionsMenu;
  QToolButton *tagButton;
  QMenu *tagMenu;
  QList<QAction *> tagActions;
  QHash<Browser *, TabState> states;
  QHash<QString, QAction *> namedActions;
  bool restoring = false, explicitNavigation = false;
  bool writingTags = false;
  quint64 tagGeneration = 0;
  void changeTags(QString tag, int operation) {
    auto urls = selectedUrls();
    if (urls.isEmpty() || writingTags)
      return;
    writingTags = true;
    ++tagGeneration;
    for (auto a : tagMenu->actions())
      a->setEnabled(false);
    auto *job = new TagsJob;
    job->urls = urls;
    job->tag = tag;
    job->operation = operation;
    connect(job, &TagsJob::done, this, [this](QString error, QStringList) {
      writingTags = false;
      status->setText(error.isEmpty() ? "Tags updated" : error);
      updateTags();
    });
    connect(job, &QThread::finished, job, &QObject::deleteLater);
    job->start();
  }
  void updateTags() {
    auto urls = selectedUrls();
    const auto generation = ++tagGeneration;
    for (auto a : tagMenu->actions())
      a->setEnabled(false);
    for (auto a : tagActions)
      a->setChecked(false);
    if (urls.isEmpty() || writingTags)
      return;
    auto *job = new TagsJob;
    job->urls = {urls.first()};
    connect(job, &TagsJob::done, this,
            [this, urls, generation](QString error, QStringList tags) {
              if (selectedUrls() != urls || generation != tagGeneration ||
                  writingTags)
                return;
              for (auto a : tagMenu->actions())
                a->setEnabled(error.isEmpty());
              for (auto a : tagActions) {
                a->setEnabled(error.isEmpty());
                a->setChecked(tags.contains(a->text()));
              }
              if (!error.isEmpty())
                status->setText(error);
            });
    connect(job, &QThread::finished, job, &QObject::deleteLater);
    job->start();
  }
  QList<QUrl> searchSelection() const {
    QList<QUrl> urls;
    for (auto i : searchResults->selectionModel()->selectedRows())
      urls << i.data(Qt::UserRole).toUrl();
    return urls;
  }
  void stopSearch() {
    if (fileSearch)
      fileSearch->cancel();
    if (searchTimer)
      searchTimer->stop();
    if (contentStack)
      contentStack->setCurrentWidget(tabs);
    if (search && !search->text().isEmpty()) {
      QSignalBlocker b(search);
      search->clear();
    }
    if (browser() && status)
      refresh();
  }
  void beginSearch() {
    if (!browser())
      return;
    if (search->text().trimmed().isEmpty()) {
      fileSearch->cancel();
      contentStack->setCurrentWidget(tabs);
      return;
    }
    searchRoot = browser()->url();
    searchModel->removeRows(0, searchModel->rowCount());
    contentStack->setCurrentWidget(searchPage);
    searchSummary->setText("Searching " + title(searchRoot) + "…");
    LocalFileSearch::Options options;
    options.showHidden = namedActions["hidden"]->isChecked();
    fileSearch->start(searchRoot, search->text().trimmed(), options);
    refresh();
  }
  void layoutHeader() {
    if (!modeBox || !sidebar)
      return;
    const bool narrow = width() < 980;
    modeBox->setVisible(!narrow);
    modePopup->setVisible(narrow);
    headerSidebar->setFixedWidth(
        sidebar->isVisible() ? qMax(34, sidebar->width() - 12) : 34);
    if (width() < 840)
      headerSidebar->setFixedWidth(34);
    arrange->setVisible(width() >= 840 && !search->isVisible());
    share->setVisible(width() >= 840 && !search->isVisible());
    tagButton->setVisible(width() >= 1050 && !search->isVisible());
  }
  QToolButton *button(const QString &name, const QString &label,
                      QWidget *parent) {
    auto *b = new QToolButton(parent);
    b->setIcon(symbol(name));
    b->setIconSize({20, 20});
    b->setToolTip(label);
    b->setAccessibleName(label);
    b->setFixedSize(34, 36);
    b->setAutoRaise(true);
    b->setFocusPolicy(Qt::StrongFocus);
    return b;
  }
  QString title(QUrl u) const {
    auto t = u.fileName();
    if (t.isEmpty())
      return u.isLocalFile() ? "Computer" : u.scheme();
    return t;
  }
  void history(int d) {
    auto &s = states[browser()];
    int next = s.historyIndex + d;
    if (next < 0 || next >= s.history.size())
      return;
    const int previous = s.historyIndex;
    s.historyIndex = next;
    s.navigatingHistory = true;
    navigate(s.history[next]);
    if (browser()->url().adjusted(QUrl::StripTrailingSlash) !=
        s.history[next].adjusted(QUrl::StripTrailingSlash))
      s.historyIndex = previous;
    s.navigatingHistory = false;
    refresh();
  }
  void saveView() {
    if (restoring || !browser())
      return;
    QSettings settings;
    settings.beginGroup(
        "Folders/" + QString::fromLatin1(browser()->url().toEncoded().toBase64(
                         QByteArray::Base64UrlEncoding)));
    settings.setValue("mode", int(browser()->mode()));
    settings.setValue("sortColumn", browser()->sortColumn());
    settings.setValue("sortAscending", browser()->sortAscending());
    settings.setValue("iconSize", browser()->iconSize());
    if (auto *tree = browser()->findChild<QTreeView *>("finderListView"))
      settings.setValue("listHeader", tree->header()->saveState());
    settings.endGroup();
  }
  void restoreView() {
    if (restoring || !browser())
      return;
    restoring = true;
    QSettings settings;
    settings.beginGroup(
        "Folders/" + QString::fromLatin1(browser()->url().toEncoded().toBase64(
                         QByteArray::Base64UrlEncoding)));
    int mode = settings.value("mode", QSettings().value("View/defaultMode", 0))
                   .toInt();
    browser()->setMode(Browser::Mode(qBound(0, mode, 3)));
    browser()->setIconSize(
        settings
            .value("iconSize", QSettings().value("View/defaultIconSize", 96))
            .toInt());
    const auto tree = browser()->findChild<QTreeView *>("finderListView");
    if (tree)
      tree->header()->restoreState(settings.value("listHeader").toByteArray());
    browser()->setSort(
        settings.value("sortColumn", QSettings().value("View/defaultSort", 0))
            .toInt(),
        settings
            .value("sortAscending",
                   QSettings().value("View/defaultAscending", true))
            .toBool());

    browser()->setFoldersFirst(
        QSettings().value("View/foldersFirst", false).toBool());
    restoring = false;
  }
  void showSearch() {
    search->show();
    search->setFocus();
    search->selectAll();
    layoutHeader();
  }
  void viewOptions() {
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("View Options — " + title(browser()->url()));
    auto *layout = new QVBoxLayout(dialog);
    auto *form = new QFormLayout;
    layout->addLayout(form);
    auto *mode = new QComboBox;
    mode->setObjectName("viewOptionMode");
    mode->addItems({"Icons", "List", "Columns", "Gallery"});
    mode->setCurrentIndex(int(browser()->mode()));
    form->addRow("View", mode);
    auto *size = new QSpinBox;
    size->setObjectName("viewOptionIconSize");
    size->setRange(40, 160);
    size->setValue(browser()->iconSize());
    form->addRow("Icon size", size);
    auto *sort = new QComboBox;
    sort->setObjectName("viewOptionSort");
    for (auto a : sortGroup->actions())
      sort->addItem(a->text(), a->data());
    sort->setCurrentIndex(sort->findData(browser()->sortColumn()));
    form->addRow("Sort by", sort);
    auto *reverse = new QCheckBox("Descending");
    reverse->setObjectName("viewOptionReverse");
    reverse->setChecked(!browser()->sortAscending());
    form->addRow(reverse);
    auto *tree = browser()->findChild<QTreeView *>("finderListView");
    QList<QPair<int, QCheckBox *>> columns;
    for (int c = 1; c < int(KDirModel::ColumnCount); ++c) {
      auto *box = new QCheckBox(
          tree->model()->headerData(c, Qt::Horizontal).toString());
      box->setObjectName("viewOptionColumn" + QString::number(c));
      box->setChecked(!tree->isColumnHidden(c));
      form->addRow(box);
      columns << qMakePair(c, box);
    }
    auto *defaults = new QCheckBox("Use these settings for new folders");
    form->addRow(defaults);
    auto *buttons =
        new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Save);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, dialog,
            [this, dialog, mode, size, sort, reverse, columns, defaults] {
              setMode(mode->currentIndex());
              browser()->setIconSize(size->value());
              browser()->setSort(sort->currentData().toInt(),
                                 !reverse->isChecked());
              auto *tree = browser()->findChild<QTreeView *>("finderListView");
              for (auto [c, box] : columns)
                tree->setColumnHidden(c, !box->isChecked());
              saveView();
              if (defaults->isChecked()) {
                QSettings s;
                s.setValue("View/defaultMode", mode->currentIndex());
                s.setValue("View/defaultIconSize", size->value());
                s.setValue("View/defaultSort", sort->currentData().toInt());
                s.setValue("View/defaultAscending", !reverse->isChecked());
              }
              refresh();
              dialog->accept();
            });
    dialog->open();
  }
  void refresh() {
    if (!browser())
      return;
    const auto u = browser()->url();
    if (u != capacityUrl) {
      capacityUrl = u;
      capacityText.clear();
      if (u.isLocalFile()) {
        auto *probe = new CapacityProbe(u);
        connect(probe, &CapacityProbe::available, this,
                [this](QUrl url, qint64 bytes) {
                  if (url == capacityUrl) {
                    capacityText =
                        QLocale().formattedDataSize(bytes) + " available";
                    refresh();
                  }
                });
        connect(probe, &QThread::finished, probe, &QObject::deleteLater);
        probe->start();
      }
    }
    folderTitle->setText(title(u));
    folderTitle->setToolTip(u.toDisplayString());
    places->setUrl(u);
    auto &s = states[browser()];
    back->setEnabled(s.historyIndex > 0);
    forward->setEnabled(s.historyIndex + 1 < s.history.size());
    modeActions->actions()[int(browser()->mode())]->setChecked(true);
    modePopup->setIcon(symbol(QStringList{"icons", "list", "columns",
                                          "gallery"}[int(browser()->mode())]));
    {
      QSignalBlocker blocker(iconSlider);
      iconSlider->setValue(browser()->iconSize());
    }
    iconSlider->setVisible(browser()->mode() == Browser::Icons);
    for (auto *a : sortGroup->actions())
      a->setChecked(a->data().toInt() == browser()->sortColumn());
    {
      QSignalBlocker blocker(descending);
      descending->setChecked(!browser()->sortAscending());
    }
    for (auto name : {"open", "rename", "duplicate", "trash", "quickLook"})
      if (namedActions.contains(name))
        namedActions[name]->setEnabled(!selectedUrls().isEmpty());
    tagButton->setEnabled(!selectedUrls().isEmpty());
    for (auto name : {"newFolder", "paste", "moveHere"})
      if (namedActions.contains(name))
        namedActions[name]->setEnabled(!showingSearch());
    if (namedActions.contains("rename"))
      namedActions["rename"]->setEnabled(selectedUrls().size() == 1);
    const int selected = selectedUrls().size();
    QString text = selected ? QString("%1 of %2 selected")
                                  .arg(selected)
                                  .arg(browser()->itemCount())
                            : QString("%1 items").arg(browser()->itemCount());
    if (!capacityText.isEmpty())
      text += " · " + capacityText;
    status->setText(text);
    while (auto *item = crumbLayout->takeAt(0)) {
      if (item->widget())
        item->widget()->deleteLater();
      delete item;
    }
    if (u.isLocalFile()) {
      QString path = u.toLocalFile();
      QStringList pieces = path.split('/', Qt::SkipEmptyParts);
      QUrl current = QUrl::fromLocalFile("/");
      auto *root = new QPushButton("Computer");
      crumbLayout->addWidget(root);
      connect(root, &QPushButton::clicked, this,
              [this] { navigate(QUrl::fromLocalFile("/")); });
      for (const QString &p : pieces) {
        auto *l = new QLabel("›");
        crumbLayout->addWidget(l);
        current.setPath(current.path() + p + '/');
        auto *b = new QPushButton(p);
        crumbLayout->addWidget(b);
        connect(b, &QPushButton::clicked, this,
                [this, current] { navigate(current); });
      }
    } else {
      auto *l = new QLabel(u.toDisplayString());
      crumbLayout->addWidget(l);
    }
    crumbLayout->addStretch();
  }
  void updateMaterial() {
    if (!windowHandle() || !sidebar)
      return;
    bool available =
        KWindowEffects::isEffectAvailable(KWindowEffects::BlurBehind);
    header->blur = sidebar->blur = available;
    header->update();
    sidebar->update();
    QRegion region(header->geometry());
    if (sidebar->isVisible())
      region += QRegion(
          sidebar->rect().translated(sidebar->mapTo(this, QPoint(0, 0))));
    KWindowEffects::enableBlurBehind(windowHandle(), available, region);
  }
  void applyStyle() {
    setStyleSheet(R"(
        QWidget{color:#ededf2;font-weight:500;} QMainWindow{background:transparent;}
        #finderFolderTitle{font-size:14px;font-weight:500;background:transparent;} 
        #navigationCapsule,#modeCapsule{background:rgba(255,255,255,15);border:1px solid rgba(255,255,255,30);border-radius:18px;}
        QToolButton{border:0;border-radius:10px;background:transparent;} QToolButton:hover{background:rgba(255,255,255,24);} QToolButton:checked{background:rgba(255,255,255,38);} QToolButton:disabled{opacity:.4;}
        QToolButton::menu-indicator{image:none;width:0;height:0;}
        #window-close:hover{background:#bb4949;} #finderPlaces{background:transparent;border:0;}
        #finderPlaces::item{min-height:30px;border:0;border-radius:8px;} #finderPlaces::item:selected{background:rgba(255,255,255,28);color:white;} #finderPlaces::item:hover{background:rgba(255,255,255,15);}
        QLineEdit{background:#343438;border:1px solid #55555b;border-radius:8px;padding:6px 8px;selection-background-color:#277cef;}
        QTabWidget::pane{border:0;background:#1e1e1e;} QTabBar::tab{background:#27272b;border:0;padding:6px 20px;} QTabBar::tab:selected{background:#3b3b3f;}
        #finderFooter{background:#252525;} #finderFooter QPushButton{background:transparent;border:0;padding:0 2px;font-size:11px;} #finderFooter QLabel{font-size:11px;color:#aaaab1;}
        QMenu{background:#303035;border:1px solid #55555b;padding:6px;border-radius:10px;} QMenu::item{padding:6px 16px;border-radius:5px;} QMenu::item:selected{background:#277cef;}
        QHeaderView::section{background:#252525;color:#bbbbc0;border:0;padding:4px 8px;border-bottom:1px solid #36363a;}
        #finderPreviewPane{background:#1e1e1e;} QPushButton{background:#333337;border:1px solid #4e4e54;border-radius:8px;padding:4px 9px;} QPushButton:hover{background:#424248;}
        QScrollBar:vertical{background:transparent;width:9px;margin:2px;} QScrollBar::handle:vertical{background:#ffffff38;border-radius:3px;min-height:30px;} QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;} QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:transparent;}
        QScrollBar:horizontal{background:transparent;height:9px;margin:2px;} QScrollBar::handle:horizontal{background:#ffffff38;border-radius:3px;min-width:30px;} QScrollBar::add-line:horizontal,QScrollBar::sub-line:horizontal{width:0;} QScrollBar::add-page:horizontal,QScrollBar::sub-page:horizontal{background:transparent;}
        #finderSearchResults{background:#1e1e1e;border:0;alternate-background-color:#252525;}
    )");
  }
  QAction *addAction(QMenu *m, const QString &name, const QString &text,
                     const QStringList &keys, std::function<void()> fn) {
    auto *a = m->addAction(text);
    namedActions[name] = a;
    QList<QKeySequence> sequences;
    for (auto k : keys)
      sequences << QKeySequence(k);
    a->setShortcuts(sequences);
    QMainWindow::addAction(a);
    connect(a, &QAction::triggered, this, std::move(fn));
    return a;
  }
  void makeActions() {
    auto *file = menuBar()->addMenu("File");
    auto *edit = menuBar()->addMenu("Edit");
    auto *view = menuBar()->addMenu("View");
    auto *go = menuBar()->addMenu("Go");
    auto *window = menuBar()->addMenu("Window");
    menuBar()->setNativeMenuBar(true);
    menuBar()->hide();
    addAction(file, "newWindow", "New Finder Window", {"Ctrl+N", "Meta+N"},
              [this] {
                emit newWindowRequested(QUrl::fromLocalFile(QDir::homePath()));
              });
    addAction(file, "newTab", "New Tab", {"Ctrl+T", "Meta+T"},
              [this] { addTab(browser()->url(), true); });
    addAction(file, "newFolder", "New Folder", {"Ctrl+Shift+N", "Meta+Shift+N"},
              [this] {
                if (!showingSearch())
                  browser()->newFolder();
              });
    addAction(file, "open", "Open", {"Meta+Down"}, [this] {
      for (auto u : selectedUrls())
        openUrl(u);
    });
    addAction(file, "quickLook", "Quick Look", {},
              [this] { quickLook(selectedUrls()); });
    addAction(file, "rename", "Rename", {"F2"},
              [this] { selectedOperation("rename"); });
    addAction(file, "duplicate", "Duplicate", {"Ctrl+D", "Meta+D"},
              [this] { selectedOperation("duplicate"); });
    addAction(file, "trash", "Move to Trash",
              {"Delete", "Meta+Backspace", "Ctrl+Backspace"},
              [this] { selectedOperation("trash"); });
    addAction(file, "info", "Get Info", {"Ctrl+I", "Meta+I"}, [this] {
      auto urls = selectedUrls();
      auto *d = new KPropertiesDialog(
          urls.isEmpty() ? browser()->url() : urls.first(), this);
      d->setAttribute(Qt::WA_DeleteOnClose);
      d->show();
    });
    addAction(file, "close", "Close Tab", {"Ctrl+W", "Meta+W"},
              [this] { tabs->tabCloseRequested(tabs->currentIndex()); });
    addAction(edit, "undo", "Undo", {"Ctrl+Z", "Meta+Z"},
              [this] { browser()->undo(); });
    addAction(edit, "copy", "Copy", {"Ctrl+C", "Meta+C"}, [this] { copy(); });
    addAction(edit, "cut", "Cut", {"Ctrl+X", "Meta+X"}, [this] { copy(true); });
    addAction(edit, "paste", "Paste", {"Ctrl+V", "Meta+V"}, [this] {
      if (auto *line = qobject_cast<QLineEdit *>(QApplication::focusWidget()))
        line->paste();
      else if (qobject_cast<QTextEdit *>(QApplication::focusWidget()))
        return;
      else if (!showingSearch())
        browser()->paste();
    });
    addAction(edit, "moveHere", "Move Item Here", {"Ctrl+Alt+V", "Meta+Alt+V"},
              [this] {
                if (!showingSearch())
                  browser()->paste(true);
              });
    addAction(edit, "selectAll", "Select All", {"Ctrl+A", "Meta+A"}, [this] {
      if (auto *line = qobject_cast<QLineEdit *>(QApplication::focusWidget()))
        line->selectAll();
      else if (auto *text =
                   qobject_cast<QTextEdit *>(QApplication::focusWidget()))
        text->selectAll();
      else if (showingSearch())
        searchResults->selectAll();
      else
        browser()->activeView()->selectAll();
    });
    addAction(view, "search", "Find", {"Ctrl+F", "Meta+F"},
              [this] { showSearch(); });
    addAction(view, "viewOptions", "Show View Options", {"Ctrl+J", "Meta+J"},
              [this] { viewOptions(); });
    auto *hidden = addAction(
        view, "hidden", "Show Hidden Files", {"Ctrl+H", "Meta+Shift+."},
        [this] {
          browser()->setHiddenFiles(namedActions["hidden"]->isChecked());
          for (auto b : states.keys())
            if (b != browser())
              b->setHiddenFiles(namedActions["hidden"]->isChecked());
          if (showingSearch())
            beginSearch();
        });
    hidden->setCheckable(true);
    auto *path =
        addAction(view, "path", "Show Path Bar", {"Meta+Alt+P"}, [this] {
          crumbs->setVisible(namedActions["path"]->isChecked());
        });
    path->setCheckable(true);
    path->setChecked(true);
    for (int i = 0; i < 4; ++i)
      addAction(
          view, "mode" + QString::number(i),
          QStringList{"as Icons", "as List", "as Columns", "as Gallery"}[i],
          {"Ctrl+" + QString::number(i + 1), "Meta+" + QString::number(i + 1)},
          [this, i] { setMode(i); });
    addAction(go, "back", "Back", {"Alt+Left", "Meta+["},
              [this] { history(-1); });
    addAction(go, "forward", "Forward", {"Alt+Right", "Meta+]"},
              [this] { history(1); });
    addAction(go, "parent", "Enclosing Folder", {"Alt+Up", "Meta+Up"}, [this] {
      auto u = browser()->url();
      QString path = u.path();
      if (path.endsWith('/') && path != "/")
        path.chop(1);
      path = path.left(path.lastIndexOf('/') + 1);
      u.setPath(path.isEmpty() ? "/" : path);
      navigate(u);
    });
    addAction(go, "location", "Go to Folder…", {"Ctrl+L", "Meta+Shift+G"},
              [this] { revealLocation(); });
    for (auto entry : QList<QPair<QString, QStandardPaths::StandardLocation>>{
             {"Home", QStandardPaths::HomeLocation},
             {"Desktop", QStandardPaths::DesktopLocation},
             {"Documents", QStandardPaths::DocumentsLocation},
             {"Downloads", QStandardPaths::DownloadLocation}})
      addAction(go, "go" + entry.first, entry.first, {}, [this, entry] {
        navigate(QUrl::fromLocalFile(
            QStandardPaths::writableLocation(entry.second)));
      });
    addAction(window, "nextTab", "Next Tab", {"Ctrl+Tab"}, [this] {
      tabs->setCurrentIndex((tabs->currentIndex() + 1) % tabs->count());
    });
    addAction(window, "previousTab", "Previous Tab", {"Ctrl+Shift+Tab"},
              [this] {
                tabs->setCurrentIndex(
                    (tabs->currentIndex() + tabs->count() - 1) % tabs->count());
              });
    for (auto n : {"newFolder", "open", "quickLook", "rename", "duplicate",
                   "trash", "info"})
      actionsMenu->addAction(namedActions[n]);
    actionsMenu->addSeparator();
    actionsMenu->addAction(namedActions["hidden"]);
    actionsMenu->addAction(namedActions["path"]);
    actionsMenu->addAction(namedActions["viewOptions"]);
    actionsMenu->addSeparator();
    for (auto menu : {file, edit, view, go, window})
      actionsMenu->addMenu(menu);
    connect(KIO::FileUndoManager::self(), &KIO::FileUndoManager::undoAvailable,
            namedActions["undo"], &QAction::setEnabled);
    connect(KIO::FileUndoManager::self(),
            &KIO::FileUndoManager::undoTextChanged, namedActions["undo"],
            &QAction::setText);
    namedActions["undo"]->setEnabled(
        KIO::FileUndoManager::self()->isUndoAvailable());
  }
};

class Manager : public QObject {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.local.Finder")
public:
  QList<QPointer<FinderWindow>> windows;
  bool testing = false;
  bool daemon = false;
  FinderWindow *create(QUrl u) {
    auto *w = new FinderWindow(u);
    windows << w;
    connect(w, &FinderWindow::newWindowRequested, this,
            [this](const QUrl &url) { create(url); });
    connect(w, &QObject::destroyed, this, [this] {
      if (!daemon && windowCount() == 0)
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
    });
    w->setAttribute(Qt::WA_DeleteOnClose);
    w->show();
    return w;
  }
  FinderWindow *active() {
    for (auto &w : windows)
      if (w && w->isActiveWindow())
        return w;
    for (auto &w : windows)
      if (w)
        return w;
    return nullptr;
  }
public slots:
  int windowCount() {
    int n = 0;
    for (auto *w : QApplication::topLevelWidgets())
      if (w->isVisible())
        ++n;
    return n;
  }
  void showFolders(const QStringList &paths) {
    for (auto path : paths)
      create(
          QUrl::fromUserInput(path, QDir::homePath(), QUrl::AssumeLocalFile));
  }
  void showItems(const QStringList &paths) {
    if (paths.isEmpty())
      return;
    QList<QUrl> urls;
    for (auto p : paths)
      urls << QUrl::fromUserInput(p, QDir::homePath(), QUrl::AssumeLocalFile);
    QMap<QUrl, QList<QUrl>> groups;
    for (auto u : urls)
      groups[u.adjusted(QUrl::RemoveFilename)] << u;
    for (auto it = groups.begin(); it != groups.end(); ++it) {
      auto *w = create(it.key());
      w->showItems(it.value());
    }
  }
  void quickLook(const QStringList &paths) {
    QList<QUrl> urls;
    for (auto p : paths)
      urls << QUrl::fromUserInput(p, QDir::homePath(), QUrl::AssumeLocalFile);
    auto *w = active();
    if (!w)
      w = create(QUrl::fromLocalFile(QDir::homePath()));
    w->quickLook(urls);
  }
};
class FileManagerAPI : public QObject {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.freedesktop.FileManager1")
public:
  Manager *manager;
  explicit FileManagerAPI(Manager *m) : QObject(m), manager(m) {}
public slots:
  void ShowFolders(const QStringList &uris, const QString &) {
    manager->showFolders(uris);
  }
  void ShowItems(const QStringList &uris, const QString &) {
    manager->showItems(uris);
  }
  void ShowItemProperties(const QStringList &uris, const QString &) {
    for (auto uri : uris) {
      auto *d = new KPropertiesDialog(QUrl(uri));
      d->setAttribute(Qt::WA_DeleteOnClose);
      d->show();
    }
  }
};
class TestAPI : public QObject {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.local.FinderTest")
public:
  Manager *m;
  explicit TestAPI(Manager *manager) : QObject(manager), m(manager) {}
public slots:
  QString diagnostics() {
    if (!m->active())
      return "{}";
    return QString::fromUtf8(QJsonDocument(m->active()->diagnostics())
                                 .toJson(QJsonDocument::Compact));
  }
  bool capture(const QString &path) {
    return m->active() && m->active()->grab().save(path);
  }
  bool navigate(const QString &url) {
    if (!m->active())
      return false;
    m->active()->navigate(
        QUrl::fromUserInput(url, QDir::homePath(), QUrl::AssumeLocalFile));
    return true;
  }
  bool selectFiles(const QStringList &paths) {
    if (!m->active())
      return false;
    QList<QUrl> urls;
    for (auto p : paths)
      urls << QUrl::fromUserInput(p, QDir::homePath(), QUrl::AssumeLocalFile);
    if (auto *tree = m->active()->findChild<QTreeView *>("finderSearchResults");
        m->active()->showingSearch() && tree) {
      tree->selectionModel()->clearSelection();
      for (int row = 0; row < tree->model()->rowCount(); ++row) {
        auto index = tree->model()->index(row, 0);
        if (urls.contains(index.data(Qt::UserRole).toUrl()))
          tree->selectionModel()->select(index, QItemSelectionModel::Select |
                                                    QItemSelectionModel::Rows);
      }
      tree->setFocus();
      return true;
    }
    m->active()->browser()->selectUrls(urls);
    m->active()->browser()->activeView()->setFocus();
    return true;
  }
  bool setMode(int mode) {
    if (!m->active())
      return false;
    m->active()->setMode(mode);
    return true;
  }
  bool trigger(const QString &name) {
    if (!m->active() || !m->active()->action(name))
      return false;
    QTimer::singleShot(0, m->active()->action(name), &QAction::trigger);
    return true;
  }
  bool key(int code, int modifiers, const QString &text) {
    if (!m->active())
      return false;
    QPointer<FinderWindow> window = m->active();
    QTimer::singleShot(0, this, [window, code, modifiers, text] {
      if (window)
        window->key(code, modifiers, text);
    });
    return true;
  }
  bool dialogText(const QString &text) {
    for (auto *w : QApplication::topLevelWidgets())
      if (auto *d = qobject_cast<QInputDialog *>(w)) {
        d->setTextValue(text);
        d->accept();
        return true;
      }
    if (auto *line = qobject_cast<QLineEdit *>(QApplication::focusWidget())) {
      line->setText(text);
      QKeyEvent e(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
      QApplication::sendEvent(line, &e);
      return true;
    }
    if (auto *edit = qobject_cast<QTextEdit *>(QApplication::focusWidget())) {
      edit->setPlainText(text);
      QKeyEvent e(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
      QApplication::sendEvent(edit, &e);
      return true;
    }
    return false;
  }
  QString topLevels() {
    QJsonArray items;
    for (auto *w : QApplication::topLevelWidgets())
      if (w->isVisible()) {
        QJsonObject item{{"title", w->windowTitle()},
                         {"class", w->metaObject()->className()}};
        QJsonArray labels;
        for (auto *l : w->findChildren<QLabel *>())
          if (l->isVisible()) {
            auto pix = l->pixmap();
            labels << QJsonObject{{"name", l->objectName()},
                                  {"text", l->text()},
                                  {"imageWidth", pix.width()},
                                  {"imageHeight", pix.height()}};
          }
        for (auto *text : w->findChildren<QTextEdit *>())
          if (text->isVisible())
            labels << QJsonObject{{"name", text->objectName()},
                                  {"text", text->toPlainText().left(1000)},
                                  {"readOnly", text->isReadOnly()}};
        for (auto *stack : w->findChildren<QStackedWidget *>())
          if (stack->isVisible() && stack->currentWidget())
            labels << QJsonObject{
                {"name", stack->objectName()},
                {"current", stack->currentWidget()->objectName()}};
        item["labels"] = labels;
        items << item;
      }
    return QString::fromUtf8(
        QJsonDocument(items).toJson(QJsonDocument::Compact));
  }
  bool captureTopLevel(const QString &title, const QString &path) {
    for (auto *w : QApplication::topLevelWidgets())
      if (w->isVisible() && w->windowTitle() == title)
        return w->grab().save(path);
    return false;
  }
  bool clickObject(const QString &name) {
    auto windows = QApplication::topLevelWidgets();
    if (auto *active = QApplication::activeWindow()) {
      windows.removeOne(active);
      windows.prepend(active);
    }
    for (auto *w : windows)
      if (w->isVisible())
        if (auto *b = w->findChild<QAbstractButton *>(name);
            b && b->isVisible() && b->isEnabled()) {
          b->click();
          return true;
        }
    return false;
  }
  bool clickTopLevel(const QString &title, const QString &name) {
    for (auto *w : QApplication::topLevelWidgets())
      if (w->isVisible() && w->windowTitle() == title)
        if (auto *b = w->findChild<QAbstractButton *>(name);
            b && b->isVisible() && b->isEnabled()) {
          b->click();
          return true;
        }
    return false;
  }
  bool prepareTagMenu() {
    if (!m->active())
      return false;
    m->active()->prepareTagMenu();
    return true;
  }
  bool setControl(const QString &name, int value) {
    for (auto *w : QApplication::topLevelWidgets())
      if (w->isVisible()) {
        if (auto *b = w->findChild<QComboBox *>(name)) {
          b->setCurrentIndex(value);
          return true;
        }
        if (auto *b = w->findChild<QSpinBox *>(name)) {
          b->setValue(value);
          return true;
        }
        if (auto *b = w->findChild<QCheckBox *>(name)) {
          b->setChecked(value);
          return true;
        }
      }
    return false;
  }
  bool setText(const QString &name, const QString &text) {
    if (!m->active())
      return false;
    if (auto *line = m->active()->findChild<QLineEdit *>(name)) {
      line->setText(text);
      return true;
    }
    return false;
  }
  bool dialogButton(const QString &text) {
    for (auto *w : QApplication::topLevelWidgets())
      if (w->isVisible())
        for (auto *b : w->findChildren<QPushButton *>())
          if (b->text().remove('&') == text) {
            b->click();
            return true;
          }
    return false;
  }
  bool resizeWindow(int width, int height) {
    if (!m->active())
      return false;
    m->active()->resize(width, height);
    return true;
  }
  bool closeWindow() {
    if (!m->active())
      return false;
    m->active()->close();
    return true;
  }
};

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  app.setApplicationName("Finder");
  app.setOrganizationName("LocalDesktop");
  app.setDesktopFileName("org.local.finder");
  app.setQuitOnLastWindowClosed(false);
  app.setStyle("Fusion");
  QIcon::setThemeName("MacTahoe-dark");
  QFont font("Ubuntu", 10);
  font.setWeight(QFont::Medium);
  app.setFont(font);
  QPalette pal;
  pal.setColor(QPalette::Window, QColor("#26262a"));
  pal.setColor(QPalette::WindowText, QColor("#ededf2"));
  pal.setColor(QPalette::Base, QColor("#1e1e1e"));
  pal.setColor(QPalette::AlternateBase, QColor("#252525"));
  pal.setColor(QPalette::Text, QColor("#ededf2"));
  pal.setColor(QPalette::Button, QColor("#343438"));
  pal.setColor(QPalette::ButtonText, QColor("#ededf2"));
  pal.setColor(QPalette::Highlight, QColor("#3478f6"));
  pal.setColor(QPalette::HighlightedText, Qt::white);
  app.setPalette(pal);
  Manager manager;
  const QStringList args = app.arguments();
  manager.daemon = args.contains("--daemon");
  manager.testing = qEnvironmentVariable("KUBUNTU_VIRTUAL_SCREEN") == "1" &&
                    args.contains("--test-mode");
  auto bus = QDBusConnection::sessionBus();
  bool quick = args.contains("--quick-look"),
       select = args.contains("--select");
  QStringList paths;
  bool afterOptions = false;
  for (int i = 1; i < args.size(); ++i) {
    if (args[i] == "--" && !afterOptions) {
      afterOptions = true;
      continue;
    }
    if (!afterOptions && args[i].startsWith("--"))
      continue;
    paths << QUrl::fromUserInput(args[i], QDir::currentPath(),
                                 QUrl::AssumeLocalFile)
                 .toString();
  }
  if (!manager.testing && !bus.registerService("org.local.Finder")) {
    QDBusMessage msg = QDBusMessage::createMethodCall(
        "org.local.Finder", "/Finder", "org.local.Finder",
        quick    ? "quickLook"
        : select ? "showItems"
                 : "showFolders");
    if (paths.isEmpty() && !manager.daemon)
      paths << QDir::homePath();
    if (!paths.isEmpty()) {
      msg << paths;
      auto reply = bus.call(msg);
      return reply.type() == QDBusMessage::ErrorMessage ? 1 : 0;
    }
    return 0;
  }
  if (manager.testing)
    bus.registerService("org.local.Finder");
  bus.registerObject("/Finder", &manager, QDBusConnection::ExportAllSlots);
  FileManagerAPI fileApi(&manager);
  if (bus.registerService("org.freedesktop.FileManager1"))
    bus.registerObject("/org/freedesktop/FileManager1", &fileApi,
                       QDBusConnection::ExportAllSlots);
  TestAPI test(&manager);
  if (manager.testing) {
    bus.registerService("org.local.FinderTest");
    bus.registerObject("/Test", &test, QDBusConnection::ExportAllSlots);
  }
  if (paths.isEmpty() && !manager.daemon)
    paths << QDir::homePath();
  if (quick)
    manager.quickLook(paths);
  else if (select)
    manager.showItems(paths);
  else
    manager.showFolders(paths);
  return app.exec();
}
#include "main.moc"
