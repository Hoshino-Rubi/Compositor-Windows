#include "WideProcessing.h"
#include "RetouchStroke.h"
#include "Pixels.h"
#include "Warp.h"
#include <QPainter>
#include <cmath>
namespace comp {
static QRgb sample(const QImage &image, double x, double y) {
    x = std::clamp(x, 0., double(image.width() - 1));
    y = std::clamp(y, 0., double(image.height() - 1));
    const int ix = int(x), iy = int(y), jx = std::min(ix + 1, image.width() - 1),
              jy = std::min(iy + 1, image.height() - 1);
    const double fx = x - ix, fy = y - iy;
    QRgb a = image.pixel(ix, iy), b = image.pixel(jx, iy), c = image.pixel(ix, jy), d = image.pixel(jx, jy);
    auto mix = [&](int shift) {
        return qRound((((a >> shift) & 255) * (1 - fx) + ((b >> shift) & 255) * fx) * (1 - fy) +
                      (((c >> shift) & 255) * (1 - fx) + ((d >> shift) & 255) * fx) * fy);
    };
    return qRgba(mix(16), mix(8), mix(0), mix(24));
}
void warpDab(QImage &image, QPointF from, QPointF to, double radius, double hardness, double strength, bool smudge,
             const QPainterPath &selection) {
    if (hdrImage(image)) {
        QImage coverage;
        if(!selection.isEmpty()) {coverage=QImage(image.size(),QImage::Format_Grayscale16);if(coverage.isNull())throw Error("tooLarge");coverage.fill(Qt::black);QPainter p(&coverage);p.fillPath(selection,Qt::white);}
        RetouchStroke stroke(image,radius,hardness,strength,smudge,coverage);stroke.append(image,{from,to});return;
    }
    if (wideImage(image))
        return warpDab16(image, from, to, radius, hardness, strength, smudge, selection);
    if (image.isNull() || radius < 1 || from == to)
        return;
    image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QPointF movement = (to - from) * (smudge ? 1 : strength);
    QRect area =
        QRectF(to - QPointF(radius, radius), QSizeF(radius * 2, radius * 2)).toAlignedRect().intersected(image.rect());
    if (area.isEmpty())
        return;
    const int margin = qCeil(std::max(abs(movement.x()), abs(movement.y()))) + 2;
    const QRect sourceRect = area.adjusted(-margin, -margin, margin, margin).intersected(image.rect());
    QImage source = image.copy(sourceRect);
    const double hard = std::clamp(hardness, 0., .98);
    for (int y = area.top(); y <= area.bottom(); ++y) {
        auto *row = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = area.left(); x <= area.right(); ++x) {
            if (!selection.isEmpty() && !selection.contains(QPointF(x + .5, y + .5)))
                continue;
            const double u = QLineF(QPointF(x + .5, y + .5), to).length() / radius;
            if (u >= 1)
                continue;
            double weight = u <= hard ? 1 : (1 - u) / (1 - hard);
            weight = weight * weight * (3 - 2 * weight);
            const QPointF pos = QPointF(x, y) - sourceRect.topLeft() - movement * (smudge ? 1 : weight);
            QRgb pixel = sample(source, pos.x(), pos.y());
            if (smudge) {
                const double alpha = weight * strength;
                QRgb original = row[x];
                pixel = qRgba(qRound(qRed(original) * (1 - alpha) + qRed(pixel) * alpha),
                              qRound(qGreen(original) * (1 - alpha) + qGreen(pixel) * alpha),
                              qRound(qBlue(original) * (1 - alpha) + qBlue(pixel) * alpha),
                              qRound(qAlpha(original) * (1 - alpha) + qAlpha(pixel) * alpha));
            }
            row[x] = pixel;
        }
    }
}
QImage perspectiveWarp(const QImage &image, const QPolygonF &destination) {
    if (wideImage(image) && !hdrImage(image))
        return perspectiveWarp16(image, destination);
    QPolygonF source{QPointF(0, 0), QPointF(image.width(), 0), QPointF(image.width(), image.height()),
                     QPointF(0, image.height())};
    QTransform transform;
    if (!QTransform::quadToQuad(source, destination, transform))
        throw Error("invalidTransform");
    QRect bounds = destination.boundingRect().toAlignedRect();
    if (bounds.width() > MaxSide || bounds.height() > MaxSide ||
        qint64(bounds.width()) * bounds.height() > MaxSourcePixels)
        throw Error("tooLarge");
    QImage result(bounds.size(), pixelFormat(image));
    if (result.isNull())
        throw Error("tooLarge");
    result.fill(Qt::transparent);
    QPainter p(&result);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.translate(-bounds.topLeft());
    p.setTransform(transform, true);
    p.drawImage(0, 0, image);
    return result;
}
} // namespace comp
