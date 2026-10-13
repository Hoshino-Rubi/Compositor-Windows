#pragma once
#include "Project.h"
#include "GpuNative.h"
namespace comp {
struct GpuPreviewMetrics {
    quint64 textureAllocations = 0, sourceUploads = 0, uploadedBytes = 0;
    quint64 preparedLayers = 0, dispatches = 0, readbacks = 0, cacheHits = 0, evictions = 0;
    quint64 processingTextureAllocations = 0, processingUploads = 0, processingUploadedBytes = 0;
    quint64 processingReadbackBytes = 0, processingCacheHits = 0;
    // Explicit Map waits reported by the native paint/healing callbacks.
    quint64 nativeReadbackOperations = 0;
    // Pixel counters exclude rounded-up dispatch padding; readback bytes exclude row pitch.
    quint64 compositedPixels = 0, packedPixels = 0, packDispatches = 0, readbackBytes = 0;
    quint64 blurDispatches = 0, blurredPixels = 0, adjustmentDispatches = 0;
    quint64 toneDispatches = 0, tonedPixels = 0, toneTableBuilds = 0, toneTableUploads = 0, toneTableBytes = 0;
    quint64 processingDispatches = 0;
    quint64 textureFrameCopies = 0, noReadbackFrames = 0, externalBytes = 0;
    quint64 residentBytes = 0, budgetBytes = 0, cachedFrames = 0;
};
// A null image requests the reference CPU renderer (unsupported document or unavailable device).
QImage renderGpuPreview(const State &state, QSize target, QPoint viewportOrigin = {});
// Return an immutable GPU snapshot for a native presenter.  The returned owner
// retains the D3D11 texture and shader-resource view; no CPU readback is done.
std::shared_ptr<GpuPreviewTexture> renderGpuTexture(const State &state, QSize target, QPoint viewportOrigin = {});
// Also returns null for scan-order-dependent filters deliberately assigned to the CPU.
QImage filterGpuImage(const QImage &source, const QJsonObject &settings, double scale = 1, int clipping = 0);
QImage effectsGpuImage(const QImage &source, const QJsonObject &settings, double scale = 1);
QImage effectsAccelerated(const QImage &source, const QJsonObject &settings, double scale = 1);
QImage filteredAccelerated(const QImage &source, const QJsonObject &settings, double scale = 1, int clipping = 0);
QString gpuPreviewStatus();
GpuPreviewMetrics gpuPreviewMetrics();
void resetGpuPreviewMetrics();
void clearGpuPreviewCache();
} // namespace comp
