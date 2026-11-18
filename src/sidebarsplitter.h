// SPDX-License-Identifier: MIT
#pragma once
#include <QSplitter>
#include <QSplitterHandle>

// Qt keeps an overlapping native grab area at zero layout width. Painting
// nothing leaves the adjacent surfaces visible while retaining resize input.
class SidebarHandle : public QSplitterHandle {
public:
  using QSplitterHandle::QSplitterHandle;
protected:
  void paintEvent(QPaintEvent *) override {}
};
class SidebarSplitter : public QSplitter {
public:
  explicit SidebarSplitter(QWidget *parent = nullptr)
      : QSplitter(Qt::Horizontal, parent) { setHandleWidth(0); }
protected:
  QSplitterHandle *createHandle() override {
    auto *handle = new SidebarHandle(orientation(), this);
    handle->setAccessibleName(tr("Resize Sidebar"));
    return handle;
  }
};
