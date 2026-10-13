#include "GpuCanvasPresenter.h"
#include "GpuNative.h"
#include "GpuPreview.h"
#include "Project.h"

#include <QApplication>
#include <QImage>
#include <QTest>
#include <QVector4D>

#include <cmath>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <windows.h>
#endif

using namespace comp;

namespace {

#ifdef Q_OS_WIN

constexpr wchar_t kWindowClass[] = L"CompositorGpuDisplayTestWindow";

LRESULT CALLBACK testWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(window, message, wParam, lParam);
}

class HiddenWindow final {
  public:
    HiddenWindow() {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW existing{};
        existing.cbSize = sizeof(existing);
        if (!GetClassInfoExW(instance, kWindowClass, &existing)) {
            WNDCLASSEXW klass{};
            klass.cbSize = sizeof(klass);
            klass.hInstance = instance;
            klass.lpfnWndProc = testWindowProc;
            klass.lpszClassName = kWindowClass;
            klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            RegisterClassExW(&klass);
        }
        window_ = CreateWindowExW(0, kWindowClass, L"Compositor GPU display test",
                                  WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, instance, nullptr);
        if (window_) ShowWindow(window_, SW_HIDE);
    }

    ~HiddenWindow() {
        if (window_) DestroyWindow(window_);
    }

    HiddenWindow(const HiddenWindow &) = delete;
    HiddenWindow &operator=(const HiddenWindow &) = delete;

    HWND get() const { return window_; }
    explicit operator bool() const { return window_ != nullptr; }

  private:
    HWND window_ = nullptr;
};

State displayState() {
    State state;
    state.id = "GpuDisplayTests";
    state.size = QSize(32, 24);
    Layer layer;
    layer.id = "GpuDisplayLayer";
    layer.name = "Display test layer";
    layer.image = QImage(state.size, QImage::Format_ARGB32_Premultiplied);
    layer.image.fill(QColor(36, 116, 184, 255));
    layer.placement.size = state.size;
    state.layers.append(layer);
    state.active = layer.id;
    return state;
}

QImage solid(QSize size, QColor color) {
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(color);
    return image;
}

void requireDisplayOrSkip(const char *message) {
    if (qEnvironmentVariableIntValue("COMPOSITOR_REQUIRE_GPU"))
        QFAIL(message);
    QSKIP(message);
}

QVector4D nativePixel(ID3D11Device *device, ID3D11DeviceContext *context,
                      ID3D11Texture2D *texture, QPoint point) {
    if (!device || !context || !texture) return {};
    D3D11_TEXTURE2D_DESC sourceDesc{};
    texture->GetDesc(&sourceDesc);
    if (sourceDesc.Format != DXGI_FORMAT_R32G32B32A32_FLOAT ||
        point.x() < 0 || point.y() < 0 || UINT(point.x()) >= sourceDesc.Width ||
        UINT(point.y()) >= sourceDesc.Height)
        return {};
    D3D11_TEXTURE2D_DESC stagingDesc = sourceDesc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging))) return {};
    context->CopyResource(staging.Get(), texture);
    context->Flush();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return {};
    const auto *row = static_cast<const float *>(mapped.pData) +
                      size_t(point.y()) * mapped.RowPitch / sizeof(float);
    const float *pixel = row + size_t(point.x()) * 4;
    const QVector4D result(pixel[0], pixel[1], pixel[2], pixel[3]);
    context->Unmap(staging.Get(), 0);
    return result;
}

#endif

} // namespace

class GpuDisplayTests final : public QObject {
    Q_OBJECT

  private slots:
    void gpuTextureHasNoReadbackAndRemainsImmutable() {
#ifndef Q_OS_WIN
        requireDisplayOrSkip("The native display path is Windows/D3D11 only.");
#else
        clearGpuPreviewCache();
        resetGpuPreviewMetrics();
        auto state = displayState();
        const auto first = renderGpuTexture(state, state.size);
        if (!first || !first->native || !first->native->shaderResourceView)
            requireDisplayOrSkip("Direct3D 11 preview is unavailable on this runner.");

        const auto firstOwner = first->native;
        auto metrics = gpuPreviewMetrics();
        QCOMPARE(metrics.readbacks, quint64(0));
        QVERIFY(metrics.noReadbackFrames > 0);
        QVERIFY(firstOwner->bytes > 0);
        QVERIFY(firstOwner->size == state.size);

        // A second request may be served from the immutable snapshot cache.
        const auto second = renderGpuTexture(state, state.size);
        QVERIFY(second && second->native && second->native->shaderResourceView);
        QCOMPARE(second->native->shaderResourceView.Get(), firstOwner->shaderResourceView.Get());
        QCOMPARE(gpuPreviewMetrics().readbacks, quint64(0));

        // Invalidate the document while retaining the first owner.  The old
        // SRV must remain valid and the new frame must not alias it.
        state.layers[0].image.fill(QColor(196, 58, 72, 255));
        const auto third = renderGpuTexture(state, state.size);
        QVERIFY(third && third->native && third->native->shaderResourceView);
        QVERIFY(third->native->shaderResourceView.Get() != firstOwner->shaderResourceView.Get());
        QVERIFY(firstOwner->shaderResourceView.Get() != nullptr);
        QCOMPARE(gpuPreviewMetrics().readbacks, quint64(0));
        clearGpuPreviewCache();
#endif
    }

