#pragma once
#include "Project.h"
#include "GpuNative.h"
#include <atomic>
#include <memory>
namespace comp {
// A tile may be backed by either a CPU image or an immutable native texture.
// textureSource is expressed in texture pixels; it identifies the sub-rectangle
// that corresponds to rectangle when a tile was rendered from a halo or a full
// canvas texture.  An empty source means that no native texture is published.
struct PreviewTile {
    QRect rectangle;
    QImage image;
    std::shared_ptr<GpuPreviewTexture> texture;
    QRect textureSource;
};
struct CanvasPreview {
    QImage overview;
    std::shared_ptr<GpuPreviewTexture> overviewTexture;
    QVector<PreviewTile> tiles;
    quint64 revision = 0;
    bool referenceOnly = false;
    QString documentId;
    QSize canvasSize;
    // Retain the global-adjustment composite across pans of the same revision.
    QImage fullReference;
};
QImage renderPreview(const State &state, QSize target = {}, QPoint viewportOrigin = {});
QImage renderRegion(const State &state, QRect region, bool accelerated = false);
CanvasPreview renderCanvasPreview(const State &state, QRect visible, QSize overviewSize, bool detailed,
                                  quint64 revision, const CanvasPreview &previous,
                                  const std::shared_ptr<std::atomic_bool> &cancelled,
                                  bool preferGpuTexture = false, bool referenceOnly = false);
} // namespace comp
