#include "RetouchStroke.h"
#include "GpuRetouchStroke.h"
#include "Pixels.h"
#include <QHash>
#include <QLineF>
#include <array>
#include <cmath>
#include <vector>
namespace comp {
namespace {
using Color = std::array<float, 4>;
struct Offset {
    float x = 0, y = 0;
};
Color read(const QImage &image, int x, int y, bool wide) {
    x = std::clamp(x, 0, image.width() - 1);
    y = std::clamp(y, 0, image.height() - 1);
    if (wide) {
        const auto p = reinterpret_cast<const QRgba64 *>(image.constScanLine(y))[x];
        return {float(p.red()), float(p.green()), float(p.blue()), float(p.alpha())};
    }
    const auto p = reinterpret_cast<const QRgb *>(image.constScanLine(y))[x];
    return {float(qRed(p)), float(qGreen(p)), float(qBlue(p)), float(qAlpha(p))};
}
Color sample(const QImage &image, double x, double y, bool wide) {
    x = std::clamp(x, 0., double(image.width() - 1));
    y = std::clamp(y, 0., double(image.height() - 1));
    const int ix = int(x), iy = int(y);
    const float fx = float(x - ix), fy = float(y - iy);
    const auto a = read(image, ix, iy, wide), b = read(image, ix + 1, iy, wide), c = read(image, ix, iy + 1, wide),
               d = read(image, ix + 1, iy + 1, wide);
    Color out{};
    for (int k = 0; k < 4; ++k)
        out[k] = (a[k] + (b[k] - a[k]) * fx) * (1 - fy) + (c[k] + (d[k] - c[k]) * fx) * fy;
    return out;
}
void write(QImage &image, int x, int y, Color p, bool wide) {
    const int max = wide ? 65535 : 255;
    int c[4];
    for (int k = 0; k < 4; ++k)
        c[k] = std::clamp(qRound(p[k]), 0, max);
    for (int k = 0; k < 3; ++k)
        c[k] = std::min(c[k], c[3]);
    if (wide)
        reinterpret_cast<QRgba64 *>(image.scanLine(y))[x] = rgba64(c[0], c[1], c[2], c[3]);
    else
        reinterpret_cast<QRgb *>(image.scanLine(y))[x] = qRgba(c[0], c[1], c[2], c[3]);
}
} // namespace
struct RetouchStroke::Impl {
    QImage original, coverage;
    float radius, hardness, strength;
    bool smudge, wide, cpu = false, started = false;
    QVector<QPointF> history;
    std::shared_ptr<GpuRetouchStroke> gpu;
    // Sparse 64x64 field blocks keep an untouched large canvas allocation-free.
    QHash<quint64, std::shared_ptr<std::vector<Offset>>> offsets;
    std::vector<Color> carried;
    int r, side;
    Offset field(int x, int y) const {
        x = std::clamp(x, 0, original.width() - 1);
        y = std::clamp(y, 0, original.height() - 1);
        const auto it = offsets.constFind((quint64(y / 64) << 32) | quint32(x / 64));
        return it == offsets.cend() ? Offset() : (*it.value())[(y % 64) * 64 + x % 64];
    }
    void setField(int x, int y, Offset value) {
        const auto key = (quint64(y / 64) << 32) | quint32(x / 64);
        auto &block = offsets[key];
        if (!block) {
            if (quint64(offsets.size()) * 64 * 64 * sizeof(Offset) > 256ull * 1024 * 1024)
                throw Error("tooLarge");
            block = std::make_shared<std::vector<Offset>>(64 * 64);
        }
        (*block)[(y % 64) * 64 + x % 64] = value;
    }
    float weight(float distance) const {
        const float u = distance / radius;
        if (u >= 1)
            return 0;
        const float t = u <= hardness ? 1.f : (1 - u) / (1 - hardness);
        return t * t * (3 - 2 * t);
    }
    float mask(int x, int y) const {
        if (coverage.isNull())
            return 1;
        return coverage.format() == QImage::Format_Grayscale16
                   ? reinterpret_cast<const quint16 *>(coverage.constScanLine(y))[x] / 65535.f
                   : coverage.constScanLine(y)[x] / 255.f;
    }
    void pickup(const QImage &image, QPointF point) {
        if (!smudge)
            return;
        carried.resize(size_t(side) * side);
        const int cx = qRound(point.x()), cy = qRound(point.y());
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx)
                carried[size_t(dy + r) * side + dx + r] =
                    image.rect().contains(cx + dx, cy + dy) ? read(image, cx + dx, cy + dy, wide) : Color{};
    }
    void dab(QImage &image, QPointF from, QPointF to) {
        if (from == to || strength == 0)
            return;
        const int cx = qRound(to.x()), cy = qRound(to.y());
        const QRect area =
            (smudge ? QRect(cx - r, cy - r, side, side)
                    : QRectF(to - QPointF(radius, radius), QSizeF(radius * 2, radius * 2)).toAlignedRect())
                .intersected(image.rect());
        if (area.isEmpty())
            return;
        const float mx = float(to.x() - from.x()) * strength, my = float(to.y() - from.y()) * strength;
        const int margin = int(std::ceil(std::max(std::abs(mx), std::abs(my)))) + 2;
        const QRect work = area.adjusted(-margin, -margin, margin, margin).intersected(image.rect());
        std::vector<Offset> before;
        if (!smudge) {
            before.resize(size_t(work.width()) * work.height());
            for (int y = work.top(); y <= work.bottom(); ++y)
                for (int x = work.left(); x <= work.right(); ++x)
                    before[size_t(y - work.y()) * work.width() + x - work.x()] = field(x, y);
        }
        auto old = [&](int x, int y) {
            x = std::clamp(x, work.left(), work.right());
            y = std::clamp(y, work.top(), work.bottom());
            return before[size_t(y - work.y()) * work.width() + x - work.x()];
        };
        image.detach();
        for (int y = area.top(); y <= area.bottom(); ++y)
            for (int x = area.left(); x <= area.right(); ++x) {
                const float w = weight(smudge ? std::hypot(float(x - cx), float(y - cy))
                                              : std::hypot(float(x + .5 - to.x()), float(y + .5 - to.y()))) *
                                mask(x, y);
                if (w <= 0)
                    continue;
                if (smudge) {
                    auto &held = carried[size_t(y - cy + r) * side + x - cx + r];
                    const auto under = read(image, x, y, wide);
                    for (int k = 0; k < 4; ++k)
                        held[k] = under[k] + (held[k] - under[k]) * w * strength;
                    write(image, x, y, held, wide);
                } else {
                    const double sx = std::clamp(double(x) - mx * w, 0., double(image.width() - 1));
                    const double sy = std::clamp(double(y) - my * w, 0., double(image.height() - 1));
                    const int ix = int(sx), iy = int(sy);
                    const float fx = float(sx - ix), fy = float(sy - iy);
                    const auto a = old(ix, iy), b = old(ix + 1, iy), c = old(ix, iy + 1), d = old(ix + 1, iy + 1);
                    Offset moved{(a.x + (b.x - a.x) * fx) * (1 - fy) + (c.x + (d.x - c.x) * fx) * fy - mx * w,
                                 (a.y + (b.y - a.y) * fx) * (1 - fy) + (c.y + (d.y - c.y) * fx) * fy - my * w};
                    setField(x, y, moved);
                    write(image, x, y, sample(original, double(x) + moved.x, double(y) + moved.y, wide), wide);
                }
            }
    }
};
RetouchStroke::RetouchStroke(const QImage &source, double radius, double hardness, double strength, bool smudge,
                             const QImage &coverage)
    : impl_(std::make_unique<Impl>()) {
    if (source.isNull() || !std::isfinite(radius) || radius < 1 || radius > MaxSide || !std::isfinite(hardness) ||
        !std::isfinite(strength) || (!coverage.isNull() && coverage.size() != source.size()))
        throw Error("invalidSize");
    auto &s = *impl_;
    s.wide = wideImage(source);
    s.original = source.convertToFormat(pixelFormat(source));
    s.radius = float(radius);
    s.hardness = float(std::clamp(hardness, 0., .98));
    s.strength = float(std::clamp(strength, 0., 1.));
    s.smudge = smudge;
    s.r = int(std::ceil(radius));
    s.side = 2 * s.r + 1;
    if (smudge && quint64(s.side) * s.side > 16ull * 1024 * 1024)
        throw Error("tooLarge");
    if (!coverage.isNull())
        s.coverage = coverage.convertToFormat(maskFormat(s.wide ? 16 : 8));
}
RetouchStroke::~RetouchStroke() = default;
void RetouchStroke::append(QImage &image, const QVector<QPointF> &points) {
    auto &s = *impl_;
    if (points.isEmpty())
        return;
    if (image.size() != s.original.size())
        throw Error("invalidSize");
    if (image.format() != s.original.format())
        image = image.convertToFormat(s.original.format());
    if (image.isNull())
        throw Error("tooLarge");
    for (const auto &p : points)
        if (!std::isfinite(p.x()) || !std::isfinite(p.y()) || std::abs(p.x()) > 4 * MaxSide ||
            std::abs(p.y()) > 4 * MaxSide)
            throw Error("invalidSize");
    if (!s.started) {
        s.history.append(points.front());
        s.started = true;
        s.pickup(image, points.front());
    }
    const qsizetype oldSize = s.history.size();
    for (qsizetype i = 1; i < points.size(); ++i)
        if (points[i] != s.history.back())
            s.history.append(points[i]);
    if (s.history.size() == oldSize)
        return;
    if (!s.cpu && s.radius >= 64) {
        const auto result =
            retouchGpuSegment(s.gpu, s.original, image, points, s.radius, s.hardness, s.strength, s.smudge, s.coverage);
        if (!result.isNull()) {
            image = result;
            return;
        }
    }
    if (!s.cpu) {
        // A lost/over-budget device is replayed from the untouched original.
        // Never continue from a partially updated GPU field or stale brush color.
        s.gpu.reset();
        s.cpu = true;
        image = s.original;
        s.offsets.clear();
        s.pickup(image, s.history.front());
        for (qsizetype i = 1; i < s.history.size(); ++i)
            s.dab(image, s.history[i - 1], s.history[i]);
    } else {
        for (qsizetype i = oldSize; i < s.history.size(); ++i)
            s.dab(image, s.history[i - 1], s.history[i]);
    }
}
} // namespace comp