    void hiddenWindowPresenterRendersStagesAndResizes() {
#ifndef Q_OS_WIN
        requireDisplayOrSkip("The native display path is Windows/D3D11 only.");
#else
        HiddenWindow window;
        if (!window) requireDisplayOrSkip("Unable to create a hidden Win32 test window.");
        clearGpuPreviewCache();
        resetGpuPreviewMetrics();
        const auto state = displayState();
        const auto texture = renderGpuTexture(state, state.size);
        if (!texture || !texture->native || !texture->native->shaderResourceView)
            requireDisplayOrSkip("Direct3D 11 preview is unavailable on this runner.");

        GpuCanvasPresenter presenter;
        presenter.setCaptureForTesting(true);
        const QSize firstSize(32, 24);
        const QImage underlay = solid(firstSize, QColor(235, 222, 168, 255));
        QImage overlay = solid(firstSize, Qt::transparent);
        overlay.fill(Qt::transparent);
        // This is chrome-only test data.  The document pixels remain in the
        // SRV sprite and are never converted to a QImage by the presenter.
        for (int y = 2; y < 10; ++y)
            for (int x = 4; x < 16; ++x)
                overlay.setPixelColor(x, y, QColor(236, 62, 74, 128));

        QVector<GpuCanvasSprite> sprites;
        sprites.append({texture->native->shaderResourceView.Get(), texture->size, {},
                        QRectF(8, 6, 16, 12), true});
        bool presented = false;
        GpuCanvasPresentInfo firstInfo;
        QImage firstCapture;
        QVector4D sourcePixel;
        runGpuNative([&](ID3D11Device *device, ID3D11DeviceContext *context, quint64) {
            sourcePixel = nativePixel(device, context, texture->native->texture.Get(), QPoint(12, 9));
            presented = presenter.present(device, context, window.get(), firstSize, underlay,
                                          overlay, sprites);
            firstInfo = presenter.lastPresent();
            firstCapture = presenter.captureRenderedFrameForTesting(device, context);
            GpuNativeResult result;
            result.uploadedBytes = firstInfo.underlayUploadBytes + firstInfo.overlayUploadBytes;
            return result;
        });
        QVERIFY2(presented, qPrintable(QString("first present failed: %1")
                                           .arg(gpuCanvasPresentStatusName(firstInfo.status))));
        QVERIFY(firstInfo.presented());
        QVERIFY(firstInfo.underlayUploadBytes > 0);
        QVERIFY(firstInfo.overlayUploadBytes > 0);
        QCOMPARE(firstInfo.sourceReadback, false);
        QCOMPARE(firstInfo.sourceUpload, false);
        QCOMPARE(firstInfo.physicalSize, firstSize);
        QCOMPARE(gpuPreviewMetrics().readbacks, quint64(0));
        QVERIFY2(!firstCapture.isNull(), "test-only back-buffer capture failed");
        QCOMPARE(firstCapture.size(), firstSize);
        const auto capture = firstCapture.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        const auto pixel = [&](int x, int y) { return capture.pixelColor(x, y); };
        const QColor underlayPixel = pixel(1, 1);
        qInfo().nospace() << "first frame pixels underlay=" << underlayPixel
                          << " document=" << pixel(20, 15)
                          << " overlay=" << pixel(10, 8)
                          << " source=" << sourcePixel
                          << " format=" << capture.format();
        QVERIFY(std::abs(underlayPixel.red() - 235) <= 4);
        QVERIFY(std::abs(underlayPixel.green() - 222) <= 4);
        QVERIFY(std::abs(underlayPixel.blue() - 168) <= 4);
        const QColor documentPixel = pixel(20, 15);
        QVERIFY(std::abs(documentPixel.red() - 36) <= 8);
        QVERIFY(std::abs(documentPixel.green() - 116) <= 8);
        QVERIFY(std::abs(documentPixel.blue() - 184) <= 8);
        const QColor overPixel = pixel(10, 8);
        QVERIFY(overPixel.red() > documentPixel.red());
        QVERIFY(overPixel.green() < documentPixel.green());
        QVERIFY(overPixel.blue() < documentPixel.blue());
        // The sprite occupies only (8,6)-(24,18).  This colored underlay
        // sample catches a regression where a fullscreen-triangle shortcut
        // leaks the document or overlay outside an arbitrary targetRect.
        const QColor outsideSpritePixel = pixel(28, 20);
        QVERIFY(std::abs(outsideSpritePixel.red() - 235) <= 4);
        QVERIFY(std::abs(outsideSpritePixel.green() - 222) <= 4);
        QVERIFY(std::abs(outsideSpritePixel.blue() - 168) <= 4);

        // A different physical size exercises ResizeBuffers and keeps the
        // same native source SRV.  The presenter is still run under the same
        // runGpuNative/global context mutex as the preview producer.
        const QSize resized(48, 36);
        const QImage resizedUnderlay = solid(resized, QColor(18, 31, 52, 255));
        const QImage resizedOverlay = solid(resized, QColor(0, 0, 0, 0));
        sprites[0].targetRect = QRectF(8, 6, 16, 12);
        presented = false;
        GpuCanvasPresentInfo resizedInfo;
        QImage resizedCapture;
        runGpuNative([&](ID3D11Device *device, ID3D11DeviceContext *context, quint64) {
            presented = presenter.present(device, context, window.get(), resized, resizedUnderlay,
                                          resizedOverlay, sprites);
            resizedInfo = presenter.lastPresent();
            resizedCapture = presenter.captureRenderedFrameForTesting(device, context);
            return GpuNativeResult{};
        });
        QVERIFY2(presented, qPrintable(QString("resize present failed: %1")
                                           .arg(gpuCanvasPresentStatusName(resizedInfo.status))));
        QCOMPARE(resizedInfo.physicalSize, resized);
        QVERIFY(resizedInfo.presented());
        QCOMPARE(resizedCapture.size(), resized);
        const auto resizedPixel = resizedCapture.convertToFormat(QImage::Format_RGBA8888_Premultiplied)
                                      .pixelColor(1, 1);
        QVERIFY(std::abs(resizedPixel.red() - 18) <= 4);
        QVERIFY(std::abs(resizedPixel.green() - 31) <= 4);
        QVERIFY(std::abs(resizedPixel.blue() - 52) <= 4);

        // A second size change catches stale RTV bindings and exercises the
        // capture resource resize path once more.
        const QSize secondResize(20, 18);
        const QImage secondUnderlay = solid(secondResize, QColor(74, 28, 96, 255));
        sprites[0].targetRect = QRectF(8, 6, 8, 6);
        presented = false;
        GpuCanvasPresentInfo secondInfo;
        QImage secondCapture;
        runGpuNative([&](ID3D11Device *device, ID3D11DeviceContext *context, quint64) {
            presented = presenter.present(device, context, window.get(), secondResize, secondUnderlay,
                                          {}, sprites);
            secondInfo = presenter.lastPresent();
            secondCapture = presenter.captureRenderedFrameForTesting(device, context);
            return GpuNativeResult{};
        });
        QVERIFY2(presented, qPrintable(QString("second resize present failed: %1")
                                            .arg(gpuCanvasPresentStatusName(secondInfo.status))));
        QCOMPARE(secondInfo.physicalSize, secondResize);
        QCOMPARE(secondCapture.size(), secondResize);
        const auto secondPixel = secondCapture.convertToFormat(QImage::Format_RGBA8888_Premultiplied)
                                      .pixelColor(1, 1);
        QVERIFY(std::abs(secondPixel.red() - 74) <= 4);
        QVERIFY(std::abs(secondPixel.green() - 28) <= 4);
        QVERIFY(std::abs(secondPixel.blue() - 96) <= 4);
        QVERIFY(presenter.isReady());
        presenter.setCaptureForTesting(false);
        presenter.release();
        QVERIFY(!presenter.isReady());
        clearGpuPreviewCache();
#endif
    }

