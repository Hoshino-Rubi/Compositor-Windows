#pragma once
#include <QImage>
#include <QPainter>
#include <QPoint>
#include <QSize>
#include <QTransform>
#include <QVector>

namespace comp {
// Rasterize ONLY geometric coverage using a constant white texel. No source
// document pixels are sampled on the CPU. Qt's image coverage differs from
// fillPath at projective edges; preserve it, then upload compact row spans.
inline QVector<QPoint> gpuRasterSpans(const QTransform &mapping, QSize sourceSize, QSize size, int depth) {
    if (size.width() <= 0 || size.height() <= 0 || size.height() > 30000)
        return {};
    const auto format = depth == 16 ? QImage::Format_RGBA64_Premultiplied : QImage::Format_ARGB32_Premultiplied;
    QImage white(1, 1, format), coverage(size, format);
    if (white.isNull() || coverage.isNull()) return {};
    white.fill(Qt::white); coverage.fill(Qt::transparent);
    QPainter painter(&coverage);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setTransform(mapping);
    painter.drawImage(QRectF(QPointF(), sourceSize), white);
    painter.end();
    QVector<QPoint> spans(size.height(), QPoint(0, 0));
    for (int y = 0; y < size.height(); ++y) {
        int left = 0, right = size.width();
        const auto alpha = [&](int x) {
            return depth == 16 ? int(reinterpret_cast<const QRgba64 *>(coverage.constScanLine(y))[x].alpha())
                               : qAlpha(reinterpret_cast<const QRgb *>(coverage.constScanLine(y))[x]);
        };
        while (left < right && !alpha(left)) ++left;
        while (right > left && !alpha(right - 1)) --right;
        spans[y] = QPoint(left, right);
    }
    return spans;
}
} // namespace comp
