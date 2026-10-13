#pragma once

#include <QImage>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <memory>

#ifdef Q_OS_WIN
// Keep the Windows SDK out of the public Qt headers.  windows.h is included
// only by the implementation; this is the handle declaration used by HWND.
struct HWND__;
#endif

namespace comp {

// HWND is intentionally kept out of the public API on non-Windows builds so
// the CPU/offscreen target can still compile the UI library.  On Windows a
// value must be a live HWND owned by the QWidget (normally QWidget::winId()).
#ifdef Q_OS_WIN
using GpuCanvasWindowHandle = HWND__ *;
#else
using GpuCanvasWindowHandle = void *;
#endif

// A sprite is a borrowed shader-resource view.  The caller must keep the
// texture/view owner alive until present() returns.  sourceRect and targetRect
// are in texture pixels and physical window pixels respectively; an empty
// sourceRect samples the complete texture.  The caller applies DPR before
// filling targetRect, so this presenter never mixes logical and physical
// coordinates.
struct GpuCanvasSprite {
    void *shaderResourceView = nullptr; // ID3D11ShaderResourceView* on Windows
    QSize textureSize;
    QRectF sourceRect;
    QRectF targetRect;
    bool smooth = true;
};

enum class GpuCanvasPresentStatus {
    Idle,
    Presented,
    PresentWouldBlock, // DXGI_ERROR_WAS_STILL_DRAWING: keep the displayed frame
    InvalidArguments,
    Unsupported,
    BudgetExceeded,
    DeviceLost,
    Failed,
};

struct GpuCanvasPresentInfo {
    GpuCanvasPresentStatus status = GpuCanvasPresentStatus::Idle;
    qint64 nativeCode = 0;
    QSize physicalSize;
    quint64 underlayUploadBytes = 0;
    quint64 overlayUploadBytes = 0;
    bool sourceReadback = false;
    bool sourceUpload = false;

    // PresentWouldBlock is deliberately successful from Canvas' point of
    // view.  The back buffer contains the rendered frame and the compositor
    // keeps the previous frame visible; retrying is preferable to falling
    // back to a CPU document readback.
    bool presented() const noexcept {
        return status == GpuCanvasPresentStatus::Presented ||
               status == GpuCanvasPresentStatus::PresentWouldBlock;
    }
    explicit operator bool() const noexcept { return presented(); }
};

const char *gpuCanvasPresentStatusName(GpuCanvasPresentStatus status) noexcept;

class GpuCanvasPresenter final {
  public:
    GpuCanvasPresenter();
    ~GpuCanvasPresenter();

    GpuCanvasPresenter(const GpuCanvasPresenter &) = delete;
    GpuCanvasPresenter &operator=(const GpuCanvasPresenter &) = delete;

    // The bridge calls this while holding GpuPreview's process-wide GPU
    // mutex.  device/context are borrowed for the call and must be the same
    // D3D11 device/context that created every sprite SRV.  physicalSize,
    // underlay and overlay are all physical-pixel sized by the caller (DPR is
    // applied by Canvas).  Underlay/overlay are chrome only: this method never
    // reads document pixels back to the CPU and never uploads a document
    // QImage.
    bool present(void *device, void *context,
                 GpuCanvasWindowHandle window,
                 QSize physicalSize,
                 const QImage &underlay,
                 const QImage &overlay,
                 const QVector<GpuCanvasSprite> &sprites);

    // Test-only diagnostics.  Disabled by default, so production presentation
    // performs no extra copy or readback.  When enabled, present() copies the
    // just-rendered back buffer to a private staging texture before Present;
    // captureRenderedFrameForTesting() then performs the intentional
    // synchronous readback used by GpuDisplayTests.cpp.  Never call this from
    // Canvas or a production paint path.
    void setCaptureForTesting(bool enabled);
    QImage captureRenderedFrameForTesting(void *device, void *context);

    // Release the swap chain and presenter-owned chrome/shader resources.
    // Safe to call before a QWidget/native HWND is destroyed or after device
    // removal.  The next present() lazily rebuilds resources on the supplied
    // device and window.
    void release();

    GpuCanvasPresentInfo lastPresent() const;
    bool isReady() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace comp
