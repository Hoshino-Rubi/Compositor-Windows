#pragma once

#include "GpuNative.h"
#include "Project.h"

#include <memory>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#endif

namespace comp {

// Build one layer's prepared surface directly on the D3D11 device.  The
// caller must already hold the process-wide GPU mutex (renderGpuPreview and
// runGpuNative both serialize the immediate context); this function never
// acquires it and never performs a readback.  `includeAncestors` applies
// ancestor masks and the product of ancestor/folder opacities.  The root
// layer's opacity can be omitted for the live-effects path, which applies it
// after the effect kernel just like the CPU renderer.
#ifdef Q_OS_WIN
std::shared_ptr<GpuNativeTexture> renderGpuLayerSurface(
    ID3D11Device *device, ID3D11DeviceContext *context, const State &state, int index, QSize target,
    bool includeAncestors, bool includeLayerOpacity, quint64 budget, GpuNativeResult &stats,
    const std::shared_ptr<GpuNativeTexture> &rootSourceOverride = {}, bool rootOpacityAsCoverage = false);
#else
std::shared_ptr<GpuNativeTexture> renderGpuLayerSurface(
    void *device, void *context, const State &state, int index, QSize target,
    bool includeAncestors, bool includeLayerOpacity, quint64 budget, GpuNativeResult &stats,
    const std::shared_ptr<GpuNativeTexture> &rootSourceOverride = {}, bool rootOpacityAsCoverage = false);
#endif

} // namespace comp