    void presenterRejectsInvalidDeviceAndWindow() {
#ifndef Q_OS_WIN
        requireDisplayOrSkip("The native display path is Windows/D3D11 only.");
#else
        GpuCanvasPresenter presenter;
        const bool nullDevice = presenter.present(nullptr, nullptr, nullptr, QSize(16, 16), {}, {}, {});
        QVERIFY(!nullDevice);
        QCOMPARE(presenter.lastPresent().status, GpuCanvasPresentStatus::InvalidArguments);

        HiddenWindow window;
        if (!window) requireDisplayOrSkip("Unable to create a hidden Win32 test window.");
        const bool nullSize = presenter.present(nullptr, nullptr, window.get(), QSize(), {}, {}, {});
        QVERIFY(!nullSize);
        QCOMPARE(presenter.lastPresent().status, GpuCanvasPresentStatus::InvalidArguments);

        bool callbackCalled = false;
        bool invalidWindow = true;
        GpuCanvasPresentInfo invalidWindowInfo;
        runGpuNative([&](ID3D11Device *device, ID3D11DeviceContext *context, quint64) {
            callbackCalled = true;
            invalidWindow = presenter.present(device, context, reinterpret_cast<HWND>(quintptr(1)),
                                              QSize(16, 16), {}, {}, {});
            invalidWindowInfo = presenter.lastPresent();
            return GpuNativeResult{};
        });
        if (!callbackCalled) requireDisplayOrSkip("Direct3D 11 preview is unavailable on this runner.");
        QVERIFY(!invalidWindow);
        QCOMPARE(invalidWindowInfo.status, GpuCanvasPresentStatus::InvalidArguments);
#endif
    }
};

QTEST_MAIN(GpuDisplayTests)
#include "GpuDisplayTests.moc"
