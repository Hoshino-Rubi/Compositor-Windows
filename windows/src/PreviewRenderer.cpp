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
        if (QStringList{"Add Noise","Grain","Motion Blur","Camera Raw","Vignette","Bloom / Glow","Dither","Tonal Contrast","Lens Correction","Scanlines"}.contains(kind)) return true;
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
bool cancelled(const std::shared_ptr<std::atomic_bool> &flag) {
    return flag && flag->load();
}
QSize textureSize(const std::shared_ptr<GpuPreviewTexture> &texture) {
    if (!texture) return {};
    if (texture->size.isValid()) return texture->size;
    return texture->native ? texture->native->size : QSize();
}
bool usableTexture(const std::shared_ptr<GpuPreviewTexture> &texture) {
    return texture && !texture->isNull() && textureSize(texture).isValid();
}
bool validTextureSource(const std::shared_ptr<GpuPreviewTexture> &texture, const QRect &source,
                        const QRect &region) {
    if (!usableTexture(texture) || source.isEmpty() || source.size() != region.size()) return false;
    const QRect bounds(QPoint(), textureSize(texture));
    return bounds.contains(source.topLeft()) && bounds.contains(source.bottomRight());
}
} // namespace
QImage renderPreview(const State &s, QSize target, QPoint viewportOrigin) {
    if (!target.isValid()) target = s.size;
    // Small images are cheaper on the CPU than a GPU upload/readback.
    if (qint64(target.width()) * target.height() >= 65536) {
        auto image = renderGpuPreview(s, target, viewportOrigin);
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
    auto result = accelerated ? renderPreview(local, {}, work.topLeft()) : render(local);
    return result.copy(region.translated(-work.topLeft()));
}
CanvasPreview renderCanvasPreviewCpu(const State &s, QRect visible, QSize overviewSize, bool detailed,
                                     quint64 revision, const CanvasPreview &previous,
                                     const std::shared_ptr<std::atomic_bool> &cancelledFlag,
                                     bool referenceOnly = false) {
    CanvasPreview result;
    result.revision = revision;
    if (previous.revision == revision && previous.overview.size() == overviewSize)
        result.overview = previous.overview;
    else result.overview = referenceOnly ? render(s, overviewSize) : renderPreview(s, overviewSize);
    if (cancelled(cancelledFlag) || !detailed) return result;
    visible = visible.intersected(QRect(QPoint(), s.size));
    if (visible.isEmpty()) return result;
    constexpr int tileSize = 512;
    QImage full;
    for (int y = visible.top() / tileSize * tileSize; y <= visible.bottom(); y += tileSize)
        for (int x = visible.left() / tileSize * tileSize; x <= visible.right(); x += tileSize) {
            if (cancelled(cancelledFlag)) return result;
            const QRect region = QRect(x, y, tileSize, tileSize).intersected(QRect(QPoint(), s.size));
            QImage image;
            if (previous.revision == revision)
                for (const auto &tile : previous.tiles)
                    if (tile.rectangle == region) { image = tile.image; break; }
            if (image.isNull()) {
                if (globalAdjustment(s)) {
                    if (full.isNull()) full = referenceOnly ? render(s) : renderPreview(s);
                    image = full.copy(region);
                } else image = renderRegion(s, region, !referenceOnly);
            }
            result.tiles.append({region, image});
        }
    return result;
}

namespace {
std::shared_ptr<GpuPreviewTexture> renderRegionTexture(const State &state, const QRect &region,
                                                       QRect *textureSource) {
    const int halo = padding(state);
    const QRect canvas(QPoint(), state.size);
    const QRect work = region.adjusted(-halo, -halo, halo, halo).intersected(canvas);
    if (work.isEmpty()) return {};
    State local = state;
    local.size = work.size();
    for (auto &layer : local.layers) {
        layer.placement.origin -= work.topLeft();
        layer.maskPlacement.origin -= work.topLeft();
    }
    auto texture = renderGpuTexture(local, work.size(), work.topLeft());
    if (!usableTexture(texture)) return {};
    const QRect source = region.translated(-work.topLeft());
    if (!validTextureSource(texture, source, region)) return {};
    if (textureSource) *textureSource = source;
    return texture;
}
} // namespace

CanvasPreview renderCanvasPreview(const State &s, QRect visible, QSize overviewSize, bool detailed,
                                  quint64 revision, const CanvasPreview &previous,
                                  const std::shared_ptr<std::atomic_bool> &cancelledFlag,
                                  bool preferGpuTexture) {
    if (!preferGpuTexture)
        return renderCanvasPreviewCpu(s, visible, overviewSize, detailed, revision, previous, cancelledFlag);

    // Keep the native path texture-only.  If any requested texture cannot be
    // produced (unsupported document, unavailable device, or budget), return
    // one complete CPU frame rather than mixing a GPU frame with readback tiles.
    CanvasPreview result;
    result.revision = revision;
    const bool sameRevision = previous.revision == revision;
    const bool previousOverviewSize = previous.overview.size() == overviewSize ||
                                      textureSize(previous.overviewTexture) == overviewSize;
    auto overviewTexture = (sameRevision && previousOverviewSize && usableTexture(previous.overviewTexture))
                               ? previous.overviewTexture
                               : std::shared_ptr<GpuPreviewTexture>();
    if (!overviewTexture)
        overviewTexture = renderGpuTexture(s, overviewSize);
    if (!usableTexture(overviewTexture))
        return renderCanvasPreviewCpu(s, visible, overviewSize, detailed, revision, previous, cancelledFlag, true);
    result.overviewTexture = overviewTexture;
    if (cancelled(cancelledFlag) || !detailed) return result;

    visible = visible.intersected(QRect(QPoint(), s.size));
    if (visible.isEmpty()) return result;
    constexpr int tileSize = 512;
    const bool fullFrame = globalAdjustment(s);
    std::shared_ptr<GpuPreviewTexture> fullTexture;
    if (fullFrame && overviewSize == s.size)
        fullTexture = overviewTexture;
    else if (fullFrame && sameRevision) {
        // Global-adjustment tiles all sample one full-canvas texture.  Reuse
        // any retained tile owner before asking the bridge for another frame.
        for (const auto &tile : previous.tiles) {
            if (usableTexture(tile.texture) && textureSize(tile.texture) == s.size) {
                fullTexture = tile.texture;
                break;
            }
        }
    }
    if (fullFrame && !fullTexture)
        fullTexture = renderGpuTexture(s, s.size);
    if (fullFrame && !usableTexture(fullTexture))
        return renderCanvasPreviewCpu(s, visible, overviewSize, detailed, revision, previous, cancelledFlag, true);

    for (int y = visible.top() / tileSize * tileSize; y <= visible.bottom(); y += tileSize)
        for (int x = visible.left() / tileSize * tileSize; x <= visible.right(); x += tileSize) {
            if (cancelled(cancelledFlag)) return result;
            const QRect region = QRect(x, y, tileSize, tileSize).intersected(QRect(QPoint(), s.size));
            std::shared_ptr<GpuPreviewTexture> texture;
            QRect source;
            if (sameRevision) {
                for (const auto &tile : previous.tiles) {
                    if (tile.rectangle != region ||
                        !validTextureSource(tile.texture, tile.textureSource, region)) continue;
                    texture = tile.texture;
                    source = tile.textureSource;
                    break;
                }
            }
            if (!usableTexture(texture)) {
                if (fullFrame) {
                    texture = fullTexture;
                    source = region;
                } else {
                    texture = renderRegionTexture(s, region, &source);
                }
            }
            if (!validTextureSource(texture, source, region)) {
                // A texture allocation can fail after the overview succeeded;
                // use the reference renderer for the whole frame in that case.
                return renderCanvasPreviewCpu(s, visible, overviewSize, detailed, revision, previous, cancelledFlag, true);
            }
            result.tiles.append({region, {}, texture, source});
        }
    return result;
}
} // namespace comp
