#pragma once

#include <QImage>
#include <QSize>
#include <QtGlobal>
#include <functional>
#include <memory>

#ifdef Q_OS_WIN
#pragma push_macro("NOMINMAX")
#pragma push_macro("interface")
#pragma push_macro("small")
#pragma push_macro("near")
#pragma push_macro("far")
#pragma push_macro("min")
#pragma push_macro("max")
#ifdef NOMINMAX
#undef NOMINMAX
#endif
#ifdef interface
#undef interface
#endif
#ifdef small
#undef small
#endif
#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#pragma pop_macro("max")
#pragma pop_macro("min")
#pragma pop_macro("far")
#pragma pop_macro("near")
#pragma pop_macro("small")
#pragma pop_macro("interface")
#pragma pop_macro("NOMINMAX")
#endif

namespace comp {

// Result returned by native GPU callbacks.  The image is optional: texture-only
// callers leave it null and report no readback bytes.
struct GpuNativeResult {
    QImage image;
    quint64 dispatches = 0;
    quint64 uploadedBytes = 0;
    quint64 readbackBytes = 0;
    quint64 readbackOperations = 0; // Explicit CPU Map waits, not returned image count.
};

#ifdef Q_OS_WIN
using GpuNativeCallback = std::function<GpuNativeResult(ID3D11Device *, ID3D11DeviceContext *, quint64)>;

// The resource and view are deliberately kept together.  A presenter may use
// shaderResourceView directly while this owner keeps both COM objects alive.
struct GpuNativeTexture {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> shaderResourceView;
    QSize size;
    int depth = 8;
    quint64 bytes = 0;
};
#else
using GpuNativeCallback = std::function<GpuNativeResult(void *, void *, quint64)>;

struct GpuNativeTexture {
    QSize size;
    int depth = 8;
    quint64 bytes = 0;
};
#endif

// An immutable GPU snapshot.  The shared owner is never mutated by the
// preview cache after publication, so a presenter can retain it for a frame.
struct GpuPreviewTexture {
    std::shared_ptr<GpuNativeTexture> native;
    QSize size;
    int depth = 8;

    bool isNull() const noexcept { return !native; }
    explicit operator bool() const noexcept { return !isNull(); }
};

// Register a weak reservation while already inside runGpuNative (no nested lock).
void retainGpuNativeAllocation(const std::shared_ptr<GpuNativeTexture> &allocation);

// Execute a native operation while holding the process-wide D3D11 context
// mutex.  On non-Windows builds this is a no-op fallback returning a null image.
QImage runGpuNative(const GpuNativeCallback &callback);

} // namespace comp
