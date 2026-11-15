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
#include <QImageReader>
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
#include <QSplitterHandle>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
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
  int sidebarWidth = 256;
  using QWidget::QWidget;

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.fillRect(rect(), QColor(34, 34, 37, blur ? 120 : 255));
    if (objectName() == "finderHeader") {
      // The sidebar material continues through its titlebar; the content
      // toolbar has a quiet solid base beneath its grouped glass controls.
      p.fillRect(QRect(sidebarWidth, 0, width() - sidebarWidth, height()),
                 QColor(30, 30, 30));
      p.setPen(QColor(255, 255, 255, 26));
      p.drawLine(0, 0, width(), 0);
      p.setPen(QColor(0, 0, 0, 60));
      p.drawLine(0, height() - 1, width(), height() - 1);
    }
  }
};
class WindowControl : public QToolButton {
public:
  WindowControl(QString kind, QWidget *parent) : QToolButton(parent), kind(std::move(kind)) {
    setFixedSize(20, 28);
    setObjectName("window-" + this->kind);
    setAccessibleName(this->kind == "close" ? "Close" : this->kind == "minimize" ? "Minimize" : "Zoom");
    setToolTip(accessibleName());
    setFocusPolicy(Qt::StrongFocus);
  }
protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QColor color(kind == "close" ? "#ff5f57" : kind == "minimize" ? "#febc2e" : "#28c840");
    if (!window()->isActiveWindow()) color = QColor("#646466");
    p.setPen(QPen(color.darker(115), .5));
    p.setBrush(color);
    p.drawEllipse(QRectF(1, 7, 14, 14));
    if (underMouse() || hasFocus()) {
      p.setPen(QPen(QColor(25, 25, 25, 175), 1.2));
      if (kind == "close") { p.drawLine(5, 11, 11, 17); p.drawLine(11, 11, 5, 17); }
      else if (kind == "minimize") p.drawLine(5, 14, 11, 14);
      else { p.drawLine(5, 14, 11, 14); p.drawLine(8, 11, 8, 17); }
    }
  }
  void enterEvent(QEnterEvent *event) override { QToolButton::enterEvent(event); update(); }
  void leaveEvent(QEvent *event) override { QToolButton::leaveEvent(event); update(); }
