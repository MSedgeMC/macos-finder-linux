// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#pragma once

#include <KFilePlacesView>
#include <memory>

class QTreeView;

// The native view remains the bookmark/device backend. Its private grouped
// delegate is never replaced: a separate presentation owns visible hit areas.
class FinderPlacesView : public KFilePlacesView {
    Q_OBJECT
public:
    explicit FinderPlacesView(QWidget *parent = nullptr);
    ~FinderPlacesView() override;

    void setModel(QAbstractItemModel *model) override;
    QTreeView *presentationView() const;
    void setTeardownFunction(TeardownFunction teardown);

public slots:
    void setUrl(const QUrl &url);
    void setTag(const QString &tag);

signals:
    // Color names match the real user.xdg.tags values used by FileTags.
    // The application supplies the results view; no indexed/fake KIO URL is
    // substituted for a tag search.
    void tagActivated(QString tag);
    void errorMessage(const QString &message);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    class Private;
    std::unique_ptr<Private> d;
};
