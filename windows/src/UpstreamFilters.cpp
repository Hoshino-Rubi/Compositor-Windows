#include "Filters.h"
#include "Pixels.h"
#include "PixelParallel.h"
#include <array>
#include <cmath>
namespace comp {
QJsonObject transformFilterLayer(QJsonObject settings, const QTransform &mapping, double scale) {
    if (!isFilterLayerKind(settings["kind"].toString())) return settings;
    auto f = settings["filterLayerSettings"].toObject();
    for (const auto key : {"progressiveStart", "progressiveEnd", "rgbSplitCenter"}) {
        const auto a = f[key].toArray();
        if (a.size() == 2) { const auto p = mapping.map(QPointF(a[0].toDouble(), a[1].toDouble())); f[key] = QJsonArray{p.x(), p.y()}; }
    }
    for (const auto key : {"progressiveFrom", "progressiveTo", "bloomRadius", "rgbSplitDistance", "rgbSplitBlur", "rgbSplitReach"})
        if (f.contains(key)) f[key] = f[key].toDouble() * scale;
    settings["filterLayerSettings"] = f;
    return settings;
}
QJsonObject defaultFilterLayerSettings(QSize canvas) {
    return {{"progressiveFrom", 0}, {"progressiveTo", 20},
            {"progressiveStart", QJsonArray{canvas.width() * .5, canvas.height() * .35}},
            {"progressiveEnd", QJsonArray{canvas.width() * .5, double(canvas.height())}},
            {"progressiveRadial", false}, {"progressiveInverted", false},
            {"bloomAmount", 40}, {"bloomRadius", 24}, {"rgbSplitDistance", 10},
            {"rgbSplitBlur", 0}, {"rgbSplitAngle", 0}, {"rgbSplitRadial", false},
            {"rgbSplitCenter", QJsonArray{canvas.width() * .5, canvas.height() * .5}},
            {"rgbSplitReach", std::max(1., std::hypot(canvas.width(), canvas.height()) / 2)}};
}
bool isFilterLayerKind(const QString &kind) {
    return kind == "Progressive Blur" || kind == "Bloom / Glow" || kind == "RGB Split";
}
QJsonObject mappedFilterLayer(const QJsonObject &settings, QSize canvas, QSize target) {
    auto result = settings;
    const auto f = settings["filterLayerSettings"].toObject();
    for (auto it = f.begin(); it != f.end(); ++it) result[it.key()] = it.value();
    result.remove("filterLayerSettings");
    const double scale = double(target.width()) / std::max(1, canvas.width());
    for (const auto &entry : {std::pair{"progressiveFrom", 0.}, {"progressiveTo", 20.},
                             {"bloomRadius", 24.}, {"rgbSplitDistance", 10.},
                             {"rgbSplitBlur", 0.}, {"rgbSplitReach", 1000.}})
        result[entry.first] = result[entry.first].toDouble(entry.second) * scale;
    auto point = [&](const char *key, QPointF fallback) {
        const auto a = result[key].toArray();
        return a.size() == 2 ? QPointF(a[0].toDouble(), a[1].toDouble()) : fallback;
    };
    const auto start = point("progressiveStart", {canvas.width() * .5, canvas.height() * .35});
    const auto end = point("progressiveEnd", {canvas.width() * .5, double(canvas.height())});
    result["progressiveStartX"] = start.x() / std::max(1, canvas.width());
    result["progressiveStartY"] = start.y() / std::max(1, canvas.height());
    result["progressiveEndX"] = end.x() / std::max(1, canvas.width());
    result["progressiveEndY"] = end.y() / std::max(1, canvas.height());
    result["progressiveInvert"] = result["progressiveInverted"].toBool();
    const auto center = point("rgbSplitCenter", {canvas.width() * .5, canvas.height() * .5});
    result["rgbSplitCenter"] = QJsonArray{center.x() * target.width() / std::max(1, canvas.width()),
                                        center.y() * target.height() / std::max(1, canvas.height())};
    return result;
}
// Reference CPU port of upstream MetalDispersion's weighted spectral sampling.
// Float intermediates retain native integer depth and HDR values; no GPU performance claim.
QImage rgbSplit(const QImage &source, const QJsonObject &settings) {
    const double distance = std::clamp(settings["rgbSplitDistance"].toDouble(10), 0., 200.);
    const double blur = std::clamp(settings["rgbSplitBlur"].toDouble(), 0., 400.);
    if (source.isNull() || (distance == 0 && blur == 0)) return source;
    const bool radial = settings["rgbSplitRadial"].toBool();
    const double angle = settings["rgbSplitAngle"].toDouble() * 3.14159265358979323846 / 180.;
    const auto center = settings["rgbSplitCenter"].toArray();
    const QPointF origin = center.size() == 2 ? QPointF(center[0].toDouble(), center[1].toDouble())
                                             : QPointF(source.width() * .5, source.height() * .5);
    const double reach = std::max(1., settings["rgbSplitReach"].toDouble(1000));
    const int spectral = distance > 0 ? std::clamp(int(distance * (radial ? 2 : 1)), 3, 32) : 1;
    const int smear = blur > 0 ? std::clamp(int(blur * (radial ? 2 : 1) / 2), 2, 24) : 1;
    auto input = source.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
    QImage output(source.size(), input.format());
    if (input.isNull() || output.isNull()) throw Error("tooLarge");
    const auto *in = input.constBits(); auto *out = output.bits();
    const auto stride = input.bytesPerLine(), outStride = output.bytesPerLine();
    const int w = input.width(), h = input.height();
    auto sample = [&](double x, double y) {
        x = std::clamp(x, 0., double(w - 1)); y = std::clamp(y, 0., double(h - 1));
        const int ix = int(x), iy = int(y), jx = std::min(ix + 1, w - 1), jy = std::min(iy + 1, h - 1);
        const auto *a = reinterpret_cast<const float *>(in + iy * stride);
        const auto *b = reinterpret_cast<const float *>(in + jy * stride);
        std::array<double, 4> p{};
        for (int c = 0; c < 4; ++c)
            p[c] = (a[ix * 4 + c] * (1 - (x - ix)) + a[jx * 4 + c] * (x - ix)) * (1 - (y - iy)) +
                   (b[ix * 4 + c] * (1 - (x - ix)) + b[jx * 4 + c] * (x - ix)) * (y - iy);
        return p;
    };
    auto smooth = [](double lo, double hi, double x) { x = std::clamp((x - lo) / (hi - lo), 0., 1.); return x * x * (3 - 2 * x); };
    pixel_parallel(h, [&](size_t row) {
        const int y = int(row);
        auto *dst = reinterpret_cast<FloatPixel *>(out + y * outStride);
        for (int x = 0; x < w; ++x) {
            const double dx = radial ? (x + .5 - origin.x()) / reach : std::cos(angle);
            const double dy = radial ? (y + .5 - origin.y()) / reach : -std::sin(angle);
            const double hash = std::sin(x * 12.9898 + y * 78.233) * 43758.5453;
            const double jitter = hash - std::floor(hash);
            std::array<double, 3> color{}, alpha{}, total{};
            for (int i = 0; i < spectral; ++i) {
                const double t = spectral == 1 ? 0 : (i + jitter) / spectral * 2 - 1;
                const std::array<double, 3> weight{smooth(-.3, 1., t), 1 - smooth(0, 1., std::abs(t)) + .05, smooth(-.3, 1., -t)};
                for (int j = 0; j < smear; ++j) {
                    const double u = smear == 1 ? 0 : (j + jitter) / smear;
                    const auto p = sample(x - dx * (distance * t + blur * u), y - dy * (distance * t + blur * u));
                    for (int c = 0; c < 3; ++c) {
                        color[c] += weight[c] * p[c]; alpha[c] += weight[c] * p[3]; total[c] += weight[c];
                    }
                }
            }
            for (int c = 0; c < 3; ++c) { color[c] /= total[c]; alpha[c] /= total[c]; }
            const auto a = float(std::max({alpha[0], alpha[1], alpha[2]}));
            dst[x] = {float(color[0]), float(color[1]), float(color[2]), a};
        }
    });
    return output.convertToFormat(pixelFormat(source));
}
} // namespace comp
