#include "GpuWarp.h"

#include "Pixels.h"
#include "Warp.h"

#include <QImage>
#include <QRect>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace comp {
namespace {
constexpr quint64 WarpPixelThreshold = 65536;
constexpr quint64 WarpMaxPixels = 8ull * 1024 * 1024;

void warpCpu(QImage &image, const QVector<QPointF> &points, double radius, double hardness, double strength,
             bool smudge, const QPainterPath &selection) {
    for (int i = 1; i < points.size(); ++i)
        warpDab(image, points[i - 1], points[i], radius, hardness, strength, smudge, selection);
}

bool finitePoint(const QPointF &point) {
    return std::isfinite(point.x()) && std::isfinite(point.y());
}

QRect dabArea(const QPointF &point, double radius, const QRect &bounds) {
    // Keep this geometry in lockstep with Warp.cpp.  The CPU dab converts the
    // same QRectF with toAlignedRect before clipping it to the image.
    return QRectF(point - QPointF(radius, radius), QSizeF(radius * 2, radius * 2))
        .toAlignedRect()
        .intersected(bounds);
}

bool checkedMargin(const QPointF &movement, int &margin) {
    const double largest = std::max(std::abs(movement.x()), std::abs(movement.y()));
    if (!std::isfinite(largest) || largest > double(std::numeric_limits<int>::max() - 2))
        return false;
    margin = std::max(0, int(std::ceil(largest))) + 2;
    return true;
}

QImage binarySelection(const QPainterPath &selection, const QRect &roi) {
    if (selection.isEmpty())
        return {};
    QImage mask(roi.size(), QImage::Format_Grayscale8);
    if (mask.isNull())
        return {};
    mask.fill(Qt::black);
    for (int y = 0; y < mask.height(); ++y) {
        auto *row = mask.scanLine(y);
        const double globalY = roi.top() + y + .5;
        for (int x = 0; x < mask.width(); ++x)
            row[x] = selection.contains(QPointF(roi.left() + x + .5, globalY)) ? uchar(255) : uchar(0);
    }
    return mask;
}
} // namespace

void warpStrokeAccelerated(QImage &image, const QVector<QPointF> &points, double radius, double hardness,
                           double strength, bool smudge, const QPainterPath &selection) {
    if (image.isNull() || points.size() < 2)
        return;
    if (radius < 1 || !std::isfinite(radius) || !std::isfinite(hardness) || !std::isfinite(strength)) {
        // Invalid parameters have no defined pixel result.  Leave the image
        // unchanged instead of feeding NaNs to either implementation.
        return;
    }
    for (const auto &point : points)
        if (!finitePoint(point)) {
            return;
        }

    if (qEnvironmentVariableIntValue("COMPOSITOR_DISABLE_GPU")) {
        warpCpu(image, points, radius, hardness, strength, smudge, selection);
        return;
    }

    const int depth = wideImage(image) ? 16 : 8;
    if (image.format() != pixelFormat(depth)) {
        // The reference dab canonicalizes these formats for the whole image;
        // avoid a partial GPU patch whose channel/alpha representation differs.
        warpCpu(image, points, radius, hardness, strength, smudge, selection);
        return;
    }

    const QRect imageBounds = image.rect();
    QRect roi;
    QVector<QPointF> localPoints;
    localPoints.reserve(points.size());
    quint64 workPixels = 0;
    quint64 activeDabs = 0;
    bool hasDab = false;
    for (int i = 1; i < points.size(); ++i) {
        const QPointF from = points[i - 1], to = points[i];
        if (from == to)
            continue;
        const auto area = dabArea(to, radius, imageBounds);
        if (area.isEmpty())
            continue;
        const QPointF movement = (to - from) * (smudge ? 1. : strength);
        int margin = 0;
        if (!checkedMargin(movement, margin)) {
            warpCpu(image, points, radius, hardness, strength, smudge, selection);
            return;
        }
        const QRect source = area.adjusted(-margin, -margin, margin, margin).intersected(imageBounds);
        if (source.isEmpty())
            continue;
        roi = hasDab ? roi.united(source) : source;
        hasDab = true;
        const quint64 dabPixels = quint64(area.width()) * quint64(area.height());
        if (dabPixels > std::numeric_limits<quint64>::max() - workPixels)
            workPixels = std::numeric_limits<quint64>::max();
        else
            workPixels += dabPixels;
        ++activeDabs;
    }

    // A short/small dab is quicker through the reference implementation, and
    // a region above the transfer cap cannot be represented by this pass.
    const quint64 roiPixels = roi.isEmpty() ? 0 : quint64(roi.width()) * quint64(roi.height());
    const bool sparse = roiPixels && activeDabs &&
                        static_cast<long double>(roiPixels) * static_cast<long double>(activeDabs) >
                            4.0L * static_cast<long double>(workPixels);
    if (!hasDab || workPixels < WarpPixelThreshold || roi.isEmpty() || roiPixels > WarpMaxPixels || sparse) {
        warpCpu(image, points, radius, hardness, strength, smudge, selection);
        return;
    }

    const QImage::Format canonical = depth == 16 ? QImage::Format_RGBA64_Premultiplied
                                                 : QImage::Format_ARGB32_Premultiplied;
    const QImage sourceRoi = image.copy(roi).convertToFormat(canonical);
    if (sourceRoi.isNull() || sourceRoi.size() != roi.size()) {
        warpCpu(image, points, radius, hardness, strength, smudge, selection);
        return;
    }

    localPoints.clear();
    localPoints.reserve(points.size());
    for (const auto &point : points)
        localPoints.append(point - roi.topLeft());

    QImage selectionMask;
    if (!selection.isEmpty()) {
        selectionMask = binarySelection(selection, roi);
        if (selectionMask.isNull()) {
            warpCpu(image, points, radius, hardness, strength, smudge, selection);
            return;
        }
    }

    // warpGpuImage performs all transfers and leaves the input untouched when
    // hardware setup/dispatch/readback is unavailable.
    const QImage result = warpGpuImage(sourceRoi, localPoints, radius, hardness, strength, smudge, selectionMask);
    if (result.isNull() || result.size() != roi.size()) {
        warpCpu(image, points, radius, hardness, strength, smudge, selection);
        return;
    }
    const QImage patch = result.convertToFormat(image.format());
    if (patch.isNull() || patch.size() != roi.size() || patch.depth() != image.depth()) {
        warpCpu(image, points, radius, hardness, strength, smudge, selection);
        return;
    }

    // No caller-visible image detachment or writes happen before this point, so
    // every failed GPU route above can safely replay the exact CPU dab sequence.
    image.detach();
    const int bytesPerPixel = image.depth() / 8;
    for (int y = 0; y < roi.height(); ++y)
        std::memcpy(image.scanLine(roi.top() + y) + roi.left() * bytesPerPixel, patch.constScanLine(y),
                    size_t(roi.width()) * bytesPerPixel);
}
} // namespace comp
