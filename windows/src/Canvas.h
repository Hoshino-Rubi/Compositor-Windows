#pragma once
#include "Document.h"
#include "RetouchStroke.h"
#include "PreviewRenderer.h"
#include "GpuCanvasPresenter.h"
#include <QWidget>
#include <QFutureWatcher>
#include <QTimer>
#include <QTextEdit>
namespace comp {
enum class Tool {
    Move,
    Brush,
    Eraser,
    Rectangle,
    Ellipse,
    Text,
    Eyedropper,
    Hand,
    Crop,
    Marquee,
    Lasso,
    Wand,
    Clone,
    Blur,
    Gradient,
    Zoom,
    Healing,
    Line
};
inline const QStringList ToolKeys{"move",       "brush", "eraser",   "rectangle", "ellipse", "text",
                                  "eyedropper", "hand",  "crop",     "marquee",   "lasso",   "wand",
                                  "clone",      "blur",  "gradient", "zoom",      "healing", "line"};
class Canvas : public QWidget {
    Q_OBJECT
  public:
    explicit Canvas(Document *document, QWidget *parent = nullptr);
    ~Canvas() override;
    Tool tool() const { return tool_; }
    void setTool(Tool tool);
    void setColor(QColor color) { color_ = color; }
    QColor color() const { return color_; }
    void setBrushSize(int size) {
        brushSize_ = size;
        update();
    }
    int brushSize() const { return brushSize_; }
    void setCloneAligned(bool enabled) { cloneAligned_ = enabled; }
    void setCloneSampleAll(bool enabled) { cloneSampleAll_ = enabled; }
    void setHealingMode(int mode) { healingMode_ = std::clamp(mode, 0, 2); }
    void setBlurRadius(double radius) { blurRadius_ = std::clamp(radius, 1., 250.); }
    void setLineWidth(int width) { lineWidth_ = std::clamp(width, 1, 5000); }
    void setRetouchMode(int value) { retouchMode_ = value; }
    void setObjectMode(bool value) { objectMode_ = value; }
    void setPolygonal(bool value) {
        polygonal_ = value;
        polygonPoints_.clear();
        update();
    }
    void setFlow(double flow) { flow_ = flow; }
    void setMaskTarget(bool mask) { maskTarget_ = mask; }
    void setShapeNames(QString rectangle, QString ellipse, QString line) {
        rectangleName_ = std::move(rectangle);
        ellipseName_ = std::move(ellipse);
        lineName_ = std::move(line);
    }
    void setReferencePreview(bool enabled);
    void setCmykProof(bool enabled, bool gamut);
    void setGuides(bool visible) {
        guides_ = visible;
        update();
    }
    void setHardness(double value) { hardness_ = value; }
    void setSelectionMode(int value) { selectionMode_ = value; }
    void setEllipseSelection(bool value) { ellipseSelection_ = value; }
    void setWandTolerance(int value) { tolerance_ = value; }
    void setContiguous(bool value) { contiguous_ = value; }
    void setGradientRadial(bool value) { radial_ = value; }
    void setBackgroundColor(QColor value) { background_ = value; }
    void setTransparentGradient(bool value) { transparentGradient_ = value; }
    void setCornerRadius(double value) { cornerRadius_ = value; }
    void setSmoothCorners(bool value) { smoothCorners_ = value; }
    void setRulers(bool value) {
        rulers_ = value;
        update();
    }
    void setGrid(bool value) {
        grid_ = value;
        update();
    }
    void setSnap(bool value) { snap_ = value; }
    void setSnapTargets(bool guides, bool grid, bool layers, bool bounds) {
        snapGuides_ = guides; snapGrid_ = grid; snapLayers_ = layers; snapBounds_ = bounds;
    }
    void setLockGuides(bool locked) { lockGuides_ = locked; }
    void setFullscreenCanvas(bool value) { fullscreenCanvas_ = value; update(); }
    void setPixelGrid(bool visible) { pixelGrid_ = visible; update(); }
    void setTransformControls(bool visible) { transformControls_ = visible; update(); }
    void setGridSpacing(int value) {
        gridSpacing_ = value;
        update();
    }
    void setAutoSelect(bool value) { autoSelect_ = value; }
    void setSelectedLayers(QStringList ids) {
        selectedLayers_ = std::move(ids);
        update();
    }
    void setCropRatio(double value) { cropRatio_ = value; }
    void fit();
    void setZoom(double value);
    double zoom() const { return zoom_; }
    QPointF documentPoint(QPointF view) const;
    QPointF viewPoint(QPointF document) const;
    void cancelGesture();
    void beginInlineText(const QString &id);
    void finishInlineText(bool apply = true);
    bool editingText() const { return inlineText_ != nullptr; }
    bool eventFilter(QObject *, QEvent *) override;
  signals:
    void zoomChanged(double zoom);
    void colorPicked(QColor color);
    void textRequested(QPointF point);
    void filesDropped(QStringList paths);
    void failed(QString key, QString detail);
    void brushSizeChanged(int size);
    void flowChanged(int value);
    void toolChanged(Tool tool);
    void fullscreenRequested();
    void deleteRequested();
    void objectRequested(QPoint point);