private:
  QString kind;
};
class SidebarHandle : public QSplitterHandle {
public:
  using QSplitterHandle::QSplitterHandle;

protected:
  // The zero-width handle overlaps both adjoining panes for native resize
  // input. Leave their pixels visible instead of painting a style separator.
  void paintEvent(QPaintEvent *) override {}
};
class SidebarSplitter : public QSplitter {
public:
  explicit SidebarSplitter(QWidget *parent = nullptr)
      : QSplitter(Qt::Horizontal, parent) {
    // Qt retains a wider overlapping grab area when the layout width is zero.
    // No transparent layout strip is reserved between sidebar and file pane.
    setHandleWidth(0);
  }

protected:
  QSplitterHandle *createHandle() override {
    auto *handle = new SidebarHandle(orientation(), this);
    handle->setAccessibleName("Resize Sidebar");
    return handle;
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
  PreviewPane *gallery = nullptr, *column = nullptr, *preview = nullptr;
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
    hs->setContentsMargins(5, 0, 6, 0);
    hs->setSpacing(7);
    for (auto name : {"close", "minimize", "maximize"}) {
      auto *control = new WindowControl(QString(name), headerSidebar);
      hs->addWidget(control);
      connect(control, &QToolButton::clicked, this, [this, name] {
        if (QString(name) == "minimize") showMinimized();
        else if (QString(name) == "maximize") {
          if (isMaximized()) showNormal(); else showMaximized();
        } else close();
      });
    }
    hs->addStretch();
    auto *sidebarButton = button("sidebar", "Show Sidebar", headerSidebar);
    sidebarButton->setObjectName("finderSidebarToggle");
    hs->addWidget(sidebarButton);
    headerSidebar->setFixedWidth(244);
    hl->addWidget(headerSidebar);
    connect(sidebarButton, &QToolButton::clicked, this, [this] {
      sidebar->setVisible(!sidebar->isVisible());
      layoutHeader();
      updateMaterial();
    });
    auto *nav = new QWidget;
    nav->setObjectName("navigationCapsule");
    auto *nl = new QHBoxLayout(nav);
    nl->setContentsMargins(2, 0, 3, 0);
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
    ml->setContentsMargins(3, 0, 3, 0);
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
    arrange->setObjectName("finderArrange");
    arrange->setFixedWidth(44);
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
    sortMenu->addSeparator();
    auto *groups = sortMenu->addMenu("Group By");
    groupActions = new QActionGroup(this);
    groupActions->setExclusive(true);
    const QStringList grouping{"None", "Name", "Kind", "Date Modified", "Size", "Tags"};
    for (int i = 0; i < grouping.size(); ++i) {
      auto *action = groups->addAction(grouping[i]);
      action->setCheckable(true);
      action->setData(i);
      action->setChecked(i == 0);
      groupActions->addAction(action);
      namedActions["group" + QString::number(i)] = action;
      connect(action, &QAction::triggered, this, [this, i] {
        browser()->setGroup(Browser::Group(i)); saveView(); refresh();
      });
    }
    arrange->setPopupMode(QToolButton::InstantPopup);
    hl->addWidget(arrange);
    auto *actionsCapsule = new QWidget(header);
    actionsCapsule->setObjectName("actionsCapsule");
    auto *actionsLayout = new QHBoxLayout(actionsCapsule);
    actionsLayout->setContentsMargins(3, 0, 3, 0);
    actionsLayout->setSpacing(0);
    share = button("share", "Share", actionsCapsule);
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
    shareMenu->addAction("Email…", this, [this] {
      const auto urls = selectedUrls();
      if (urls.isEmpty()) return;
      QStringList arguments;
      for (const auto &url : urls) {
        if (!url.isLocalFile()) { status->setText("Download remote files before sending attachments."); return; }
        arguments << "--attach" << url.toLocalFile();
      }
      if (!QProcess::startDetached("xdg-email", arguments)) status->setText("Could not start the email application.");
    });
    share->setMenu(shareMenu);
    share->setPopupMode(QToolButton::InstantPopup);
    actionsLayout->addWidget(share);
    tagButton = button("tags", "Tags", actionsCapsule);
    tagMenu = new QMenu(tagButton);
    tagButton->setMenu(tagMenu);
    tagButton->setPopupMode(QToolButton::InstantPopup);
    actionsLayout->addWidget(tagButton);
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
    auto *more = button("more", "Actions", actionsCapsule);
    actionsMenu = new QMenu(more);
    more->setMenu(actionsMenu);
    more->setPopupMode(QToolButton::InstantPopup);
    actionsLayout->addWidget(more);
    hl->addWidget(actionsCapsule);
    search = new QLineEdit;
    search->setObjectName("finderSearch");
    search->setPlaceholderText("Search this folder");
    search->setClearButtonEnabled(true);
    search->setFixedSize(159, 36);
    search->hide();
    hl->addWidget(search);
    connect(search, &QLineEdit::textChanged, this, [this](const QString &t) {
      Q_UNUSED(t);
      if (searchTimer)
        searchTimer->start();
    });
    auto *searchButton = button("search", "Search", header);
    searchButton->setObjectName("finderSearchButton");
    searchButton->setFixedWidth(36);
    connect(searchButton, &QToolButton::clicked, this,
            [this] { showSearch(); });
    hl->addWidget(searchButton);
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
    body = new SidebarSplitter;
    body->setObjectName("finderBody");
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
    connect(places, &FinderPlacesView::tagActivated, this, [this](const QString &tag) {
      selectedTag = tag;
      search->hide();
      searchModel->removeRows(0, searchModel->rowCount());
      searchRoot = QUrl::fromLocalFile(QDir::homePath());
      contentStack->setCurrentWidget(searchPage);
      searchSummary->setText("Tags: " + tag + " — Searching…");
      LocalFileSearch::Options options;
      options.requiredTag = tag;
      options.showHidden = true;
      fileSearch->start(searchRoot, QString(), options);
      refresh();
    });
    connect(places, &FinderPlacesView::errorMessage, this, [this](const QString &error) {
      status->setText(error);
    });
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
    contentStack->setBackgroundRole(QPalette::Base);
    contentStack->setAutoFillBackground(true);
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
    auto *rightPane = new QWidget;
    auto *rightLayout = new QVBoxLayout(rightPane);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);
    rightLayout->addWidget(contentStack, 1);
    body->addWidget(rightPane);
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
                      .arg(selectedTag.isEmpty() ? title(searchRoot) : "Tags: " + selectedTag)
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
    statusRow = new QWidget;
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
    size->setValue(64);
    size->setFixedWidth(100);
    size->setToolTip("Icon size");
    connect(size, &QSlider::valueChanged, this, [this](int px) {
      if (browser())
        browser()->setIconSize(px);
      saveView();
    });
    sr->addWidget(size);
    bl->addWidget(statusRow);
    rightLayout->addWidget(bottom);
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
      connect(p, &PreviewPane::getInfoRequested, this, [this](const QUrl &url) {
        auto *dialog = new KPropertiesDialog(url, this); dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->show();
      });
      connect(p, &PreviewPane::rotateCopyRequested, this, [this](const QUrl &url) {
        if (!url.isLocalFile()) return;
        auto temporary = std::make_shared<QTemporaryDir>();
        if (!temporary->isValid()) { status->setText("Could not create the rotated copy."); return; }
        const auto staged = temporary->filePath("rotated.png");
        auto error = std::make_shared<QString>();
        auto *worker = QThread::create([url, staged, error, temporary] {
          QImageReader reader(url.toLocalFile()); reader.setAutoTransform(true);
          const auto size = reader.size();
          if (!size.isValid() || qint64(size.width()) * size.height() > 64000000) {
            *error = "This image is too large to rotate safely."; return;
          }
          const auto image = reader.read();
          if (image.isNull()) { *error = reader.errorString(); return; }
          auto rotated = image.transformed(QTransform().rotate(-90));
          rotated.setDotsPerMeterX(image.dotsPerMeterY());
          rotated.setDotsPerMeterY(image.dotsPerMeterX());
          if (!rotated.save(staged, "PNG"))
            *error = "Could not write the rotated image copy.";
        });
        connect(worker, &QThread::finished, this, [this, url, staged, error, temporary] {
          if (!error->isEmpty()) { status->setText(*error); return; }
          const QFileInfo original(url.toLocalFile());
          const QString stem = original.completeBaseName() + " rotated";
          QString destination = original.dir().filePath(stem + ".png");
          int number = 2;
          while (QFileInfo::exists(destination)) destination = original.dir().filePath(stem + " " + QString::number(number++) + ".png");
          auto *job = KIO::copyAs(QUrl::fromLocalFile(staged), QUrl::fromLocalFile(destination));
          job->setUiDelegate(KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, this));
          KIO::FileUndoManager::self()->recordCopyJob(job);
          connect(job, &KJob::result, this, [this, temporary](KJob *finished) {
            status->setText(finished->error() ? finished->errorText() : "Rotated copy created");
          });
        });
        connect(worker, &QThread::finished, worker, &QObject::deleteLater); worker->start();
      });
      connect(p, &PreviewPane::tagChangeRequested, this, [this, p](const QUrl &url, const QString &tag, int operation) {
        if (writingTags) { p->tagChangeFinished(url, "A tag update is already running."); return; }
        writingTags = true; ++tagGeneration;
        auto *job = new TagsJob; job->urls = {url}; job->tag = tag; job->operation = operation;
        QPointer<PreviewPane> pane(p);
        connect(job, &TagsJob::done, this, [this, pane, url](QString error, QStringList) {
          writingTags = false;
          if (pane) pane->tagChangeFinished(url, error);
          for (auto &state : states) { state.gallery->refreshTags(); state.column->refreshTags(); state.preview->refreshTags(); }
          if (browser()) browser()->refreshTags();
          updateTags();
        });
        connect(job, &QThread::finished, job, &QObject::deleteLater); job->start();
      });
      return p;
    };
    s.gallery = attach(b->galleryPreviewHost());
    s.gallery->setGallery(true);
    s.column = attach(b->columnPreviewHost());
    s.preview = attach(b->previewHost());
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
              if (b->mode() == Browser::Gallery) s.gallery->setUrls(urls);
              if (b->mode() == Browser::Columns) s.column->setUrls(urls);
              if (b->previewShown()) s.preview->setUrls(urls);
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
        {"group", int(browser()->group())},
        {"gridSpacing", browser()->gridSpacing()},
        {"previewShown", browser()->previewShown()},
        {"sortAscending", browser()->sortAscending()},
        {"tagsBusy", writingTags},
        {"tagsReady", !tagActions.isEmpty() && tagActions.first()->isEnabled()},
        {"hiddenColumns", hiddenColumns},
        {"count", browser()->itemCount()},
        {"selected", selected},
        {"tabs", tabs->count()},
        {"headerHeight", header->height()},
        {"sidebarWidth", sidebar->width()},
        {"sidebarVisible", sidebar->isVisible()},
        {"bodyY", body->y()},
        {"bodyHeight", body->height()},
        {"contentX", contentStack->mapTo(this, QPoint(0, 0)).x()},
        {"splitterGrabWidth", body->handle(1)->width()},
        {"navigationWidth", back->parentWidget()->width()},
        {"viewCapsuleWidth", modeBox->width()},
        {"visibleLists", columns},
        {"focus", f ? f->metaObject()->className() : ""},
        {"search", search->text()},
        {"width", width()},
        {"height", height()},
        {"blur", header->blur}};
  }
  void testMaterialBlur(bool enabled) {
    setProperty("finderDisableBlur", !enabled);
    updateMaterial();
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
            (showingSearch() || search->isVisible() || location->isVisible())) {
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
  QString selectedTag;
  QUrl capacityUrl;
  QString capacityText;
  QWidget *bottom, *crumbs, *statusRow, *modeBox, *headerSidebar;
  QHBoxLayout *crumbLayout;
  QLabel *folderTitle, *status;
  FinderPlacesView *places;
  QLineEdit *location, *search;
  QToolButton *back, *forward, *modePopup, *arrange, *share;
  QSlider *iconSlider;
  QActionGroup *modeActions, *sortGroup, *groupActions;
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
      if (browser()) browser()->refreshTags();
      for (auto &state : states) { state.gallery->refreshTags(); state.column->refreshTags(); state.preview->refreshTags(); }
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
    selectedTag.clear();
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
    selectedTag.clear();
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
    const bool narrow = width() < 850 || search->isVisible();
    modeBox->setVisible(!narrow);
    modePopup->setVisible(narrow);
    headerSidebar->setFixedWidth(
        sidebar->isVisible() ? qMax(94, sidebar->width() - 12) : 94);
    arrange->setVisible(width() >= 740 && !search->isVisible());
    share->setVisible(width() >= 780 && !search->isVisible());
    tagButton->setVisible(width() >= 780 && !search->isVisible());
    header->sidebarWidth = sidebar->isVisible() ? sidebar->width() : 106;
    header->update();
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
    settings.setValue("group", int(browser()->group()));
    settings.setValue("gridSpacing", browser()->gridSpacing());
    settings.setValue("textSize", browser()->textSize());
    settings.setValue("labelPosition", int(browser()->labelPosition()));
    settings.setValue("iconPreviews", browser()->iconPreviewsShown());
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
            .value("iconSize", QSettings().value("View/defaultIconSize", 64))
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
    browser()->setGroup(Browser::Group(settings.value("group", 0).toInt()));
    browser()->setGridSpacing(settings.value("gridSpacing", 16).toInt());
    browser()->setTextSize(settings.value("textSize", 13).toInt());
    browser()->setLabelPosition(Browser::LabelPosition(settings.value("labelPosition", 0).toInt()));
    browser()->setIconPreviewsShown(settings.value("iconPreviews", true).toBool());
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
    auto *spacing = new QSpinBox;
    spacing->setObjectName("viewOptionGridSpacing");
    spacing->setRange(0, 64); spacing->setValue(browser()->gridSpacing());
    form->addRow("Grid spacing", spacing);
    auto *textSize = new QSpinBox;
    textSize->setObjectName("viewOptionTextSize");
    textSize->setRange(10, 20); textSize->setValue(browser()->textSize());
    form->addRow("Text size", textSize);
    auto *labels = new QComboBox;
    labels->setObjectName("viewOptionLabels"); labels->addItems({"Bottom", "Right"});
    labels->setCurrentIndex(int(browser()->labelPosition()));
    form->addRow("Label position", labels);
    auto *previews = new QCheckBox("Show icon preview");
    previews->setObjectName("viewOptionIconPreviews"); previews->setChecked(browser()->iconPreviewsShown());
    form->addRow(previews);
    auto *group = new QComboBox;
    group->setObjectName("viewOptionGroup"); group->addItems({"None", "Name", "Kind", "Date Modified", "Size", "Tags"});
    group->setCurrentIndex(int(browser()->group())); form->addRow("Group by", group);
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
    defaults->setEnabled(browser()->mode() != Browser::Columns);
    form->addRow(defaults);
    auto *buttons =
        new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Save);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, dialog,
            [this, dialog, mode, size, spacing, textSize, labels, previews, group, sort, reverse, columns, defaults] {
              setMode(mode->currentIndex());
              browser()->setIconSize(size->value());
              browser()->setGridSpacing(spacing->value());
              browser()->setTextSize(textSize->value());
              browser()->setLabelPosition(Browser::LabelPosition(labels->currentIndex()));
              browser()->setIconPreviewsShown(previews->isChecked());
              browser()->setGroup(Browser::Group(group->currentIndex()));
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
    if (!selectedTag.isEmpty() && showingSearch()) places->setTag(selectedTag);
    else places->setUrl(u);
    auto &s = states[browser()];
    back->setEnabled(s.historyIndex > 0);
    forward->setEnabled(s.historyIndex + 1 < s.history.size());
    modeActions->actions()[int(browser()->mode())]->setChecked(true);
    modePopup->setIcon(symbol(QStringList{"icons", "list", "columns",
                                          "gallery"}[int(browser()->mode())]));
    auto &state = states[browser()];
    const auto previewUrls = browser()->selectedUrls();
    if (browser()->mode() == Browser::Gallery) state.gallery->setUrls(previewUrls);
    if (browser()->mode() == Browser::Columns) state.column->setUrls(previewUrls);
    if (browser()->previewShown()) state.preview->setUrls(previewUrls);
    for (auto *action : groupActions->actions()) action->setChecked(action->data().toInt() == int(browser()->group()));
    if (namedActions.contains("compress")) namedActions["compress"]->setEnabled(!showingSearch() && browser()->canCompressSelection());
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
    const bool supported = KWindowEffects::isEffectAvailable(KWindowEffects::BlurBehind);
    const bool available = supported && !property("finderDisableBlur").toBool();
    header->blur = sidebar->blur = supported;
    header->update();
    sidebar->update();
    QRegion region(QRect(0, 0, header->sidebarWidth, header->height()));
    if (sidebar->isVisible())
      region += QRegion(
          sidebar->rect().translated(sidebar->mapTo(this, QPoint(0, 0))));
    KWindowEffects::enableBlurBehind(windowHandle(), available, region);
    KWindowEffects::enableBackgroundContrast(windowHandle(), available, 1.04, 1.10, 1.0, region);
  }
  void applyStyle() {
    setStyleSheet(R"(
        QWidget{color:#ededf2;font-weight:400;} QMainWindow{background:transparent;}
        #finderFolderTitle{font-size:14px;font-weight:600;background:transparent;}
        #navigationCapsule,#modeCapsule,#actionsCapsule{background:rgba(255,255,255,12);border:1px solid rgba(255,255,255,25);border-radius:18px;}
        #finderArrange,#finderSearchButton{background:rgba(255,255,255,8);border:1px solid rgba(255,255,255,22);border-radius:18px;}
        QToolButton{border:0;border-radius:16px;background:transparent;} QToolButton:hover{background:rgba(255,255,255,20);} QToolButton:checked{background:rgba(255,255,255,35);} QToolButton:disabled{opacity:.4;}
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
    auto *finder = menuBar()->addMenu("Finder");
    auto *file = menuBar()->addMenu("File");
    auto *edit = menuBar()->addMenu("Edit");
    auto *view = menuBar()->addMenu("View");
    auto *go = menuBar()->addMenu("Go");
    auto *window = menuBar()->addMenu("Window");
    menuBar()->setNativeMenuBar(true);
    menuBar()->hide();
    finder->addAction("About Finder", this, [this] {
      auto *dialog = new QDialog(this); dialog->setAttribute(Qt::WA_DeleteOnClose);
      dialog->setWindowTitle("About Finder for Linux");
      auto *layout = new QVBoxLayout(dialog);
      auto *label = new QLabel("Finder for Linux\nNative Qt / KDE file management", dialog);
      label->setAlignment(Qt::AlignCenter); layout->addWidget(label);
      auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
      connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
      layout->addWidget(buttons); dialog->open();
    });
    addAction(finder, "settings", "Settings…", {"Meta+,", "Ctrl+,"}, [this] { viewOptions(); });
    finder->addSeparator();
    addAction(finder, "quit", "Quit Finder", {"Ctrl+Q", "Meta+Q"}, [this] { close(); });
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
    addAction(file, "compress", "Compress", {}, [this] {
      if (!showingSearch()) browser()->compressSelected();
    });
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
          QSettings().setValue("Window/pathBar", namedActions["path"]->isChecked());
        });
    path->setCheckable(true);
    path->setChecked(QSettings().value("Window/pathBar", false).toBool());
    crumbs->setVisible(path->isChecked());
    auto *statusAction = addAction(view, "statusBar", "Show Status Bar", {"Meta+/"}, [this] {
      statusRow->setVisible(namedActions["statusBar"]->isChecked());
      QSettings().setValue("Window/statusBar", namedActions["statusBar"]->isChecked());
    });
    statusAction->setCheckable(true);
    statusAction->setChecked(QSettings().value("Window/statusBar", false).toBool());
    statusRow->setVisible(statusAction->isChecked());
    auto *previewAction = addAction(view, "preview", "Show Preview", {"Ctrl+Shift+P", "Meta+Shift+P"}, [this] {
      browser()->setPreviewShown(namedActions["preview"]->isChecked()); refresh();
    });
    previewAction->setCheckable(true);
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
    for (auto n : {"newFolder", "open", "quickLook", "rename", "duplicate", "compress",
                   "trash", "info"})
      actionsMenu->addAction(namedActions[n]);
    actionsMenu->addSeparator();
    actionsMenu->addAction(namedActions["hidden"]);
    actionsMenu->addAction(namedActions["path"]);
    actionsMenu->addAction(namedActions["statusBar"]);
    actionsMenu->addAction(namedActions["preview"]);
    actionsMenu->addAction(namedActions["viewOptions"]);
    actionsMenu->addSeparator();
    for (auto menu : {finder, file, edit, view, go, window})
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
  QString sidebarRows() {
    QJsonArray rows;
    if (auto *window = m->active()) {
      auto *places = window->findChild<FinderPlacesView *>("finderPlaces");
      if (places && places->presentationView()) {
        auto *tree = places->presentationView();
        for (int i = 0; i < tree->model()->rowCount(); ++i) {
          const auto index = tree->model()->index(i, 0);
          const auto rect = tree->visualRect(index);
          rows << QJsonObject{{"text", index.data().toString()}, {"height", rect.height()},
                              {"selectable", bool(index.flags() & Qt::ItemIsSelectable)}};
        }
      }
    }
    return QString::fromUtf8(QJsonDocument(rows).toJson(QJsonDocument::Compact));
  }
  bool activateSidebar(const QString &label) {
    auto *window = m->active();
    if (!window) return false;
    auto *places = window->findChild<FinderPlacesView *>("finderPlaces");
    if (!places || !places->presentationView()) return false;
    auto *tree = places->presentationView();
    for (int i = 0; i < tree->model()->rowCount(); ++i) {
      const auto index = tree->model()->index(i, 0);
      if (index.data().toString() != label) continue;
      tree->scrollTo(index);
      const auto pos = tree->visualRect(index).center();
      const auto global = tree->viewport()->mapToGlobal(pos);
      QMouseEvent press(QEvent::MouseButtonPress, QPointF(pos), QPointF(global), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
      QApplication::sendEvent(tree->viewport(), &press);
      QMouseEvent release(QEvent::MouseButtonRelease, QPointF(pos), QPointF(global), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
      QApplication::sendEvent(tree->viewport(), &release);
      return true;
    }
    return false;
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
  bool moveWindow(int x, int y) {
    if (!m->active()) return false;
    m->active()->move(x, y);
    return true;
  }
  bool setMaterialBlur(bool enabled) {
    if (!m->active()) return false;
    m->active()->testMaterialBlur(enabled);
    return true;
  }
  bool dragSidebar(int distance) {
    if (!m->active())
      return false;
    auto *splitter = m->active()->findChild<QSplitter *>("finderBody");
    if (!splitter || !splitter->handle(1)->isVisible())
      return false;
    auto *handle = splitter->handle(1);
    const QPoint start = handle->rect().center();
    const QPoint globalStart = handle->mapToGlobal(start);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(start),
                      QPointF(globalStart), Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(handle, &press);
    const QPoint globalEnd = globalStart + QPoint(distance, 0);
    QMouseEvent move(QEvent::MouseMove,
                     QPointF(handle->mapFromGlobal(globalEnd)),
                     QPointF(globalEnd), Qt::NoButton, Qt::LeftButton,
                     Qt::NoModifier);
    QApplication::sendEvent(handle, &move);
    QMouseEvent release(QEvent::MouseButtonRelease,
                        QPointF(handle->mapFromGlobal(globalEnd)),
                        QPointF(globalEnd), Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(handle, &release);
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
  // Use an already installed native reference font when available. The repo
  // does not ship or install Apple fonts; Inter is the portable fallback.
  QFont font("SF Pro Display", 10);
  font.setFamilies({"SF Pro Display", "Inter", "sans-serif"});
  font.setPixelSize(13);
  font.setWeight(QFont::Normal);
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
