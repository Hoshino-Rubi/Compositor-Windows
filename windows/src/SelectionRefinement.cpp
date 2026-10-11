#include "Selection.h"
#include "Filters.h"
#include <algorithm>
#include <cmath>
namespace comp {
QImage refineSubjectMask(const QImage &source, const QImage &mask, int radius, int contrast, int shift) {
    if (source.isNull() || mask.isNull() || source.size() != mask.size())
        throw Error("imageError");
    radius = std::clamp(radius, 0, 40);
    contrast = std::clamp(contrast, 0, 100);
    shift = std::clamp(shift, -10, 10);
    // Bound edge analysis to the model's practical resolution without distorting aspect ratio.
    const QSize analysis = source.width() > 2048 || source.height() > 2048
                               ? source.size().scaled(QSize(2048, 2048), Qt::KeepAspectRatio)
                               : source.size();
    QImage edges = mask.scaled(analysis, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                           .convertToFormat(QImage::Format_Grayscale8);
    if (edges.isNull()) throw Error("tooLarge");
    if (radius) {
        const QImage photo = source.scaled(analysis, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                                  .convertToFormat(QImage::Format_RGB32);
        QImage refined(edges.size(), QImage::Format_Grayscale8);
        if (refined.isNull()) throw Error("tooLarge");
        const int step = std::max(1, qRound(radius * double(analysis.width()) / source.width() / 3));
        for (int y = 0; y < edges.height(); ++y) {
            auto *out = refined.scanLine(y);
            const auto *row = edges.constScanLine(y);
            for (int x = 0; x < edges.width(); ++x) {
                const int base = row[x];
                if (base == 0 || base == 255) { out[x] = quint8(base); continue; }
                QColor center = photo.pixelColor(x, y);
                double sum = 2 * base, weight = 2;
                for (int dy : {-step, 0, step})
                    for (int dx : {-step, 0, step}) {
                        if (!dx && !dy) continue;
                        const int xx = std::clamp(x + dx, 0, edges.width() - 1);
                        const int yy = std::clamp(y + dy, 0, edges.height() - 1);
                        const QColor sample = photo.pixelColor(xx, yy);
                        const int distance = std::abs(center.red() - sample.red()) +
                                             std::abs(center.green() - sample.green()) +
                                             std::abs(center.blue() - sample.blue());
                        const double w = std::exp(-distance / 36.);
                        sum += w * edges.constScanLine(yy)[xx];
                        weight += w;
                    }
                out[x] = quint8(std::clamp(qRound(sum / weight), 0, 255));
            }
        }
        edges = std::move(refined);
    }
    if (shift) {
        const int steps = std::max(1, qRound(std::abs(shift) * double(analysis.width()) / source.width()));
        for (int pass = 0; pass < steps; ++pass) {
            QImage next(edges.size(), QImage::Format_Grayscale8);
            if (next.isNull()) throw Error("tooLarge");
            for (int y = 0; y < edges.height(); ++y) {
                auto *out = next.scanLine(y);
                for (int x = 0; x < edges.width(); ++x) {
                    int value = edges.constScanLine(y)[x];
                    for (const auto &[dx, dy] : {std::pair{1, 0}, std::pair{-1, 0},
                                                 std::pair{0, 1}, std::pair{0, -1}}) {
                        const int xx = std::clamp(x + dx, 0, edges.width() - 1);
                        const int yy = std::clamp(y + dy, 0, edges.height() - 1);
                        const int neighbor = edges.constScanLine(yy)[xx];
                        value = shift > 0 ? std::max(value, neighbor) : std::min(value, neighbor);
                    }
                    out[x] = quint8(value);
                }
            }
            edges = std::move(next);
        }
    }
    if (contrast)
        for (int y = 0; y < edges.height(); ++y) {
            auto *row = edges.scanLine(y);
            for (int x = 0; x < edges.width(); ++x)
                row[x] = quint8(std::clamp(qRound((row[x] - 127.5) * (1. + contrast / 50.) + 127.5), 0, 255));
        }
    return edges.size() == mask.size() ? edges : edges.scaled(mask.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}
} // namespace comp
