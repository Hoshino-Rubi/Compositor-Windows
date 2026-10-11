#pragma once
#include "Project.h"
namespace comp {
struct GpuPreviewMetrics {
    quint64 textureAllocations = 0, sourceUploads = 0, uploadedBytes = 0;
    quint64 preparedLayers = 0, dispatches = 0, readbacks = 0, cacheHits = 0, evictions = 0;
    // Pixel counters exclude rounded-up dispatch padding; readback bytes exclude row pitch.
    quint64 compositedPixels = 0, packedPixels = 0, packDispatches = 0, readbackBytes = 0;
    quint64 residentBytes = 0, budgetBytes = 0, cachedFrames = 0;
};
// A null image requests the reference CPU renderer (unsupported document or unavailable device).
QImage renderGpuPreview(const State &state, QSize target, QPoint viewportOrigin = {});
QString gpuPreviewStatus();
GpuPreviewMetrics gpuPreviewMetrics();
void resetGpuPreviewMetrics();
void clearGpuPreviewCache();
} // namespace comp
