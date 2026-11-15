// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#pragma once

#include <QColor>
#include <QImage>
#include <QObject>
#include <QPainterPath>
#include <QPointer>
#include <QRect>
#include <QUrl>
#include <QWidget>
#include <memory>

class QMenu;
class QDialog;
class QMouseEvent;
class QKeyEvent;
struct QuickActionsState;

namespace FinderQuickActions {
enum class Action { ConvertImage, CreatePdf, Markup, RotateLeft, RotateRight };
struct ActionInfo {
    Action action;
    QString text;
    bool enabled = false;
    QString explanation;
};
struct Annotation {
    enum class Kind { Draw, Rectangle, Ellipse, Text };
    Kind kind = Kind::Draw;
    QPainterPath path; // Original auto-oriented image pixel coordinates.
    QRectF rect;
    QString text;
    QColor color = QColor(220, 45, 45);
    qreal width = 4; // Original pixels, including text size below.
    qreal textSize = 36;
};
struct ImageEdits {
    QRect crop; // Empty means the complete auto-oriented image.
    int quarterTurns = 0; // Clockwise; crop and annotations precede rotation.
    QList<Annotation> annotations;
};
struct ImageOptions {
    QByteArray format = "png";
    int maximumEdge = 0; // 0 retains original pixels. Never upscales.
    int quality = 95;
    QColor alphaBackground = Qt::white; // Used for JPEG/BMP.
};
struct PdfOptions {
    int rasterDpi = 150; // Existing PDFs are rasterized; no structural merge.
    QString title;
};

// Editable vector overlay on a bounded display raster. Export uses source
// pixels, never this preview. Coordinates remain stable across crop/rotation.
class ImageCanvas : public QWidget {
    Q_OBJECT
public:
    enum class Tool { Select, Draw, Rectangle, Ellipse, Text, Crop };
    explicit ImageCanvas(QWidget *parent = nullptr);
    void setImage(const QImage &preview, const QSize &originalSize);
    void setEdits(const ImageEdits &edits);
    ImageEdits edits() const;
    void setTool(Tool tool);
    void setColor(const QColor &color);
    void setStrokeWidth(qreal pixels);
    void setText(const QString &text);
    void rotate(int quarterTurns);
    void resetCrop();
    void undo();
    QSize originalSize() const;
    QTransform imageToWidget() const;
signals:
    void editsChanged();
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
private:
    void remember();
    QPointF sourcePoint(const QPointF &point) const;
    QRect cropRect() const;
    QImage m_preview;
    QSize m_original;
    ImageEdits m_edits;
    QList<ImageEdits> m_history;
    Tool m_tool = Tool::Draw;
    QColor m_color = QColor(220, 45, 45);
    qreal m_width = 4;
    QString m_text;
    bool m_dragging = false;
    int m_selected = -1;
    QPointF m_start, m_last;
    QRectF m_pendingCrop;
};
}

// Sources are local, regular, single-frame images/PDFs; destinations use KIO
// URLs. No source mutation. All decode/render/export/stat work runs off the
// GUI thread. One operation per controller, shared pool limited to two workers.
// Resource caps: 256 MiB input, 32M image pixels, 200 pages, 16M pixels/PDF
// raster, 256M total PDF pixels, 512 MiB output; 120s between work checkpoints.
// Cancellation is cooperative between codec calls; no GUI wait on destruction.
class QuickActions : public QObject {
    Q_OBJECT
public:
    explicit QuickActions(QWidget *dialogParent = nullptr);
    ~QuickActions() override;
    static QList<FinderQuickActions::ActionInfo> actionsForUrls(const QList<QUrl> &urls);
    static QList<QByteArray> writableImageFormats();
    // Adds only this module's actions; does not clear menus owned by the host.
    void populateMenu(QMenu *menu, const QList<QUrl> &urls);
    void trigger(FinderQuickActions::Action action, const QList<QUrl> &urls);
    bool busy() const;
    // Noninteractive entry points are useful for host shortcuts and fixtures.
    // Destinations must be explicit and distinct from every selected source.
    void exportImages(const QList<QUrl> &sources, const QList<QUrl> &destinations,
                      const FinderQuickActions::ImageOptions &options,
                      const FinderQuickActions::ImageEdits &edits = {});
    void createPdf(const QList<QUrl> &sources, const QUrl &destination,
                   const FinderQuickActions::PdfOptions &options = {});
public slots:
    void cancel();
signals:
    void busyChanged(bool busy);
    void statusChanged(const QString &message);
    void progressChanged(int completed, int total);
    void errorOccurred(const QString &message);
    void outputCreated(const QUrl &url); // Actual KIO target after rename/conflict.
    void outputsCreated(const QList<QUrl> &urls);
    void finished(bool cancelled); // Exactly once for each accepted operation.
private:
    void showConversion(const QList<QUrl> &sources);
    void showPdf(const QList<QUrl> &sources);
    void showEditor(const QUrl &source);
    void chooseImageDestination(const QList<QUrl> &sources,
                                const FinderQuickActions::ImageOptions &options,
                                const FinderQuickActions::ImageEdits &edits,
                                const QString &suffix);
    void publishNext();
    void showProgress();
    void complete(bool cancelled, const QString &error = {});
    void setBusy(bool busy);
    QPointer<QWidget> m_parent;
    QPointer<QDialog> m_dialog;
    QPointer<QDialog> m_progress;
    std::shared_ptr<QuickActionsState> m_state;
};
