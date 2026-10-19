#include "WideKernels.h"
#include "SpatialFilters.h"
#include "Cmyk.h"
#include "Pixels.h"
#include "Canvas.h"
#include "Renderer.h"
#include "Filters.h"
#include "GpuNative.h"
#include "GpuPreview.h"
#include "Preferences.h"
#include <QGuiApplication>
#include "Warp.h"
#include "Selection.h"
#include "Text.h"
#include <QtConcurrent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QWheelEvent>
#include <cmath>
extern "C" {
#include "HealPixels.h"
}

namespace comp {
Canvas::Canvas(Document *document, QWidget *parent) : QWidget(parent), document_(document) {
    setObjectName("canvas");
    setMinimumSize(320, 240);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAcceptDrops(true);
    renderTimer_.setSingleShot(true);
    renderTimer_.setInterval(12);
    connect(&renderTimer_, &QTimer::timeout, this, &Canvas::startRender);
    connect(document_, &Document::changed, this, [this] {
        ++contentRevision_;
        frame_.tiles.clear();
        frame_.fullReference = {};
        if (renderedDocument_ != document_->state().id) {
            renderedDocument_ = document_->state().id;
            preview_ = QImage();
            frame_ = {};
            nativePresentation_ = false;
            setAttribute(Qt::WA_PaintOnScreen, false);
            setAttribute(Qt::WA_OpaquePaintEvent, false);
            fit();
        }
        scheduleRender();
    });
    connect(document_, &Document::selectionChanged, this, qOverload<>(&Canvas::update));
    connect(document_, &Document::visualChanged, this, [this] {
        ++contentRevision_;
        frame_.tiles.clear();
        frame_.fullReference = {};
        scheduleRender();
    });
    connect(&watcher_, &QFutureWatcher<CanvasPreview>::finished, this, [this] {
        try {
            if (runningGeneration_ == generation_) {
                frame_ = watcher_.result();
                preview_ = frame_.overview;
                const bool native = bool(frame_.overviewTexture) && !gpuPresentationFailed_;
                if (native != nativePresentation_) {
                    nativePresentation_ = native;
                    if (native) setAttribute(Qt::WA_NativeWindow);
                    setAttribute(Qt::WA_PaintOnScreen, native);
                    setAttribute(Qt::WA_OpaquePaintEvent, native);
                }
            }
        } catch (const Error &e) {
            emit failed(e.key, e.detail);
        } catch (...) {
            emit failed("tooLarge", {});
        }
        update();
        if (runningGeneration_ != generation_)
            renderTimer_.start();
    });
}
Canvas::~Canvas() {
    if (renderCancelled_) renderCancelled_->store(true);
    watcher_.waitForFinished();
}
void Canvas::scheduleRender() {
    ++generation_;
    if (renderCancelled_) renderCancelled_->store(true);
    if (!watcher_.isRunning())
        renderTimer_.start();
    update();
}
void Canvas::startRender() {
    if (!document_->hasDocument() || watcher_.isRunning())
        return;
    const State state = document_->state();
    QSize target = state.size;
    const double scale = std::min(1., std::max(.05, zoom_ * devicePixelRatioF()));
    target = QSize(qMax(1, qRound(target.width() * scale)), qMax(1, qRound(target.height() * scale)));
    if (target.width() > 2560 || target.height() > 2560)
        target.scale(2560, 2560, Qt::KeepAspectRatio);
    runningGeneration_ = generation_;
    const QRect visible = QRectF(documentPoint(QPointF()), documentPoint(QPointF(width(), height()))).toAlignedRect();
    const bool detailed = zoom_ * devicePixelRatioF() >= 1 && target != state.size;
    auto previous = frame_;
    if(!previous.unproofedOverview.isNull()) {
        previous.overview=previous.unproofedOverview;previous.tiles=previous.unproofedTiles;
        previous.unproofedOverview={};previous.unproofedTiles.clear();
    }
    const bool reference = referencePreview_ || cmykProof_ || cmykGamut_;
    const QByteArray profile = cmykProof_ || cmykGamut_ ? cmykProfile_ : QByteArray();
    const bool gamut = cmykGamut_;
    const auto revision = contentRevision_;
    renderCancelled_ = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = renderCancelled_;
    const bool preferGpu = QGuiApplication::platformName() == "windows" && !gpuPresentationFailed_ &&
                           !qEnvironmentVariableIntValue("COMPOSITOR_DISABLE_GPU");
    watcher_.setFuture(QtConcurrent::run([state, target, visible, detailed, revision, previous, cancelled, preferGpu, reference, profile, gamut] {
        auto frame = renderCanvasPreview(state, visible, target, detailed, revision, previous, cancelled, preferGpu, reference);
        if (!profile.isEmpty()) {
            frame.unproofedOverview=frame.overview;frame.unproofedTiles=frame.tiles;
            frame.overview = cmykProof(frame.overview,profile,gamut);
            for(auto &tile : frame.tiles) tile.image = cmykProof(tile.image,profile,gamut);
        }
        return frame;
    }));
}
void Canvas::setCmykProof(bool enabled, bool gamut) {
    const auto profile = chosenCmykProfile();
    if(enabled || gamut)validateCmykProfile(profile);
    cmykProfile_ = profile; cmykProof_ = enabled; cmykGamut_ = gamut;
    ++contentRevision_; frame_ = {}; scheduleRender();
}
void Canvas::setReferencePreview(bool enabled) {
    if (referencePreview_ == enabled) return;
    referencePreview_ = enabled;
    ++contentRevision_;
    frame_.fullReference = {};
    scheduleRender();
}
bool Canvas::isBrush() const {
    return tool_ == Tool::Brush || tool_ == Tool::Eraser || tool_ == Tool::Clone || tool_ == Tool::Blur ||
           tool_ == Tool::Healing;
}
void Canvas::setTool(Tool tool) {
    finishInlineText(true);
    cancelGesture();
    tool_ = tool;
    if (tool == Tool::Crop && document_->state().selectionActive && !document_->state().selection.isEmpty())
        draftRect_ = document_->state().selection.boundingRect();
    setCursor(tool == Tool::Hand   ? Qt::OpenHandCursor
              : tool == Tool::Text ? Qt::IBeamCursor
              : tool == Tool::Move ? Qt::ArrowCursor
                                   : Qt::CrossCursor);
    if (tool == Tool::Zoom) setZoomCursor(false);
    update();
}
QPointF Canvas::documentPoint(QPointF view) const {
    return (view - offset_) / zoom_;
}
QPointF Canvas::viewPoint(QPointF document) const {
    return offset_ + document * zoom_;
}
void Canvas::fit() {
    if (!document_->hasDocument())
        return;
    const auto size = document_->state().size;
    zoom_ = std::clamp(std::min(double(width() - 96) / size.width(), double(height() - 96) / size.height()), .02, 16.);
    offset_ = {(width() - size.width() * zoom_) / 2, (height() - size.height() * zoom_) / 2};
    emit zoomChanged(zoom_);
    scheduleRender();
}
void Canvas::setZoom(double value) {
    const QPointF center(width() / 2., height() / 2.);
    const QPointF position = documentPoint(center);
    zoom_ = std::clamp(value, .02, 32.);
    offset_ = center - position * zoom_;
    emit zoomChanged(zoom_);
    scheduleRender();
}
QPolygonF Canvas::selectedPolygon() const {
    const int i = document_->state().index(document_->state().active);
    if (i < 0)
        return {};
    const auto &l = document_->state().layers[i];
    const QSize source = l.image.isNull() ? l.placement.size.toSize() : l.image.size();
    return l.placement.matrix(source).map(QPolygonF(QRectF(QPointF(0, 0), source)));
}
QRectF Canvas::navigatorRect() const {
    if (!document_->hasDocument() || zoom_ < 3)
        return {};
    const auto size = QSizeF(document_->state().size).scaled(180, 130, Qt::KeepAspectRatio);
    return QRectF(QPointF(width() - size.width() - 16, height() - size.height() - 16), size);
}
void Canvas::navigateTo(QPointF point) {
    const auto box = navigatorRect();
    if (box.isEmpty())
        return;
    const auto size = document_->state().size;
    const QPointF target(std::clamp((point.x() - box.left()) / box.width(), 0., 1.) * size.width(),
                         std::clamp((point.y() - box.top()) / box.height(), 0., 1.) * size.height());
    offset_ = QPointF(width() / 2., height() / 2.) - target * zoom_;
    scheduleRender();
}
void Canvas::snapTargets(const State &state, QVector<double> &xs, QVector<double> &ys,
                         const QVector<int> &excluded) const {
    if (snapBounds_) {
        xs << 0. << state.size.width() / 2. << double(state.size.width());
        ys << 0. << state.size.height() / 2. << double(state.size.height());
    }
    if (snapGuides_)
        for (const auto &value : state.guides) {
            const auto guide = value.toObject();
            (guide["axis"] == "horizontal" ? ys : xs).append(guide["position"].toDouble());
        }
    if (snapLayers_)
        for (int j = 0; j < state.layers.size(); ++j) {
            const auto &layer = state.layers[j];
            if (excluded.contains(j) || layer.group || !layer.visible || layer.image.isNull()) continue;
            const auto box = layer.placement.matrix(layer.image.size())
                                 .mapRect(QRectF(QPointF(), layer.image.size()));
            xs << box.left() << box.center().x() << box.right();
            ys << box.top() << box.center().y() << box.bottom();
        }
}
QPaintEngine *Canvas::paintEngine() const {
    return nativePresentation_ ? nullptr : QWidget::paintEngine();
}
void Canvas::paintEvent(QPaintEvent *) {
    const bool native = nativePresentation_ && frame_.overviewTexture && document_->hasDocument();
    const double dpr = devicePixelRatioF();
    const QSize physical(qMax(1, qRound(width()*dpr)), qMax(1, qRound(height()*dpr)));
    QImage underlay, overlay;
    if (native) {
        overlay = QImage(physical, QImage::Format_ARGB32_Premultiplied);
        overlay.setDevicePixelRatio(dpr);
        overlay.fill(Qt::transparent);
    }
    QPainter p(native ? static_cast<QPaintDevice *>(&overlay) : static_cast<QPaintDevice *>(this));
    p.fillRect(rect(), fullscreenCanvas_ ? QColor(Qt::black) : QColor("#1c1c1c"));
    if (!document_->hasDocument())
        return;
    const QSize size = document_->state().size;
    const QRectF canvas(offset_, QSizeF(size) * zoom_);
    p.fillRect(canvas.adjusted(-2, -2, 3, 3), QColor(0, 0, 0, 70));
    QPixmap checker(16, 16);
    checker.fill(QColor("#d7d7d5"));
    {
        QPainter tile(&checker);
        tile.fillRect(0, 0, 8, 8, QColor("#eeeeec"));
        tile.fillRect(8, 8, 8, 8, QColor("#eeeeec"));
    }
    p.save();
    p.setClipRect(canvas);
    p.fillRect(canvas, QBrush(checker));
    p.setRenderHint(QPainter::SmoothPixmapTransform, zoom_ < 1);
    if (!native) {
        p.drawImage(canvas, preview_);
        for (const auto &tile : frame_.tiles)
            p.drawImage(QRectF(viewPoint(tile.rectangle.topLeft()), QSizeF(tile.rectangle.size()) * zoom_), tile.image);
        p.restore();
    } else {
        p.restore();
        const auto nav = navigatorRect();
        if (!nav.isEmpty()) p.fillRect(nav, QBrush(checker));
        p.end();
        underlay = std::move(overlay);
        overlay = QImage(physical, QImage::Format_ARGB32_Premultiplied);
        overlay.setDevicePixelRatio(dpr); overlay.fill(Qt::transparent);
        p.begin(&overlay);
    }
    p.save();
    p.translate(offset_);
    p.scale(zoom_, zoom_);
    QPen guide(QColor("#7bd6ce"), 1 / zoom_);
    guide.setStyle(Qt::DashLine);
    p.setPen(guide);
    if (guides_)
        for (const auto &v : document_->state().guides) {
            const auto g = v.toObject();
            const double position = g["position"].toDouble();
            if (g["axis"] == "horizontal")
                p.drawLine(QPointF(0, position), QPointF(size.width(), position));
            else
                p.drawLine(QPointF(position, 0), QPointF(position, size.height()));
        }
    if (tool_ == Tool::Move && transformControls_) {
        p.setPen(QPen(QColor("#80afff"), 1.2 / zoom_));
        p.setBrush(Qt::NoBrush);
        const auto polygon = selectedPolygon();
        p.drawPolygon(polygon);
        const int i = document_->state().index(document_->state().active);
        if (!polygon.isEmpty() && i >= 0 && !document_->state().layers[i].group) {
            p.setBrush(QColor("#80afff"));
            const double r = 3 / zoom_;
            for (int k = 0; k < 4; ++k)
                p.drawRect(QRectF(polygon[k] - QPointF(r, r), QSizeF(2 * r, 2 * r)));
            const QPointF mid = (polygon[0] + polygon[1]) / 2;
            const QPointF center = (polygon[0] + polygon[2]) / 2;
            QLineF normal(center, mid);
            normal.setLength(normal.length() + 22 / zoom_);
            p.drawLine(mid, normal.p2());
            p.drawEllipse(normal.p2(), 4 / zoom_, 4 / zoom_);
        }
    }
    if ((dragging_ || (tool_ == Tool::Crop && !draftRect_.isEmpty())) &&
        (tool_ == Tool::Rectangle || tool_ == Tool::Ellipse || tool_ == Tool::Line || tool_ == Tool::Crop)) {
        p.setPen(QPen(Qt::white, 1 / zoom_, Qt::DashLine));
        QColor fill = color_;
        fill.setAlpha(90);
        p.setBrush(tool_ == Tool::Crop ? QColor(255, 255, 255, 25) : fill);
        if (tool_ == Tool::Ellipse)
            p.drawEllipse(draftRect_);
        else if (tool_ == Tool::Line) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(color_, lineWidth_, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(anchor_, last_);
        } else
            p.drawRect(draftRect_);
    }
    if (grid_) {
        p.setPen(QPen(QColor(255, 255, 255, 45), 1 / zoom_));
        for (int x = 0; x < size.width(); x += gridSpacing_)
            p.drawLine(QPointF(x, 0), QPointF(x, size.height()));
        for (int y = 0; y < size.height(); y += gridSpacing_)
            p.drawLine(QPointF(0, y), QPointF(size.width(), y));
    }
    if (pixelGrid_ && zoom_ >= 8) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(150, 150, 150, 95), 1 / zoom_));
        const QRectF visible(documentPoint(QPointF(0, 0)), documentPoint(QPointF(width(), height())));
        for (int x = std::max(0, int(std::floor(visible.left()))); x <= std::min(size.width(), int(std::ceil(visible.right()))); ++x)
            p.drawLine(QPointF(x, 0), QPointF(x, size.height()));
        for (int y = std::max(0, int(std::floor(visible.top()))); y <= std::min(size.height(), int(std::ceil(visible.bottom()))); ++y)
            p.drawLine(QPointF(0, y), QPointF(size.width(), y));
    }
    if (document_->state().selectionActive || !selectionDraft_.isEmpty()) {
        p.setBrush(Qt::NoBrush);
        const auto path = selectionDraft_.isEmpty() ? document_->state().selection : selectionDraft_;
        p.setPen(QPen(Qt::black, 2 / zoom_));
        p.drawPath(path);
        p.setPen(QPen(Qt::white, 1 / zoom_, Qt::DashLine));
        p.drawPath(path);
    }
    if (tool_ == Tool::Healing && !healingPath_.isEmpty()) {
        p.fillPath(healingPath_, QColor(130, 170, 230, 100));
    }
    if (dragging_ && tool_ == Tool::Gradient) {
        p.setPen(QPen(Qt::white, 1.5 / zoom_));
        p.drawLine(anchor_, last_);
        p.setBrush(Qt::white);
        p.drawEllipse(anchor_, 3 / zoom_, 3 / zoom_);
        p.drawEllipse(last_, 3 / zoom_, 3 / zoom_);
    }
    if (guideDrag_) {
        p.setPen(QPen(QColor("#80c9e8"), 1 / zoom_));
        if (guideHorizontal_)
            p.drawLine(QPointF(0, last_.y()), QPointF(size.width(), last_.y()));
        else
            p.drawLine(QPointF(last_.x(), 0), QPointF(last_.x(), size.height()));
    }
    p.restore();
    if (rulers_) {
        p.fillRect(0, 0, width(), 22, QColor("#292929"));
        p.fillRect(0, 0, 22, height(), QColor("#292929"));
        QFont font = p.font();
        font.setPixelSize(9);
        p.setFont(font);
        p.setPen(QColor("#999999"));
        double step = 10;
        while (step * zoom_ < 65)
            step *= 2;
        for (double x = std::floor(documentPoint(QPointF(22, 22)).x() / step) * step;
             x < documentPoint(QPointF(width(), 0)).x(); x += step) {
            double v = viewPoint(QPointF(x, 0)).x();
            p.drawLine(QPointF(v, 17), QPointF(v, 22));
            p.drawText(QPointF(v + 3, 12), QString::number(x));
        }
        for (double y = std::floor(documentPoint(QPointF(22, 22)).y() / step) * step;
             y < documentPoint(QPointF(0, height())).y(); y += step) {
            double v = viewPoint(QPointF(0, y)).y();
            p.drawLine(QPointF(17, v), QPointF(22, v));
            p.save();
            p.translate(12, v - 3);
            p.rotate(-90);
            p.drawText(0, 0, QString::number(y));
            p.restore();
        }
    }
    if (isBrush() && underMouse()) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(Qt::white, 1));
        p.drawEllipse(mouse_, brushSize_ * zoom_ / 2, brushSize_ * zoom_ / 2);
        p.setPen(QPen(Qt::black, 1, Qt::DotLine));
        p.drawEllipse(mouse_, brushSize_ * zoom_ / 2 + 1, brushSize_ * zoom_ / 2 + 1);
    }
    const auto nav = navigatorRect();
    if (!nav.isEmpty()) {
        p.fillRect(nav.adjusted(-5, -5, 5, 5), QColor(20, 20, 20, 235));
        if (native) {
            p.save();p.setCompositionMode(QPainter::CompositionMode_Source);
            p.fillRect(nav, Qt::transparent);p.restore();
        } else {
            p.fillRect(nav, QBrush(checker));
            p.drawImage(nav, preview_);
        }
        const QRectF visible(documentPoint(QPointF(0, 0)), documentPoint(QPointF(width(), height())));
        const QRectF clipped = visible.intersected(QRectF(QPointF(), size));
        QRectF viewport(nav.x() + clipped.x() / size.width() * nav.width(),
                        nav.y() + clipped.y() / size.height() * nav.height(),
                        clipped.width() / size.width() * nav.width(), clipped.height() / size.height() * nav.height());
        p.setBrush(QColor(255, 255, 255, 30));
        p.setPen(QPen(Qt::white, 1.5));
        p.drawRect(viewport);
    }
    if (native) {
        p.end();
#ifdef Q_OS_WIN
        if (!gpuPresenter_) gpuPresenter_ = std::make_unique<GpuCanvasPresenter>();
        QVector<GpuCanvasSprite> sprites;
        const auto append = [&](const std::shared_ptr<GpuPreviewTexture> &texture, QRectF destination, QRectF source = {}) {
            if (!texture || !texture->native) return;
            sprites.append({texture->native->shaderResourceView.Get(), texture->size, source,
                            QRectF(destination.topLeft()*dpr, destination.size()*dpr), zoom_ < 1});
        };
        append(frame_.overviewTexture, canvas);
        for (const auto &tile : frame_.tiles)
            append(tile.texture, QRectF(viewPoint(tile.rectangle.topLeft()),QSizeF(tile.rectangle.size())*zoom_),
                   tile.textureSource);
        if (!nav.isEmpty()) append(frame_.overviewTexture, nav);
        bool presented = false;
        runGpuNative([&](ID3D11Device *device, ID3D11DeviceContext *context, quint64) {
            presented = gpuPresenter_->present(device,context,reinterpret_cast<HWND>(winId()),physical,
                                                  underlay,overlay,sprites);
            const auto info = gpuPresenter_->lastPresent();
            GpuNativeResult result;
            result.uploadedBytes = info.underlayUploadBytes + info.overlayUploadBytes;
            return result;
        });
        if (presented) return;
#endif
        gpuPresentationFailed_ = true;
        nativePresentation_ = false;
        setAttribute(Qt::WA_PaintOnScreen,false);setAttribute(Qt::WA_OpaquePaintEvent,false);
        if (gpuPresenter_) gpuPresenter_->release();
        frame_ = {}; ++contentRevision_;
        scheduleRender();
    }
}
void Canvas::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    if (e->oldSize().isValid())
        offset_ += QPointF((width() - e->oldSize().width()) / 2., (height() - e->oldSize().height()) / 2.);
    scheduleRender();
}
void Canvas::setZoomCursor(bool out) {
    QPixmap pixmap(32, 32);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(Qt::black, 3));
    painter.setBrush(Qt::white);
    painter.drawEllipse(QRectF(3, 3, 18, 18));
    painter.drawLine(QPointF(20, 20), QPointF(28, 28));
    painter.setPen(QPen(Qt::black, 2));
    painter.drawLine(7, 12, 17, 12);
    if (!out) painter.drawLine(12, 7, 12, 17);
    painter.end();
    setCursor(QCursor(pixmap, 12, 12));
    setProperty("zoomCursorOut", out);
}
void Canvas::mousePressEvent(QMouseEvent *e) {
    if (!document_->hasDocument())
        return;
    setFocus();
    mouse_ = e->position();
    if (e->button() == Qt::LeftButton && navigatorRect().contains(mouse_)) {
        navigatorDrag_ = true;
        navigateTo(mouse_);
        return;
    }
    if (e->button() == Qt::MiddleButton && e->modifiers().testFlag(Qt::ControlModifier)) {
        middleZoom_ = true;
        toolZoom_ = zoomMoved_ = false;
        setZoomCursor(false);
        last_ = mouse_;
        return;
    }
    if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && (space_ || tool_ == Tool::Hand))) {
        panning_ = true;
        last_ = e->position();
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    if (!lockGuides_ && tool_ == Tool::Move && guides_ &&
        e->position().x() >= 22 && e->position().y() >= 22) {
        const auto &lines = document_->state().guides;
        for (int index = lines.size() - 1; index >= 0; --index) {
            const auto guide = lines[index].toObject();
            const bool horizontal = guide["axis"] == "horizontal";
            const double distance = horizontal ? std::abs(documentPoint(e->position()).y() - guide["position"].toDouble())
                                               : std::abs(documentPoint(e->position()).x() - guide["position"].toDouble());
            if (distance <= 5. / zoom_) {
                guideDrag_ = true; movingGuide_ = index; guideHorizontal_ = horizontal;
                last_ = documentPoint(e->position()); return;
            }
        }
    }
    if (rulers_ && (e->position().x() < 22 || e->position().y() < 22)) {
        guideDrag_ = true;
        guideHorizontal_ = e->position().y() < 22;
        last_ = documentPoint(e->position());
        return;
    }
    anchor_ = documentPoint(e->position());
    last_ = anchor_;
    if (tool_ == Tool::Text) {
        const auto &state = document_->state();
        int index = state.index(state.active);
        if (index >= 0 && state.layers[index].metadata["text"].isObject()) {
            beginInlineText(state.active);
            return;
        }
        emit textRequested(anchor_);
        return;
    }
    if (tool_ == Tool::Eyedropper) {
        const QPoint point(qFloor(anchor_.x()), qFloor(anchor_.y()));
        if (QRect(QPoint(), document_->state().size).contains(point)) {
            try {
                const auto pixel = renderRegion(document_->state(), QRect(point, QSize(1, 1)));
                color_ = pixel.pixelColor(0, 0);
                color_.setAlpha(255);
                emit colorPicked(color_);
            } catch (const Error &error) { emit failed(error.key, error.detail); }
        }
        return;
    }
    if (tool_ == Tool::Zoom) {
        middleZoom_ = toolZoom_ = true;
        zoomMoved_ = false;
        last_ = e->position();
        setZoomCursor(e->modifiers().testFlag(Qt::AltModifier));
        return;
    }
    gestureSelectionMode_ = e->modifiers().testFlag(Qt::ShiftModifier) ? 1
                            : e->modifiers().testFlag(Qt::AltModifier) ? 2
                                                                       : selectionMode_;
    if (tool_ == Tool::Wand && objectMode_) {
        emit objectRequested(anchor_.toPoint());
        return;
    }
    if (tool_ == Tool::Wand) {
        try {
            document_->setSelection(
                wandPath(comp::render(document_->state()), anchor_.toPoint(), tolerance_, contiguous_),
                gestureSelectionMode_);
        } catch (const Error &error) {
            emit failed(error.key, error.detail);
        }
        return;
    }
    if (tool_ == Tool::Clone && e->modifiers().testFlag(Qt::AltModifier)) {
        cloneSource_ = anchor_;
        cloneReady_ = true;
        cloneAnchorChanged_ = true;
        return;
    }
    if (tool_ == Tool::Lasso && polygonal_) {
        if (polygonPoints_.size() > 2 && QLineF(viewPoint(polygonPoints_.first()), e->position()).length() < 9) {
            QPainterPath path;
            path.addPolygon(polygonPoints_);
            path.closeSubpath();
            document_->setSelection(path, gestureSelectionMode_);
            polygonPoints_.clear();
            selectionDraft_ = {};
        } else {
            polygonPoints_.append(anchor_);
            selectionDraft_ = {};
            selectionDraft_.addPolygon(polygonPoints_);
        }
        update();
        return;
    }
    if (tool_ == Tool::Marquee || tool_ == Tool::Lasso) {
        selectionDraft_ = {};
        selectionStart_ = document_->state().selection;
        selectionDrag_ =
            gestureSelectionMode_ == 0 && document_->state().selectionActive && selectionStart_.contains(anchor_);
        if (!selectionDrag_ && tool_ == Tool::Lasso)
            selectionDraft_.moveTo(anchor_);
    }
    int i = document_->state().index(document_->state().active);
    if (tool_ == Tool::Move) {
        auto polygon = transformControls_ ? selectedPolygon() : QPolygonF();
        bool onHandle = false;
        rotating_ = false;
        resizeHandle_ = 2;
        if (!polygon.isEmpty()) {
            for (int k = 0; k < 4; ++k)
                if (QLineF(viewPoint(polygon[k]), e->position()).length() < 10) {
                    onHandle = true;
                    resizeHandle_ = k;
                    break;
                }
            QLineF normal((polygon[0] + polygon[2]) / 2, (polygon[0] + polygon[1]) / 2);
            normal.setLength(normal.length() + 22 / zoom_);
            rotating_ = QLineF(viewPoint(normal.p2()), e->position()).length() < 10;
            onHandle = onHandle || rotating_;
        }
        if (autoSelect_ && !onHandle) {
            auto order = document_->state().drawOrder();
            std::reverse(order.begin(), order.end());
            for (int candidate : order) {
                const auto &l = document_->state().layers[candidate];
                if (l.group || !l.visible || l.image.isNull())
                    continue;
                const QPoint local = l.placement.matrix(l.image.size()).inverted().map(anchor_).toPoint();
                if (l.image.rect().contains(local) && qAlpha(l.image.pixel(local)) > 10) {
                    document_->select(l.id);
                    i = candidate;
                    break;
                }
            }
            polygon = selectedPolygon();
        }
        if (i < 0)
            return;
        resizing_ = !document_->state().layers[i].group && onHandle && !rotating_;
        start_ = document_->state();
        movingIndices_.clear();
        auto ids = selectedLayers_;
        if (!ids.contains(start_.active))
            ids = {start_.active};
        for (const auto &id : ids) {
            int j = start_.index(id);
            if (j < 0)
                continue;
            if (!movingIndices_.contains(j))
                movingIndices_.append(j);
            for (int child : start_.descendants(id))
                if (!movingIndices_.contains(child))
                    movingIndices_.append(child);
        }
        document_->begin();
    } else if (isBrush()) {
        if (i < 0 || document_->state().layers[i].group ||
            document_->state().layers[i].metadata["adjustment"].isObject())
            return;
        if (tool_ == Tool::Clone && !cloneReady_) {
            emit failed("cloneNeedsSource", {});
            return;
        }
        if (tool_ == Tool::Clone || tool_ == Tool::Blur) {
            retouchSource_ = document_->state().layers[i].image;
            if (tool_ == Tool::Blur && retouchMode_ == 0)
                retouchSource_ = filteredAccelerated(retouchSource_, {{"kind", "Gaussian Blur"}, {"blurRadius", blurRadius_}});
            else if (tool_ == Tool::Clone) {
                if (cloneSampleAll_) {
                    QImage merged = comp::render(document_->state());
                    QImage sampled(retouchSource_.size(), pixelFormat(merged));
                    if (sampled.isNull()) throw Error("tooLarge");
                    sampled.fill(Qt::transparent);
                    QPainter painter(&sampled);
                    painter.setTransform(document_->state().layers[i].placement.matrix(sampled.size()).inverted());
                    painter.drawImage(0, 0, merged);
                    painter.end();
                    retouchSource_ = sampled;
                }
                if (cloneAnchorChanged_ || !cloneAligned_)
                    cloneOffset_ = cloneSource_ - anchor_;
                cloneAnchorChanged_ = false;
            }
        }
        healingPath_ = {};
        retouchStroke_.reset();
        document_->begin();
        try {
            paintStroke(anchor_, anchor_);
        } catch (const Error &error) {
            retouchStroke_.reset();
            document_->cancel();
            emit failed(error.key, error.detail);
            return;
        }
    }
    dragging_ = true;
    draftRect_ = QRectF(anchor_, anchor_);
    update();
}
void Canvas::paintStroke(QPointF from, QPointF to) {
    auto &s = document_->draft();
    const int i = s.index(s.active);
    if (i < 0)
        return;
    auto &l = s.layers[i];
    QImage *target = maskTarget_ ? &l.mask : &l.image;
    if (target->isNull()) {
        const QSize size = maskTarget_ && !l.image.isNull() ? l.image.size() : s.size;
        if (!maskTarget_ && s.sourcePixels() + qint64(size.width()) * size.height() > MaxSourcePixels)
            throw Error("tooLarge");
        *target = QImage(size, maskTarget_ ? maskFormat(s.bitDepth) : pixelFormat(s.bitDepth));
        if (target->isNull())
            throw Error("tooLarge");
        target->fill(maskTarget_ ? Qt::white : Qt::transparent);
        if (!maskTarget_) {
            l.placement = {};
            l.placement.size = s.size;
        } else {
            l.maskPlacement = l.placement;
        }
    }
    if (maskTarget_ && target->size() == QSize(1, 1)) {
        const QSize size = l.image.isNull() ? s.size : l.image.size();
        qint64 maskPixels = 0;
        for (const auto &layer : s.layers)
            maskPixels += qint64(layer.mask.width()) * layer.mask.height();
        if (maskPixels - 1 + qint64(size.width()) * size.height() > MaxSourcePixels)
            throw Error("tooLarge");
        const QColor coverage = target->pixelColor(0, 0);
        QImage expanded(size, maskFormat(s.bitDepth));
        if (expanded.isNull())
            throw Error("tooLarge");
        expanded.fill(coverage);
        *target = expanded;
    }
    if (tool_ == Tool::Healing) {
        QPainterPath segment;
        segment.moveTo(from);
        segment.lineTo(to);
        QPainterPathStroker stroke;
        stroke.setWidth(brushSize_);
        stroke.setCapStyle(Qt::RoundCap);
        healingPath_ = healingPath_.united(stroke.createStroke(segment));
        healingPath_.addEllipse(to, brushSize_ / 2., brushSize_ / 2.);
        return;
    }
    if (maskTarget_) {
        l.originalMaskPng.clear();
        if (target->format() != pixelFormat(s.bitDepth))
            *target = target->convertToFormat(pixelFormat(s.bitDepth));
    } else {
        l.originalPng.clear();
        l.metadata.remove("text");
        l.metadata.remove("shape");
    }
    const Placement placement = maskTarget_ && !l.maskLinked ? l.maskPlacement : l.placement;
    if (tool_ == Tool::Blur && retouchMode_ > 0) {
        auto inverse = placement.matrix(target->size()).inverted();
        QPointF a = inverse.map(from), b = inverse.map(to);
        double scale = target->width() / placement.size.width();
        const double spacing = std::max(1., brushSize_ * scale * (retouchMode_ == 1 ? .005 : .025));
        int steps = std::max(1, int(QLineF(a, b).length() / spacing));
        if (!retouchStroke_) {
            const auto coverage = s.selectionActive ? grayFromAlpha(selectionCoverage(s, target->size(), placement), s.bitDepth) : QImage();
            retouchStroke_ = std::make_unique<RetouchStroke>(*target, brushSize_ * scale / 2, hardness_, flow_, retouchMode_ == 1, coverage);
        }
        QVector<QPointF> points;
        points.reserve(steps + 1);
        points.append(a);
        for (int k = 1; k <= steps; ++k) points.append(a + (b - a) * (double(k) / steps));
        retouchStroke_->append(*target, points);
        document_->preview();
        return;
    }
    const QImage beforeDab = *target;
    QPainter painter(target);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setTransform(placement.matrix(target->size()).inverted());
    QColor ink = color_;
    ink.setAlphaF(flow_);
    if (maskTarget_) {
        ink = tool_ == Tool::Eraser ? QColor(Qt::black)
              : s.bitDepth == 16    ? grayColor16(color_)
                                    : QColor(qGray(color_.rgb()), qGray(color_.rgb()), qGray(color_.rgb()));
        ink.setAlphaF(flow_);
    } else if (tool_ == Tool::Eraser)
        painter.setCompositionMode(QPainter::CompositionMode_DestinationOut);
    if (s.selectionActive && s.selectionMask.isNull())
        painter.setClipPath(s.selection);
    if (tool_ == Tool::Clone || tool_ == Tool::Blur) {
        const double distance = QLineF(from, to).length();
        const int steps = std::max(1, int(distance / std::max(1., brushSize_ * .15)));
        const auto matrix = placement.matrix(target->size());
        for (int step = 0; step <= steps; ++step) {
            const auto point = from + (to - from) * (double(step) / steps);
            QPainterPath tip;
            tip.addEllipse(point, brushSize_ / 2., brushSize_ / 2.);
            painter.save();
            painter.setClipPath(tip, Qt::IntersectClip);
            painter.setOpacity(flow_);
            if (tool_ == Tool::Clone)
                painter.translate(-cloneOffset_);
            painter.setTransform(matrix, true);
            painter.drawImage(0, 0, retouchSource_);
            painter.restore();
        }
        painter.end();
        if (!s.selectionMask.isNull())
            constrainSelection(s, placement, beforeDab, *target);
        document_->preview();
        return;
    }
    if (hardness_ < .99) {
        const double distance = QLineF(from, to).length();
        const int steps = std::max(1, int(distance / std::max(1., brushSize_ * .12)));
        for (int k = 0; k <= steps; ++k) {
            const auto point = from + (to - from) * (double(k) / steps);
            QRadialGradient gradient(point, brushSize_ / 2.);
            gradient.setColorAt(0, ink);
            gradient.setColorAt(std::min(.99, hardness_), ink);
            auto clear = ink;
            clear.setAlpha(0);
            gradient.setColorAt(1, clear);
            painter.setPen(Qt::NoPen);
            painter.setBrush(gradient);
            painter.drawEllipse(point, brushSize_ / 2., brushSize_ / 2.);
        }
        painter.end();
        if (!s.selectionMask.isNull())
            constrainSelection(s, placement, beforeDab, *target);
        document_->preview();
        return;
    }
    painter.setPen(QPen(ink, brushSize_, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawLine(from, to);
    if (from == to) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink);
        painter.drawEllipse(from, brushSize_ / 2., brushSize_ / 2.);
    }
    painter.end();
    if (!s.selectionMask.isNull())
        constrainSelection(s, placement, beforeDab, *target);
    document_->preview();
}
void Canvas::mouseMoveEvent(QMouseEvent *e) {
    mouse_ = e->position();
    if (navigatorDrag_) {
        navigateTo(mouse_);
        return;
    }
    if (middleZoom_) {
        const double delta = mouse_.x() - last_.x() - mouse_.y() + last_.y();
        if (!zoomMoved_ && std::abs(delta) < 3) return;
        if (delta != 0) { zoomMoved_ = true; setZoomCursor(delta < 0); }
        const auto point = documentPoint(last_);
        zoom_ = std::clamp(zoom_ * std::exp(delta * .01), .02, 32.);
        offset_ = last_ - point * zoom_;
        last_ = mouse_;
        emit zoomChanged(zoom_);
        scheduleRender();
        return;
    }
    if (guideDrag_) {
        last_ = documentPoint(mouse_);
        update();
        return;
    }
    if (panning_) {
        offset_ += e->position() - last_;
        last_ = e->position();
        scheduleRender();
        return;
    }
    if (!dragging_) {
        update();
        return;
    }
    QPointF current = documentPoint(e->position());
    try {
        if (tool_ == Tool::Move) {
            auto &s = document_->draft();
            const int i = s.index(s.active);
            if (i < 0 || i >= start_.layers.size())
                return;
            const auto &original = start_.layers[i];
            if (rotating_) {
                const auto center = original.placement.origin +
                                    QPointF(original.placement.size.width() / 2, original.placement.size.height() / 2);
                const double initial = std::atan2(anchor_.y() - center.y(), anchor_.x() - center.x());
                double angle = original.placement.rotation +
                               (std::atan2(current.y() - center.y(), current.x() - center.x()) - initial) * 180 /
                                   3.141592653589793;
                if (e->modifiers().testFlag(Qt::ShiftModifier))
                    angle = qRound(angle / 15) * 15;
                s.layers[i].placement.rotation = angle;
            } else if (resizing_) {
                const auto polygon = original.placement.matrix(original.image.size())
                                         .map(QPolygonF(QRectF(QPointF(), original.image.size())));
                const auto opposite = polygon[(resizeHandle_ + 2) % 4];
                QTransform rotation;
                rotation.rotate(-original.placement.rotation);
                QPointF delta = rotation.map(current - opposite);
                double w = std::clamp(abs(delta.x()), 1., 300000.), h = std::clamp(abs(delta.y()), 1., 300000.);
                if (e->modifiers().testFlag(Qt::ShiftModifier))
                    h = w * original.placement.size.height() / original.placement.size.width();
                QTransform forward;
                forward.rotate(original.placement.rotation);
                const QPointF center =
                    opposite + forward.map(QPointF(std::copysign(w / 2, delta.x()), std::copysign(h / 2, delta.y())));
                s.layers[i].placement.size = {w, h};
                s.layers[i].placement.origin = center - QPointF(w / 2, h / 2);
            } else {
                auto indices = movingIndices_;
                QPointF delta = current - anchor_;
                if (snap_ && !e->modifiers().testFlag(Qt::AltModifier)) {
                    auto bounds = original.placement.matrix(original.image.size())
                                      .mapRect(QRectF(QPointF(), original.image.size()));
                    QVector<double> xs, ys;
                    snapTargets(start_, xs, ys, indices);
                    auto gridEdge = [&](double edge, double &best, double &deltaAxis) {
                        if (!snapGrid_ || gridSpacing_ < 1) return;
                        const double where = edge + deltaAxis;
                        const double nearest = std::round(where / gridSpacing_) * gridSpacing_;
                        const double distance = std::abs(nearest - where);
                        if (distance < best) { best = distance; deltaAxis = nearest - edge; }
                    };
                    double dx = 6 / zoom_, dy = 6 / zoom_;
                    for (double edge : {bounds.left(), bounds.center().x(), bounds.right()})
                        gridEdge(edge, dx, delta.rx());
                    for (double edge : {bounds.top(), bounds.center().y(), bounds.bottom()})
                        gridEdge(edge, dy, delta.ry());
                    for (double x : xs)
                        for (double edge : {bounds.left(), bounds.center().x(), bounds.right()})
                            if (abs(x - edge - delta.x()) < dx) {
                                dx = abs(x - edge - delta.x());
                                delta.setX(x - edge);
                            }
                    for (double y : ys)
                        for (double edge : {bounds.top(), bounds.center().y(), bounds.bottom()})
                            if (abs(y - edge - delta.y()) < dy) {
                                dy = abs(y - edge - delta.y());
                                delta.setY(y - edge);
                            }
                }
                for (int j : indices) {
                    s.layers[j].placement.origin = start_.layers[j].placement.origin + delta;
                    if (!s.layers[j].maskLinked)
                        s.layers[j].maskPlacement.origin = start_.layers[j].maskPlacement.origin + delta;
                }
            }
            document_->preview();
        } else if (isBrush())
            paintStroke(last_, current);
        else if (tool_ == Tool::Marquee || tool_ == Tool::Lasso) {
            if (e->modifiers().testFlag(Qt::ShiftModifier) && gestureSelectionMode_ != 1) {
                auto d = current - anchor_;
                double extent = std::max(abs(d.x()), abs(d.y()));
                current = anchor_ + QPointF(std::copysign(extent, d.x()), std::copysign(extent, d.y()));
            }
            if (selectionDrag_) {
                QTransform shift;
                shift.translate(current.x() - anchor_.x(), current.y() - anchor_.y());
                selectionDraft_ = shift.map(selectionStart_);
            } else if (tool_ == Tool::Lasso)
                selectionDraft_.lineTo(current);
            else {
                QRectF box = QRectF(anchor_, current).normalized();
                selectionDraft_ = {};
                if (ellipseSelection_)
                    selectionDraft_.addEllipse(box);
                else
                    selectionDraft_.addRect(box);
            }
        } else if (tool_ == Tool::Gradient) {
            last_ = current;
        }

        else {
            if (e->modifiers().testFlag(Qt::ShiftModifier) && tool_ == Tool::Line) {
                const auto d = current - anchor_;
                const double distance = QLineF(anchor_, current).length();
                const double angle = std::round(std::atan2(d.y(), d.x()) / (3.141592653589793 / 4))
                                     * (3.141592653589793 / 4);
                current = anchor_ + QPointF(std::cos(angle) * distance, std::sin(angle) * distance);
            } else if (e->modifiers().testFlag(Qt::ShiftModifier) && tool_ != Tool::Crop) {
                const auto d = current - anchor_;
                const double length = std::max(std::abs(d.x()), std::abs(d.y()));
                current = anchor_ + QPointF(std::copysign(length, d.x()), std::copysign(length, d.y()));
            }
            if (tool_ == Tool::Crop) {
                if (cropRatio_ > 0) {
                    const auto d = current - anchor_;
                    current = anchor_ + QPointF(d.x(), std::copysign(abs(d.x()) / cropRatio_, d.y()));
                }
                if (snap_ && !e->modifiers().testFlag(Qt::AltModifier)) {
                    QVector<double> xs, ys;
                    snapTargets(document_->state(), xs, ys, {});
                    auto nearest = [&](double position, const QVector<double> &targets) {
                        double best = 6 / zoom_, result = position;
                        for (double target : targets)
                            if (std::abs(target - position) < best) {
                                best = std::abs(target - position);
                                result = target;
                            }
                        if (snapGrid_ && gridSpacing_ > 0) {
                            const double target = std::round(position / gridSpacing_) * gridSpacing_;
                            if (std::abs(target - position) < best) result = target;
                        }
                        return result;
                    };
                    current.rx() = nearest(current.x(), xs);
                    current.ry() = nearest(current.y(), ys);
                    if (cropRatio_ > 0) {
                        const auto d = current - anchor_;
                        current.ry() = anchor_.y() + std::copysign(std::abs(d.x()) / cropRatio_, d.y());
                    }
                }
                if (e->modifiers().testFlag(Qt::AltModifier))
                    draftRect_ = QRectF(anchor_ - (current - anchor_), current).normalized();
                else
                    draftRect_ = QRectF(anchor_, current).normalized();
            } else
                draftRect_ = QRectF(anchor_, current).normalized();
        }
    } catch (const Error &error) {
        cancelGesture();
        emit failed(error.key, error.detail);
    }
    last_ = current;
    update();
}
void Canvas::finishShape() {
    const QRect rect = tool_ == Tool::Line
                           ? QRectF(anchor_, last_).normalized().adjusted(-lineWidth_ / 2. - 1, -lineWidth_ / 2. - 1,
                                                                          lineWidth_ / 2. + 1, lineWidth_ / 2. + 1).toAlignedRect()
                           : draftRect_.toAlignedRect();
    if (rect.width() < 1 || rect.height() < 1)
        return;
    if (rect.width() > MaxSide || rect.height() > MaxSide ||
        qint64(rect.width()) * rect.height() + document_->state().sourcePixels() > MaxSourcePixels)
        throw Error("tooLarge");
    QImage image(rect.size(), pixelFormat(document_->state().bitDepth));
    if (image.isNull())
        throw Error("tooLarge");
    image.fill(Qt::transparent);
    {
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(color_);
        if (tool_ == Tool::Line) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(color_, lineWidth_, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(anchor_ - rect.topLeft(), last_ - rect.topLeft());
        } else if (tool_ == Tool::Ellipse)
            p.drawEllipse(QRectF(QPointF(0, 0), rect.size()));
        else
            p.drawPath(roundedShapePath(QRectF(image.rect()), cornerRadius_, smoothCorners_));
    }
    document_->edit([&](State &s) {
        Layer l;
        l.id = newId();
        l.name = tool_ == Tool::Ellipse ? ellipseName_ : tool_ == Tool::Line ? lineName_ : rectangleName_;
        l.image = image;
        l.placement.origin = rect.topLeft();
        l.placement.size = rect.size();
        const int i = s.index(s.active);
        if (i >= 0)
            l.parent = s.layers[i].group ? s.layers[i].id : s.layers[i].parent;
        QJsonObject shape{{"kind", tool_ == Tool::Ellipse ? "Ellipse" : tool_ == Tool::Line ? "Line" : "Rectangle"},
                          {"red", color_.redF()}, {"green", color_.greenF()}, {"blue", color_.blueF()},
                          {"cornerRadius", cornerRadius_}};
        if (tool_ == Tool::Rectangle && smoothCorners_ && cornerRadius_ > 0) shape["smoothing"] = true;
        if (tool_ == Tool::Line) {
            auto fraction = [&](QPointF point) {
                return QJsonObject{{"x", (point.x() - rect.x()) / rect.width()},
                                   {"y", (point.y() - rect.y()) / rect.height()}};
            };
            shape["lineWidth"] = lineWidth_;
            shape["start"] = fraction(anchor_);
            shape["end"] = fraction(last_);
        }
        l.metadata["shape"] = shape;
        s.layers.append(l);
        s.active = l.id;
    });
}
void Canvas::finishHealing() {
    auto &s = document_->draft();
    const int i = s.index(s.active);
    if (i < 0)
        return;
    auto &l = s.layers[i];
    auto process = [&](const QImage &source) {
    auto image = source.convertToFormat(s.bitDepth == 16 ? QImage::Format_RGBA64_Premultiplied
                                                          : QImage::Format_RGBA8888_Premultiplied);
    QImage mask(image.size(), QImage::Format_Grayscale8);
    mask.fill(Qt::black);
    QPainter p(&mask);
    p.setTransform(l.placement.matrix(image.size()).inverted());
    if (s.selectionActive)
        p.setClipPath(s.selection);
    p.fillPath(healingPath_, Qt::white);
    p.end();
    QByteArray coverage(image.width() * image.height(), 0);
    for (int y = 0; y < image.height(); ++y)
        memcpy(coverage.data() + y * image.width(), mask.constScanLine(y), image.width());
    const auto *coverageBytes = reinterpret_cast<const uint8_t *>(coverage.constData());
    const int healResult =
        s.bitDepth == 16 ? wide::spot_heal(reinterpret_cast<quint16 *>(image.bits()), coverageBytes, image.width(),
                                           image.height(), image.bytesPerLine() / 2, float(flow_), healingMode_, 0)
                         : spot_heal(image.bits(), coverageBytes, image.width(), image.height(), image.bytesPerLine(),
                                     float(flow_), healingMode_, 0);
    if (healResult < 0) throw Error("tooLarge");
    return image.convertToFormat(pixelFormat(s.bitDepth));
    };
    auto healed = hdrImage(l.image) ? hdrThrough(l.image,process) : process(l.image);
    if (!s.selectionMask.isNull())
        constrainSelection(s, l.placement, l.image, healed);
    l.image = healed;
    l.originalPng.clear();
    l.metadata.remove("text");
    l.metadata.remove("shape");
    healingPath_ = {};
}
void Canvas::finishGradient() {
    if (QLineF(anchor_, last_).length() < 1)
        return;
    document_->edit([&](State &s) {
        const int i = s.index(s.active);
        if (i < 0 || s.layers[i].group)
            return;
        auto &l = s.layers[i];
        QImage &target = maskTarget_ ? l.mask : l.image;
        if (target.isNull()) {
            target = QImage(s.size, pixelFormat(s.bitDepth));
            if (target.isNull())
                throw Error("tooLarge");
            target.fill(maskTarget_ ? Qt::white : Qt::transparent);
            if (!maskTarget_) {
                l.placement = {};
                l.placement.size = s.size;
            } else {
                l.maskLinked = false;
                l.maskPlacement = {};
                l.maskPlacement.size = s.size;
            }
        }
        target = target.convertToFormat(hdrImage(target) ? pixelFormat(target) : pixelFormat(s.bitDepth));
        QColor first = color_, second = background_;
        if (transparentGradient_) {
            second = first;
            second.setAlpha(0);
        }
        if (maskTarget_) {
            first = s.bitDepth == 16 ? grayColor16(first)
                                     : QColor(qGray(first.rgb()), qGray(first.rgb()), qGray(first.rgb()));
            second = s.bitDepth == 16
                         ? grayColor16(second)
                         : QColor(qGray(second.rgb()), qGray(second.rgb()), qGray(second.rgb()), second.alpha());
        }
        const QImage before = target;
        QPainter p(&target);
        p.setTransform((maskTarget_ && !l.maskLinked ? l.maskPlacement : l.placement).matrix(target.size()).inverted());
        if (s.selectionActive && s.selectionMask.isNull())
            p.setClipPath(s.selection);
        p.setOpacity(flow_);
        QLinearGradient linear(anchor_, last_);
        QRadialGradient radial(anchor_, QLineF(anchor_, last_).length());
        QGradient *g = radial_ ? static_cast<QGradient *>(&radial) : static_cast<QGradient *>(&linear);
        g->setColorAt(0, first);
        g->setColorAt(1, second);
        p.fillRect(QRectF(QPointF(), s.size), QBrush(*g));
        p.end();
        if (!s.selectionMask.isNull())
            constrainSelection(s, maskTarget_ && !l.maskLinked ? l.maskPlacement : l.placement, before, target);
        if (maskTarget_)
            l.originalMaskPng.clear();
        else {
            l.originalPng.clear();
            l.metadata.remove("text");
            l.metadata.remove("shape");
        }
    });
}
void Canvas::mouseDoubleClickEvent(QMouseEvent *e) {
    if (tool_ == Tool::Lasso && polygonal_ && polygonPoints_.size() > 2) {
        QPainterPath path;
        path.addPolygon(polygonPoints_);
        path.closeSubpath();
        document_->setSelection(path, gestureSelectionMode_);
        polygonPoints_.clear();
        selectionDraft_ = {};
        update();
        e->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(e);
}
void Canvas::mouseReleaseEvent(QMouseEvent *e) {
    if (navigatorDrag_) {
        navigatorDrag_ = false;
        return;
    }
    if (middleZoom_) {
        if (e->button() != (toolZoom_ ? Qt::LeftButton : Qt::MiddleButton)) return;
        middleZoom_ = false;
        if (toolZoom_ && !zoomMoved_)
            setZoom(zoom_ * (e->modifiers().testFlag(Qt::AltModifier) ? .8 : 1.25));
        toolZoom_ = zoomMoved_ = false;
        if (tool_ == Tool::Zoom) setZoomCursor(e->modifiers().testFlag(Qt::AltModifier));
        else setCursor(tool_ == Tool::Hand ? Qt::OpenHandCursor : Qt::CrossCursor);
        return;
    }
    if (guideDrag_) {
        guideDrag_ = false;
        const auto point = documentPoint(e->position());
        try {
            if (movingGuide_ >= 0) {
                const int index = movingGuide_;
                document_->edit([&](State &state) {
                    auto guide = state.guides[index].toObject();
                    guide["position"] = guideHorizontal_ ? point.y() : point.x();
                    state.guides[index] = guide;
                });
            } else
                document_->addGuide(guideHorizontal_, guideHorizontal_ ? point.y() : point.x());
            movingGuide_ = -1;
        } catch (const Error &error) {
            emit failed(error.key, error.detail);
        }
        update();
        return;
    }
    if (panning_) {
        panning_ = false;
        setCursor(tool_ == Tool::Hand ? Qt::OpenHandCursor : Qt::ArrowCursor);
        return;
    }
    if (e->button() != Qt::LeftButton || !dragging_)
        return;
    dragging_ = false;
    retouchStroke_.reset();
    try {
        if (tool_ == Tool::Move) {
            if (documentPoint(e->position()) == anchor_)
                document_->cancel();
            else
                document_->commit();
        } else if (isBrush()) {
            if (tool_ == Tool::Healing)
                finishHealing();
            document_->commit();
        } else if (tool_ == Tool::Marquee || tool_ == Tool::Lasso) {
            if (tool_ == Tool::Lasso)
                selectionDraft_.closeSubpath();
            document_->setSelection(selectionDraft_, selectionDrag_ ? 0 : gestureSelectionMode_);
            selectionDraft_ = {};
            selectionDrag_ = false;
        } else if (tool_ == Tool::Gradient)
            finishGradient();
        else if (tool_ == Tool::Crop)
            document_->crop(draftRect_.toAlignedRect());
        else
            finishShape();
    } catch (const Error &error) {
        document_->cancel();
        emit failed(error.key, error.detail);
    }
    draftRect_ = {};
    update();
}
void Canvas::cancelGesture() {
    retouchStroke_.reset();
    if (dragging_)
        document_->cancel();
    dragging_ = false;
    panning_ = false;
    middleZoom_ = navigatorDrag_ = toolZoom_ = zoomMoved_ = false;
    if (tool_ == Tool::Zoom) setZoomCursor(false);
    guideDrag_ = false;
    movingGuide_ = -1;
    rotating_ = false;
    healingPath_ = {};
    selectionDraft_ = {};
    draftRect_ = {};
    update();
}
void Canvas::wheelEvent(QWheelEvent *e) {
    if (e->modifiers().testFlag(Qt::ControlModifier)) {
        const QPointF point = documentPoint(e->position());
        zoom_ = std::clamp(zoom_ * std::pow(1.0015, e->angleDelta().y()), .02, 32.);
        offset_ = e->position() - point * zoom_;
        emit zoomChanged(zoom_);
        scheduleRender();
    } else {
        const QPoint delta = e->pixelDelta().isNull() ? e->angleDelta() / 3 : e->pixelDelta();
        offset_ += delta;
        scheduleRender();
    }
    e->accept();
}
void Canvas::keyPressEvent(QKeyEvent *e) {
    if (tool_ == Tool::Zoom && !middleZoom_ && e->key() == Qt::Key_Alt) setZoomCursor(true);
    if (tool_ == Tool::Crop && (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) &&
        !draftRect_.isEmpty()) {
        try { document_->crop(draftRect_.toAlignedRect()); }
        catch (const Error &error) { emit failed(error.key, error.detail); }
        draftRect_ = {};
        update();
        return;
    }
    if (tool_ == Tool::Lasso && polygonal_ && !polygonPoints_.isEmpty()) {
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
            if (polygonPoints_.size() > 2) {
                QPainterPath path;
                path.addPolygon(polygonPoints_);
                path.closeSubpath();
                document_->setSelection(path, gestureSelectionMode_);
            }
            polygonPoints_.clear();
            selectionDraft_ = {};
            update();
            return;
        }
        if (e->key() == Qt::Key_Backspace) {
            polygonPoints_.removeLast();
            selectionDraft_ = {};
            selectionDraft_.addPolygon(polygonPoints_);
            update();
            return;
        }
        if (e->key() == Qt::Key_Escape) {
            polygonPoints_.clear();
            selectionDraft_ = {};
            update();
            return;
        }
    }

    if (e->key() == Qt::Key_F && fullscreenCanvas_) {
        emit fullscreenRequested();
        return;
    }
    if (e->key() == Qt::Key_Delete) {
        emit deleteRequested();
        e->accept();
        return;
    }
    if (e->key() == Qt::Key_Space) {
        space_ = true;
        e->accept();
        return;
    }
    if (e->key() == Qt::Key_Escape) {
        cancelGesture();
        return;
    }
    if (e->key() == Qt::Key_BracketLeft || e->key() == Qt::Key_BracketRight) {
        brushSize_ = std::clamp(brushSize_ + (e->key() == Qt::Key_BracketRight ? 5 : -5), 1, 2000);
        emit brushSizeChanged(brushSize_);
        update();
        return;
    }
    if (e->key() >= Qt::Key_0 && e->key() <= Qt::Key_9 && isBrush()) {
        int v = (e->key() - Qt::Key_0) * 10;
        if (v == 0)
            v = 100;
        flow_ = v / 100.;
        emit flowChanged(v);
        return;
    }
    QPointF delta;
    if (e->key() == Qt::Key_Left)
        delta.setX(-1);
    if (e->key() == Qt::Key_Right)
        delta.setX(1);
    if (e->key() == Qt::Key_Up)
        delta.setY(-1);
    if (e->key() == Qt::Key_Down)
        delta.setY(1);
    if (!delta.isNull()) {
        try {
            const auto amount = delta * (e->modifiers().testFlag(Qt::ShiftModifier) ? 10 : 1);
            if ((tool_ == Tool::Marquee || tool_ == Tool::Lasso || tool_ == Tool::Wand) &&
                document_->state().selectionActive) {
                QTransform shift;
                shift.translate(amount.x(), amount.y());
                document_->setSelection(shift.map(document_->state().selection));
            } else
                document_->moveBy(amount);
        } catch (const Error &error) {
            emit failed(error.key, error.detail);
        }
        return;
    }
    QWidget::keyPressEvent(e);
}
void Canvas::keyReleaseEvent(QKeyEvent *e) {
    if (tool_ == Tool::Zoom && !middleZoom_ && e->key() == Qt::Key_Alt) setZoomCursor(false);
    if (e->key() == Qt::Key_Space)
        space_ = false;
    QWidget::keyReleaseEvent(e);
}
void Canvas::focusOutEvent(QFocusEvent *e) {
    space_ = false;
    cancelGesture();
    QWidget::focusOutEvent(e);
}
void Canvas::dragEnterEvent(QDragEnterEvent *e) {
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
}
void Canvas::dropEvent(QDropEvent *e) {
    QStringList paths;
    for (const auto &url : e->mimeData()->urls())
        if (url.isLocalFile())
            paths.append(url.toLocalFile());
    emit filesDropped(paths);
    e->acceptProposedAction();
}
void Canvas::beginInlineText(const QString &id) {
    finishInlineText(true);
    int i = document_->state().index(id);
    if (i < 0)
        return;
    const auto &layer = document_->state().layers[i];
    inlineTextId_ = id;
    inlineText_ = new QTextEdit(this);
    inlineText_->setObjectName("inlineTextEditor");
    // A paint-on-screen parent has no QWidget backing store for alien children.
    // Give the inline editor its own native surface above the swap chain.
    if (testAttribute(Qt::WA_NativeWindow)) inlineText_->setAttribute(Qt::WA_NativeWindow);
    inlineText_->setAcceptRichText(false);
    auto displayStyle = layer.metadata["text"].toObject();
    double scale = zoom_ * layer.placement.size.height() / std::max(1, layer.image.height());
    displayStyle["fontSize"] = std::max(1., displayStyle["fontSize"].toDouble(72) * scale);
    displayStyle["tracking"] = displayStyle["tracking"].toDouble() * scale;
    displayStyle["leading"] = displayStyle["leading"].toDouble() * scale;
    auto boxSize = displayStyle["boxSize"].toArray();
    if (boxSize.size() == 2)
        displayStyle["boxSize"] = QJsonArray{boxSize[0].toDouble() * scale, boxSize[1].toDouble() * scale};
    loadText(*inlineText_->document(), displayStyle);
    auto bounds = layer.placement.matrix(layer.image.size()).mapRect(QRectF(QPointF(), layer.image.size()));
    QRect box(viewPoint(bounds.topLeft()).toPoint(), (bounds.size() * zoom_).toSize());
    box.setWidth(std::max(160, box.width()));
    box.setHeight(std::max(80, box.height()));
    inlineText_->setGeometry(box);
    inlineText_->setStyleSheet("QTextEdit{background:rgba(30,30,30,210);border:1px solid #80afff;}");
    inlineText_->installEventFilter(this);
    inlineText_->show();
    inlineText_->setFocus();
}
void Canvas::finishInlineText(bool apply) {
    if (!inlineText_)
        return;
    auto *editor = inlineText_;
    inlineText_ = nullptr;
    const QString id = inlineTextId_;
    inlineTextId_.clear();
    if (apply && !editor->toPlainText().trimmed().isEmpty()) {
        try {
            document_->edit([&](State &s) {
                int i = s.index(id);
                if (i < 0)
                    return;
                auto &l = s.layers[i];
                auto style = textStyle(*editor->document(), l.metadata["text"].toObject());
                auto image = renderText(style, document_->state().bitDepth);
                const QSize old = l.image.size();
                l.placement.size = {image.width() * l.placement.size.width() / std::max(1, old.width()),
                                    image.height() * l.placement.size.height() / std::max(1, old.height())};
                l.image = image;
                l.originalPng.clear();
                l.metadata["text"] = style;
                l.name = editor->toPlainText().split('\n').first().left(60);
            });
        } catch (const Error &error) {
            emit failed(error.key, error.detail);
        }
    }
    editor->hide();
    editor->deleteLater();
}
bool Canvas::eventFilter(QObject *object, QEvent *event) {
    if (object == inlineText_ && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape) {
            finishInlineText(false);
            setFocus();
            return true;
        }
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
            key->modifiers().testFlag(Qt::ControlModifier)) {
            finishInlineText();
            setFocus();
            return true;
        }
    }
    return QWidget::eventFilter(object, event);
}
} // namespace comp