  protected:
    QPaintEngine *paintEngine() const override;
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void keyReleaseEvent(QKeyEvent *) override;
    void focusOutEvent(QFocusEvent *) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dropEvent(QDropEvent *) override;

  private:
    QTextEdit *inlineText_ = nullptr;
    QString inlineTextId_;
    Document *document_;
    Tool tool_ = Tool::Move;
    QColor color_{"#ed6a45"};
    int brushSize_ = 28;
    double flow_ = 1, zoom_ = 1;
    QPointF offset_, anchor_, last_, mouse_;
    bool dragging_ = false, panning_ = false, space_ = false, maskTarget_ = false, guides_ = true, resizing_ = false;
    State start_;
    QRectF draftRect_;
    QByteArray cmykProfile_;
    bool cmykProof_ = false, cmykGamut_ = false;
    bool nativePresentation_ = false, gpuPresentationFailed_ = false, referencePreview_ = false;
    std::unique_ptr<GpuCanvasPresenter> gpuPresenter_;
    QImage preview_;
    CanvasPreview frame_;
    quint64 contentRevision_ = 1;
    std::shared_ptr<std::atomic_bool> renderCancelled_;
    quint64 generation_ = 0, runningGeneration_ = 0;
    QFutureWatcher<CanvasPreview> watcher_;
    QTimer renderTimer_;
    QString renderedDocument_;
    QString rectangleName_ = "Rectangle", ellipseName_ = "Ellipse", lineName_ = "Line";
    void scheduleRender();
    void startRender();
    void paintStroke(QPointF from, QPointF to);
    void finishGradient();
    bool isBrush() const;
    void finishHealing();
    QPainterPath healingPath_;
    bool smoothCorners_ = false;
    double hardness_ = 1, cornerRadius_ = 0, cropRatio_ = 0;
    int selectionMode_ = 0, gestureSelectionMode_ = 0, tolerance_ = 32, gridSpacing_ = 100;
    bool ellipseSelection_ = false, contiguous_ = true, radial_ = false, transparentGradient_ = true;
    bool rulers_ = false, grid_ = false, snap_ = true, autoSelect_ = true, guideDrag_ = false, guideHorizontal_ = false;
    bool selectionDrag_ = false, cloneReady_ = false, rotating_ = false;
    bool snapGuides_ = true, snapGrid_ = false, snapLayers_ = true, snapBounds_ = true;
    bool lockGuides_ = false, pixelGrid_ = false, transformControls_ = true, fullscreenCanvas_ = false;
    int movingGuide_ = -1;
    int resizeHandle_ = 2, lineWidth_ = 4, healingMode_ = 0;
    double blurRadius_ = 5;
    bool cloneAligned_ = true, cloneSampleAll_ = false, cloneAnchorChanged_ = true;
    QStringList selectedLayers_;
    QVector<int> movingIndices_;
    QColor background_{Qt::white};
    QPointF cloneSource_, cloneOffset_;
    QImage retouchSource_;
    std::unique_ptr<RetouchStroke> retouchStroke_;
    int retouchMode_ = 0;
    bool polygonal_ = false, objectMode_ = false;
    void setZoomCursor(bool out);
    bool middleZoom_ = false, navigatorDrag_ = false, toolZoom_ = false, zoomMoved_ = false;
    QRectF navigatorRect() const;
    void navigateTo(QPointF point);
    QPolygonF polygonPoints_;
    QPainterPath selectionDraft_, selectionStart_;

    void snapTargets(const State &state, QVector<double> &xs, QVector<double> &ys, const QVector<int> &excluded) const;
    void finishShape();
    QPolygonF selectedPolygon() const;
};
} // namespace comp
