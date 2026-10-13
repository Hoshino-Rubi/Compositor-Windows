#include "GpuPaintStroke.h"

#include "GpuPaint.h"
#include "Pixels.h"

#include <QLineF>
#include <QPainter>
#include <QRadialGradient>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>

namespace comp {
namespace {

constexpr quint64 GpuPaintPixelThreshold = 65536;
constexpr quint64 GpuPaintMaxPixels = 8ull * 1024 * 1024;
constexpr int GpuPaintMaxDabs = 8192;

bool finite(double value) {
    return std::isfinite(value);
}

bool finitePoint(const QPointF &point) {
    return finite(point.x()) && finite(point.y());
}

bool finiteTransform(const QTransform &transform) {
    return finite(transform.m11()) && finite(transform.m12()) && finite(transform.m13()) &&
           finite(transform.m21()) && finite(transform.m22()) && finite(transform.m23()) &&
           finite(transform.m31()) && finite(transform.m32()) && finite(transform.m33());
}

// Paint's sourceOffset is constant for every pixel in a dab.  A projective
// mapping would make that offset vary with position, so keep those requests on
// Canvas's reference path rather than silently changing clone sampling.
bool affineTransform(const QTransform &transform) {
    return transform.type() <= QTransform::TxShear && finiteTransform(transform);
}

QPointF mapVector(const QTransform &transform, const QPointF &vector) {
    return transform.map(vector) - transform.map(QPointF());
}

bool checkedArea(const QSize &size, quint64 &area) {
    if (!size.isValid() || size.width() <= 0 || size.height() <= 0)
        return false;
    const quint64 width = quint64(size.width()), height = quint64(size.height());
    if (height > std::numeric_limits<quint64>::max() / width)
        return false;
    area = width * height;
    return true;
}

bool addWork(quint64 amount, quint64 &work) {
    if (amount > std::numeric_limits<quint64>::max() - work)
        return false;
    work += amount;
    return true;
}

int dabSteps(double distance, double spacing) {
    if (!finite(distance) || !finite(spacing) || spacing <= 0)
        return -1;
    const double ratio = distance / spacing;
    if (!finite(ratio) || ratio > double(std::numeric_limits<int>::max() - 1))
        return -1;
    return std::max(1, int(ratio));
}

QImage::Format coverageFormat(const QImage &target) {
    return wideImage(target) ? QImage::Format_Grayscale16 : QImage::Format_Grayscale8;
}

QPainterPath strokedLine(QPointF from, QPointF to, double brushSize) {
    QPainterPath segment;
    segment.moveTo(from);
    segment.lineTo(to);
    QPainterPathStroker stroker;
    stroker.setWidth(brushSize);
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    auto shape = stroker.createStroke(segment);
    // Canvas deliberately draws the endpoint a second time for a stationary
    // hard dab.  Keep that overdraw in the coverage raster (it matters at the
    // antialiased boundary even though the interior remains opaque).
    if (from == to)
        shape.addEllipse(from, brushSize / 2., brushSize / 2.);
    return shape;
}

QPainterPath tipPath(const QPointF &point, double brushSize) {
    QPainterPath tip;
    tip.addEllipse(point, brushSize / 2., brushSize / 2.);
    return tip;
}

QRect mappedCoverageRect(const QPainterPath &shape, const QTransform &documentToPixel) {
    const auto mapped = documentToPixel.map(shape).boundingRect();
    if (!finite(mapped.left()) || !finite(mapped.top()) || !finite(mapped.right()) || !finite(mapped.bottom()))
        return {};
    const double coordinateLimit = double(std::numeric_limits<int>::max()) / 4.;
    if (std::abs(mapped.left()) > coordinateLimit || std::abs(mapped.top()) > coordinateLimit ||
        std::abs(mapped.right()) > coordinateLimit || std::abs(mapped.bottom()) > coordinateLimit)
        return {};
    // Keep one extra pixel around the transformed shape for Qt's antialiasing
    // fringe.  toAlignedRect follows QPainter's integer device coverage.
    return mapped.toAlignedRect().adjusted(-1, -1, 1, 1);
}

QRect coverageRect(const QPainterPath &shape, const QTransform &documentToPixel, const QRect &bounds) {
    return mappedCoverageRect(shape, documentToPixel).intersected(bounds);
}

// Saturate `work` when the conservative estimate reaches the GPU threshold.
// Only a valid estimate strictly below the threshold permits the cheap
// preflight rejection; invalid geometry is rejected by the caller.
void accumulateEstimatedWork(quint64 area, quint64 count, quint64 &work) {
    if (area == 0 || count == 0 || work >= GpuPaintPixelThreshold)
        return;
    if (area >= GpuPaintPixelThreshold || count >= GpuPaintPixelThreshold ||
        area > (GpuPaintPixelThreshold - 1 - work) / count) {
        work = GpuPaintPixelThreshold;
        return;
    }
    work += area * count;
}

template <typename Draw>
QImage renderCoverage(const QRect &rect, const QTransform &documentToPixel, const QPainterPath &selection,
                      QImage::Format format, Draw draw) {
    if (rect.isEmpty())
        return {};
    QImage coverage(rect.size(), format);
    if (coverage.isNull())
        return {};
    coverage.fill(0);

    QTransform translation;
    translation.translate(-rect.left(), -rect.top());
    // QTransform uses row-vector composition: documentToPixel maps the
    // document point first, then the translation moves it into the ROI.
    const QTransform local = documentToPixel * translation;
    QPainter painter(&coverage);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setTransform(local);
    if (!selection.isEmpty())
        painter.setClipPath(selection, Qt::IntersectClip);
    draw(painter);
    painter.end();
    return coverage;
}

bool appendDab(QVector<GpuPaintDab> &dabs, QRect &roi, quint64 &work, const QPainterPath &shape,
               const QTransform &documentToPixel, const QPointF &sourceOffset, const QRect &bounds,
               const std::function<QImage(QRect)> &render) {
    if (dabs.size() >= GpuPaintMaxDabs)
        return false;
    const QRect rect = coverageRect(shape, documentToPixel, bounds);
    if (rect.isEmpty())
        return true;
    const QImage coverage = render(rect);
    if (coverage.isNull() || coverage.size() != rect.size())
        return false;
    quint64 pixels = 0;
    if (!checkedArea(rect.size(), pixels) || !addWork(pixels, work))
        return false;
    GpuPaintDab dab;
    dab.targetRect = rect;
    dab.coverage = coverage;
    dab.sourceOffset = sourceOffset;
    dabs.append(std::move(dab));
    roi = roi.isEmpty() ? rect : roi.united(rect);
    return true;
}

} // namespace

bool paintStrokeAccelerated(QImage &target, QPointF from, QPointF to, double brushSize, double hardness,
                            double flow, QColor color, bool erase, bool maskTarget,
                            const QTransform &documentToPixel, const QPainterPath &documentSelection,
                            const QImage &retouchSource, QPointF cloneOffset, bool cloneOrBlur) {
    if (target.isNull() || !finitePoint(from) || !finitePoint(to) || !finite(brushSize) || brushSize <= 0 ||
        !finite(hardness) || !finite(flow) || flow < 0 || flow > 1 || !color.isValid() ||
        !affineTransform(documentToPixel))
        return false;
    // The reference Canvas path remains authoritative when acceleration is
    // disabled.  Check this before rasterizing even one QPainter coverage tip.
    if (qEnvironmentVariableIntValue("COMPOSITOR_DISABLE_GPU"))
        return false;
    const int depth = wideImage(target) ? 16 : 8;
    // Canvas canonicalizes editable layers to these two premultiplied formats
    // before painting.  Do not silently quantize an unrelated QImage format
    // and call that an accelerated edit.
    if (target.format() != pixelFormat(depth))
        return false;
    if (cloneOrBlur && (retouchSource.isNull() || !finitePoint(cloneOffset)))
        return false;
    if (cloneOrBlur && retouchSource.format() != pixelFormat(depth))
        return false;

    const QRect bounds = target.rect();
    const QImage::Format format = coverageFormat(target);
    QVector<GpuPaintDab> dabs;
    dabs.reserve(32);
    QRect roi;
    quint64 workPixels = 0;

    // Eraser uses the same soft tip as Brush; its destination-out blend is
    // selected below, after the coverage has been generated.
    const bool soft = !cloneOrBlur && hardness < .99;
    // QPainter's translate(-cloneOffset) moves the source image left, hence
    // a destination pixel samples the source at destination + cloneOffset.
    const QPointF sourceOffset = cloneOrBlur ? mapVector(documentToPixel, cloneOffset) : QPointF();
    if (!finitePoint(sourceOffset))
        return false;

    // Preflight the same work budget used below.  A conservative, unclipped
    // bound is sufficient: if it is already below the threshold, clipping or
    // selection can only reduce the actual work, so generating coverage would
    // be wasted before the guaranteed CPU fallback.  Keep the exact clipped
    // count later for requests that might qualify for GPU.
    int estimatedSteps = 0;
    QPainterPath estimatedShape;
    if (cloneOrBlur || soft) {
        const int steps = dabSteps(QLineF(from, to).length(), std::max(1., brushSize * (cloneOrBlur ? .15 : .12)));
        if (steps < 0 || steps >= GpuPaintMaxDabs)
            return false;
        estimatedSteps = steps;
        estimatedShape = tipPath(from, brushSize);
    } else {
        estimatedShape = strokedLine(from, to, brushSize);
    }
    const QRect estimatedRect = mappedCoverageRect(estimatedShape, documentToPixel);
    quint64 estimatedArea = 0;
    quint64 estimatedWork = 0;
    if (checkedArea(estimatedRect.size(), estimatedArea))
        accumulateEstimatedWork(estimatedArea, quint64((cloneOrBlur || soft) ? estimatedSteps + 1 : 1),
                                estimatedWork);
    if (estimatedWork < GpuPaintPixelThreshold)
        return false;

    struct CachedTip {
        QPointF center;
        QSize size;
        QImage image;
    };
    QVector<CachedTip> tipCache;
    // Integer translations of the same unclipped tip share its exact Qt
    // raster. Do not approximate fractional phases or reuse selection clips.
    const bool reusableTip = documentSelection.isEmpty() && documentToPixel.type() <= QTransform::TxTranslate;
    auto renderTip = [&](QPointF point, QRect rect, auto draw) {
        const QPointF center = documentToPixel.map(point) - QPointF(rect.topLeft());
        if (reusableTip)
            for (const auto &entry : tipCache)
                if (entry.size == rect.size() && entry.center.x() == center.x() && entry.center.y() == center.y())
                    return entry.image;
        QImage image = renderCoverage(rect, documentToPixel, documentSelection, format, draw);
        if (reusableTip && !image.isNull() && tipCache.size() < 16)
            tipCache.append(CachedTip{center, rect.size(), image});
        return image;
    };

    if (cloneOrBlur) {
        const int steps = estimatedSteps;
        for (int step = 0; step <= steps; ++step) {
            const QPointF point = from + (to - from) * (double(step) / steps);
            const QPainterPath shape = tipPath(point, brushSize);
            if (!appendDab(dabs, roi, workPixels, shape, documentToPixel, sourceOffset, bounds, [&](QRect rect) {
                               return renderTip(point, rect, [&](QPainter &painter) {
                                                          // Canvas's clone/blur path installs the tip with
                                                          // setClipPath(), then changes the world transform before
                                                          // drawing the source image.  Rasterize that same device
                                                          // clip instead of painting an antialiased ellipse: Qt's
                                                          // clip coverage (especially after fractional rotation) is
                                                          // the reference edge semantics for this operation.
                                                          painter.save();
                                                          painter.setClipPath(shape, Qt::IntersectClip);
                                                          painter.setTransform(QTransform());
                                                          painter.fillRect(QRectF(QPointF(), QSizeF(rect.size())),
                                                                           Qt::white);
                                                          painter.restore();
                                                      });
                           }))
                return false;
        }
    } else if (soft) {
        const int steps = estimatedSteps;
        const double inner = std::min(.99, std::max(0., hardness));
        for (int step = 0; step <= steps; ++step) {
            const QPointF point = from + (to - from) * (double(step) / steps);
            const QPainterPath shape = tipPath(point, brushSize);
            if (!appendDab(dabs, roi, workPixels, shape, documentToPixel, {}, bounds, [&](QRect rect) {
                               return renderTip(point, rect, [&](QPainter &painter) {
                                                          QRadialGradient gradient(point, brushSize / 2.);
                                                          gradient.setColorAt(0, QColor::fromRgbF(1, 1, 1, 1));
                                                          gradient.setColorAt(inner, QColor::fromRgbF(1, 1, 1, 1));
                                                          gradient.setColorAt(1, QColor::fromRgbF(1, 1, 1, 0));
                                                          painter.setPen(Qt::NoPen);
                                                          painter.setBrush(gradient);
                                                          painter.drawEllipse(point, brushSize / 2., brushSize / 2.);
                                                      });
                           }))
                return false;
        }
    } else {
        const QPainterPath shape = strokedLine(from, to, brushSize);
        if (!appendDab(dabs, roi, workPixels, shape, documentToPixel, {}, bounds, [&](QRect rect) {
                           return renderCoverage(rect, documentToPixel, documentSelection, format,
                                                  [&](QPainter &painter) {
                                                      painter.setPen(QPen(Qt::white, brushSize, Qt::SolidLine,
                                                                          Qt::RoundCap, Qt::RoundJoin));
                                                      painter.drawLine(from, to);
                                                      if (from == to) {
                                                          painter.setPen(Qt::NoPen);
                                                          painter.setBrush(Qt::white);
                                                          painter.drawEllipse(from, brushSize / 2., brushSize / 2.);
                                                      }
                                                  });
                       }))
            return false;
    }

    if (dabs.isEmpty() || roi.isEmpty() || workPixels < GpuPaintPixelThreshold)
        return false;
    quint64 roiPixels = 0;
    if (!checkedArea(roi.size(), roiPixels) || roiPixels > GpuPaintMaxPixels)
        return false;

    GpuPaintOptions options;
    options.flow = flow;
    options.qtOpacity = cloneOrBlur;
    options.maskTarget = maskTarget;
    options.depth = depth;
    options.color = color;
    if (cloneOrBlur) {
        options.operation = cloneOffset == QPointF() ? GpuPaintOperation::Blur : GpuPaintOperation::Clone;
    } else if (erase && !maskTarget) {
        options.operation = GpuPaintOperation::Eraser;
    } else {
        options.operation = GpuPaintOperation::Brush;
        // Canvas replaces the ink alpha with flow (rather than multiplying
        // an existing QColor alpha), so let the GPU apply opacity exactly
        // once through options.flow.
        options.color.setAlphaF(1.0);
        if (maskTarget) {
            if (erase) {
                QColor black(Qt::black);
                options.color = black;
            } else if (options.depth == 16) {
                options.color = grayColor16(color);
            } else {
                const int gray = qGray(color.rgb());
                options.color = QColor(gray, gray, gray, color.alpha());
            }
            options.color.setAlphaF(1.0);
        }
    }

    const QImage result = paintGpuImage(target, retouchSource, dabs, options);
    if (result.isNull() || result.size() != target.size() || result.depth() != target.depth())
        return false;
    target = result;
    return true;
}

} // namespace comp
