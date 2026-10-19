#include "SpatialFilters.h"
#include "Filters.h"
#include "Pixels.h"
#include <QPainter>
#include <numbers>
namespace comp {
QPainterPath roundedShapePath(QRectF rect, double radius, bool smoothing) {
    radius = std::clamp(radius, 0., std::min(rect.width(), rect.height()) / 2);
    QPainterPath path;
    const double smooth =
        radius > 0 && smoothing
            ? std::min(.6, std::max(0., std::min(rect.width(), rect.height()) / (2 * radius) - 1))
            : 0;
    if (smooth <= 0) {
        path.addRoundedRect(rect, radius, radius);
        return path;
    }
    const double reach = (1 + smooth) * radius, angle = smooth * std::numbers::pi / 4;
    const double tangent = radius * std::tan(angle / 2), c = tangent * std::cos(angle),
                 d = tangent * std::sin(angle);
    const double arcLength = radius * std::sqrt(2.) * std::sin((1 - smooth) * std::numbers::pi / 4);
    const double b = (reach - arcLength - c - d) / 3, a = 2 * b;
    const QPointF corners[]{rect.topRight(), rect.bottomRight(), rect.bottomLeft(), rect.topLeft()};
    for (int i = 0; i < 4; ++i) {
        QTransform transform;
        transform.translate(corners[i].x(), corners[i].y());
        transform.rotate(i * 90);
        QPainterPath corner;
        corner.moveTo(-reach, 0);
        corner.cubicTo(-reach + a, 0, -reach + a + b, 0, -reach + a + b + c, d);
        corner.arcTo(QRectF(-2 * radius, 0, 2 * radius, 2 * radius), 90 - angle * 180 / std::numbers::pi,
                     -(90 - 2 * angle * 180 / std::numbers::pi));
        corner.cubicTo(0, reach - a - b, 0, reach - a, 0, reach);
        corner = transform.map(corner);
        if (i == 0)
            path = corner;
        else
            path.connectPath(corner);
    }
    path.closeSubpath();
    return path;
}
QImage blurPreservingTransparency(const QImage &source, double radius) {
    if (source.isNull() || radius <= 0)
        return source;
    const auto blurred = blurImage(source, radius).convertToFormat(QImage::Format_RGBA64);
    const auto original = source.convertToFormat(QImage::Format_RGBA64);
    QImage result(blurred.size(), QImage::Format_RGBA64);
    if (result.isNull())
        throw Error("tooLarge");
    for (int y = 0; y < result.height(); ++y) {
        auto *out = reinterpret_cast<QRgba64 *>(result.scanLine(y));
        const auto *rgb = reinterpret_cast<const QRgba64 *>(blurred.constScanLine(y));
        const auto *alpha = reinterpret_cast<const QRgba64 *>(original.constScanLine(y));
        for (int x = 0; x < result.width(); ++x)
            out[x] = QRgba64::fromRgba64(rgb[x].red(), rgb[x].green(), rgb[x].blue(), alpha[x].alpha());
    }
    return result.convertToFormat(pixelFormat(source));
}
QImage progressiveBlur(const QImage &source, const QJsonObject &settings) {
    if (source.isNull())
        return source;
    double from = std::clamp(settings["progressiveFrom"].toDouble(), 0., 250.);
    double to = std::clamp(settings["progressiveTo"].toDouble(20), 0., 250.);
    if (settings["progressiveInvert"].toBool())
        std::swap(from, to);
    const QPointF start(settings["progressiveStartX"].toDouble(.25) * source.width(),
                        settings["progressiveStartY"].toDouble(.5) * source.height());
    const QPointF end(settings["progressiveEndX"].toDouble(.75) * source.width(),
                      settings["progressiveEndY"].toDouble(.5) * source.height());
    const auto delta = end - start;
    const double length2 = std::max(1e-12, QPointF::dotProduct(delta, delta));
    const bool radial = settings["progressiveRadial"].toBool();
    QImage result(source.size(), QImage::Format_RGBA32FPx4_Premultiplied);
    if (result.isNull())
        throw Error("tooLarge");
    const double maximum = std::max(from, to);
    if (maximum <= 0)
        return source;
    QImage low = source.convertToFormat(result.format());
    double previous = 0;
    for (double next = std::min(.5, maximum);; next = std::min(maximum, next * 2)) {
        const auto high = blurImage(source, next).convertToFormat(result.format());
        for (int y = 0; y < source.height(); ++y) {
            auto *out = reinterpret_cast<FloatPixel *>(result.scanLine(y));
            const auto *a = reinterpret_cast<const FloatPixel *>(low.constScanLine(y));
            const auto *b = reinterpret_cast<const FloatPixel *>(high.constScanLine(y));
            for (int x = 0; x < source.width(); ++x) {
                const auto v = QPointF(x + .5, y + .5) - start;
                double t = radial ? std::sqrt(QPointF::dotProduct(v, v) / length2)
                                  : QPointF::dotProduct(v, delta) / length2;
                const double radius = from + (to - from) * std::clamp(t, 0., 1.);
                if (radius < previous || radius > next)
                    continue;
                t = (radius - previous) / (next - previous);
                auto mix = [t](float u, float v) { return float(u * (1 - t) + v * t); };
                out[x] = floatRgba(mix(a[x].r, b[x].r), mix(a[x].g, b[x].g), mix(a[x].b, b[x].b),
                                   mix(a[x].a, b[x].a));
            }
        }
        if (next >= maximum)
            break;
        previous = next;
        low = high;
    }
    return result.convertToFormat(source.format());
}
QImage motionBlurClamped(const QImage &source, const QJsonObject &settings) {
    if (source.isNull())
        return source;
    const int margin = std::clamp(int(std::ceil(settings["motionDistance"].toDouble(10) / 2)) + 3, 4, 1003);
    const QSize size = source.size() + QSize(2 * margin, 2 * margin);
    if (size.width() > MaxSide || size.height() > MaxSide ||
        qint64(size.width()) * size.height() > MaxSourcePixels)
        throw Error("tooLarge");
    QImage padded(size, pixelFormat(source));
    if (padded.isNull())
        throw Error("tooLarge");
    padded.fill(Qt::transparent);
    QPainter painter(&padded);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    const int w = source.width(), h = source.height();
    painter.drawImage(margin, margin, source);
    painter.drawImage(QRect(0, margin, margin, h), source, QRect(0, 0, 1, h));
    painter.drawImage(QRect(margin + w, margin, margin, h), source, QRect(w - 1, 0, 1, h));
    painter.drawImage(QRect(margin, 0, w, margin), source, QRect(0, 0, w, 1));
    painter.drawImage(QRect(margin, margin + h, w, margin), source, QRect(0, h - 1, w, 1));
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x)
            painter.drawImage(QRect(x ? margin + w : 0, y ? margin + h : 0, margin, margin), source,
                              QRect(x ? w - 1 : 0, y ? h - 1 : 0, 1, 1));
    painter.end();
    return filtered(padded, settings).copy(margin, margin, w, h);
}
void expandLayerPixels(Layer &layer, QRect bounds) {
    const QSize oldSize = layer.image.size();
    bounds = bounds.united(QRect(QPoint(), oldSize));
    if (bounds == QRect(QPoint(), oldSize))
        return;
    if (bounds.width() > MaxSide || bounds.height() > MaxSide ||
        qint64(bounds.width()) * bounds.height() > MaxSourcePixels)
        throw Error("tooLarge");
    auto pad = [&](QImage image) {
        QImage result(bounds.size(), image.format());
        if (result.isNull())
            throw Error("tooLarge");
        result.fill(Qt::transparent);
        QPainter p(&result);
        p.drawImage(-bounds.topLeft(), image);
        p.end();
        return result;
    };
    // Preserve the old image-to-document matrix, including rotation/flips.
    const auto oldMatrix = layer.placement.matrix(oldSize);
    layer.image = pad(layer.image);
    if (!layer.mask.isNull() && layer.maskLinked) {
        const QSize maskSize = layer.mask.size();
        const auto format = layer.mask.format();
        QImage paddedMask(bounds.size(), format);
        if (paddedMask.isNull()) throw Error("tooLarge");
        // Outside the former layer, an explicit mask must not hide newly generated pixels.
        paddedMask.fill(Qt::white);
        QPainter painter(&paddedMask);
        painter.drawImage(QRect(-bounds.topLeft(), oldSize), layer.mask, QRect(QPoint(), maskSize));
        painter.end(); layer.mask = paddedMask; layer.originalMaskPng.clear();
    }
    auto &placement = layer.placement;
    placement.size = QSizeF(placement.size.width() * bounds.width() / oldSize.width(),
                            placement.size.height() * bounds.height() / oldSize.height());
    const auto newMatrix = placement.matrix(bounds.size());
    placement.origin += oldMatrix.map(QPointF(bounds.topLeft())) - newMatrix.map(QPointF());
    layer.originalPng.clear();
}
void prepareBlurBounds(Layer &layer, QSize canvas, const QJsonObject &settings) {
    const auto kind = settings["kind"].toString();
    if (layer.image.isNull() ||
        (kind != "Gaussian Blur" && kind != "Motion Blur" && kind != "Progressive Blur") ||
        settings["preserveTransparency"].toBool())
        return;
    const double radius =
        kind == "Motion Blur" ? settings["motionDistance"].toDouble(10) / 2
        : kind == "Progressive Blur"
            ? std::max(settings["progressiveFrom"].toDouble(), settings["progressiveTo"].toDouble(20)) * 3
            : settings["blurRadius"].toDouble(5) * 3;
    const int margin = std::clamp(int(std::ceil(radius + 2)), 0, 3002);
    QRect bounds = layer.image.rect().adjusted(-margin, -margin, margin, margin);
    const auto matrix = layer.placement.matrix(layer.image.size());
    // Only edges coinciding with the canvas clamp. Interior layer edges spread into clear pixels.
    if (std::abs(matrix.m12()) < 1e-9 && std::abs(matrix.m21()) < 1e-9) {
        const auto canvasPixels = matrix.inverted().mapRect(QRectF(QPointF(), canvas));
        if (std::abs(canvasPixels.left()) < .01)
            bounds.setLeft(0);
        if (std::abs(canvasPixels.top()) < .01)
            bounds.setTop(0);
        if (std::abs(canvasPixels.right() - layer.image.width()) < .01)
            bounds.setRight(layer.image.width() - 1);
        if (std::abs(canvasPixels.bottom() - layer.image.height()) < .01)
            bounds.setBottom(layer.image.height() - 1);
    }
    expandLayerPixels(layer, bounds);
}
} // namespace comp
