#include "PreviewRenderer.h"
#include "GpuPreview.h"
#include "Renderer.h"
#include <algorithm>
#include <cmath>
namespace comp {
namespace {
bool globalAdjustment(const State &s) {
    for (const auto &l : s.layers) {
        const auto kind = l.metadata["adjustment"].toObject()["kind"].toString();
        // These kernels depend on full-image coordinates or a rotated image grid.
        if (kind == "Add Noise" || kind == "Grain" || kind == "Motion Blur") return true;
    }
    return false;
}
int padding(const State &s) {
    int total = 2;
    auto blur = [](double radius) { return radius <= 0 ? 0 : 3 * std::clamp(qRound(radius * .58), 1, 250); };
    for (const auto &l : s.layers) {
        const auto adjustment = l.metadata["adjustment"].toObject();
        if (adjustment["kind"] == "Gaussian Blur") total += blur(adjustment["blurRadius"].toDouble(10));
        const auto effects = l.metadata["effects"].toObject();
        int extent = 0;
        for (auto it = effects.begin(); it != effects.end(); ++it) {
            const auto e = it.value().toObject();
            if (!it.value().isObject() || !e["enabled"].toBool(true)) continue;
            int radius = 0;
            if (it.key() == "shadow" || it.key() == "innerShadow")
                radius = blur(e["blur"].toDouble(it.key() == "shadow" ? 20 : 10)) +
                         int(std::ceil(e["distance"].toDouble(it.key() == "shadow" ? 20 : 10))) + 2;
            else if (it.key() == "outerGlow" || it.key() == "innerGlow") radius = blur(e["size"].toDouble(20)) + 2;
            else if (it.key() == "stroke") radius = int(std::ceil(e["size"].toDouble(4))) + 2;
            extent = std::max(extent, radius);
        }
        total = std::min(MaxSide, total + extent);
    }
    return total;
}
} // namespace
QImage renderPreview(const State &s, QSize target) {
    if (!target.isValid()) target = s.size;
    // Small images are cheaper on the CPU than a GPU upload/readback.
    if (qint64(target.width()) * target.height() >= 65536) {
        auto image = renderGpuPreview(s, target);
        if (!image.isNull()) return image;
    }
    return render(s, target);
}
QImage renderRegion(const State &s, QRect region, bool accelerated) {
    region = region.intersected(QRect(QPoint(), s.size));
    if (region.isEmpty()) return {};
    if (globalAdjustment(s)) return (accelerated ? renderPreview(s) : render(s)).copy(region);
    const int halo = padding(s);
    const QRect work = region.adjusted(-halo, -halo, halo, halo).intersected(QRect(QPoint(), s.size));
    State local = s;
    local.size = work.size();
    for (auto &l : local.layers) {
        l.placement.origin -= work.topLeft();
        l.maskPlacement.origin -= work.topLeft();
    }
    auto result = accelerated ? renderPreview(local) : render(local);
    return result.copy(region.translated(-work.topLeft()));
}
CanvasPreview renderCanvasPreview(const State &s, QRect visible, QSize overviewSize, bool detailed,
                                  quint64 revision, const CanvasPreview &previous,
                                  const std::shared_ptr<std::atomic_bool> &cancelled) {
    CanvasPreview result;
    result.revision = revision;
    if (previous.revision == revision && previous.overview.size() == overviewSize)
        result.overview = previous.overview;
    else result.overview = renderPreview(s, overviewSize);
    if (cancelled->load() || !detailed) return result;
    visible = visible.intersected(QRect(QPoint(), s.size));
    if (visible.isEmpty()) return result;
    constexpr int tileSize = 512;
    QImage full;
    for (int y = visible.top() / tileSize * tileSize; y <= visible.bottom(); y += tileSize)
        for (int x = visible.left() / tileSize * tileSize; x <= visible.right(); x += tileSize) {
            if (cancelled->load()) return result;
            const QRect region = QRect(x, y, tileSize, tileSize).intersected(QRect(QPoint(), s.size));
            QImage image;
            if (previous.revision == revision)
                for (const auto &tile : previous.tiles)
                    if (tile.rectangle == region) { image = tile.image; break; }
            if (image.isNull()) {
                if (globalAdjustment(s)) {
                    if (full.isNull()) full = renderPreview(s);
                    image = full.copy(region);
                } else image = renderRegion(s, region, true);
            }
            result.tiles.append({region, image});
        }
    return result;
}
} // namespace comp
