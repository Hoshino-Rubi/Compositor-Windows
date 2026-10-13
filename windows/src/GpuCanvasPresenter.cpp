#include "GpuCanvasPresenter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#ifdef Q_OS_WIN

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

namespace comp {
namespace {

using Microsoft::WRL::ComPtr;

// The presenter deliberately uses a tiny vertex/pixel pair instead of a
// framework paint engine.  A sprite is sampled directly from the SRV supplied
// by GpuPreview and drawn into the swap-chain back buffer.  No staging texture
// or map/readback path exists in this file.
constexpr char kVertexShader[] = R"hlsl(
cbuffer Frame : register(b0) {
    float4 sourceUv;
    float4 targetPx;
    float2 outputSize;
    float opacity;
    float padding;
};
struct VSOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};
VSOut main(uint id : SV_VertexID) {
    // Two ordinary triangles keep each sprite inside targetPx.  The previous
    // full-viewport triangle trick used corners (0,0), (2,0), (0,2); that
    // only works when a scissor is present and otherwise expands every
    // arbitrary sprite rectangle while sampling only half its source.
    float2 corner = id == 0 ? float2(0, 0) :
                    (id == 1 ? float2(1, 0) :
                     (id == 2 ? float2(0, 1) :
                      (id == 3 ? float2(0, 1) :
                       (id == 4 ? float2(1, 0) : float2(1, 1)))));
    float2 px = targetPx.xy + corner * targetPx.zw;
    VSOut result;
    result.position = float4(px.x / outputSize.x * 2 - 1,
                             1 - px.y / outputSize.y * 2, 0, 1);
    result.uv = sourceUv.xy + sourceUv.zw * corner;
    return result;
}
)hlsl";

constexpr char kPixelShader[] = R"hlsl(
cbuffer Frame : register(b0) {
    float4 sourceUv;
    float4 targetPx;
    float2 outputSize;
    float opacity;
    float padding;
};
Texture2D sourceTexture : register(t0);
SamplerState sourceSampler : register(s0);
struct VSOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};
float4 main(VSOut input) : SV_Target {
    return sourceTexture.Sample(sourceSampler, input.uv) * opacity;
}
)hlsl";

struct FrameConstants {
    float sourceUv[4];
    float targetPx[4];
    float outputSize[2];
    float opacity;
    float padding;
};
static_assert(sizeof(FrameConstants) == 48);

bool isDeviceLost(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
           hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR || hr == DXGI_ERROR_DEVICE_HUNG;
}

bool checkedAdd(quint64 &total, quint64 value) {
    if (value > std::numeric_limits<quint64>::max() - total) return false;
    total += value;
    return true;
}

quint64 imageBytes(const QImage &image) {
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) return 0;
    return quint64(image.width()) * quint64(image.height()) * 4;
}

} // namespace

struct GpuCanvasPresenter::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<ID3D11RenderTargetView> renderTarget;

    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> pixelShader;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> linearSampler;
    ComPtr<ID3D11SamplerState> pointSampler;
    ComPtr<ID3D11BlendState> blendState;
    ComPtr<ID3D11RasterizerState> rasterizerState;

    ComPtr<ID3D11Texture2D> underlayTexture;
    ComPtr<ID3D11ShaderResourceView> underlayView;
    ComPtr<ID3D11Texture2D> overlayTexture;
    ComPtr<ID3D11ShaderResourceView> overlayView;
    // These resources are allocated only when a test explicitly enables the
    // capture hook.  They are not part of the production presentation path.
    ComPtr<ID3D11Texture2D> captureTexture;
    ComPtr<ID3D11Texture2D> captureStaging;
    QSize captureSize;
    DXGI_FORMAT captureFormat = DXGI_FORMAT_UNKNOWN;
    bool captureEnabled = false;
    QSize underlaySize;
    QSize overlaySize;
    qint64 underlayKey = 0;
    qint64 overlayKey = 0;

    GpuCanvasWindowHandle window = nullptr;
    QSize physicalSize;
    GpuCanvasPresentInfo last;
    bool deviceLost = false;

    void resetOwned() {
        // The RTV must be released before ResizeBuffers or device teardown.
        renderTarget.Reset();
        swapChain.Reset();
        underlayView.Reset();
        underlayTexture.Reset();
        overlayView.Reset();
        overlayTexture.Reset();
        captureStaging.Reset();
        captureTexture.Reset();
        captureSize = {};
        captureFormat = DXGI_FORMAT_UNKNOWN;
        vertexShader.Reset();
        pixelShader.Reset();
        constants.Reset();
        linearSampler.Reset();
        pointSampler.Reset();
        blendState.Reset();
        rasterizerState.Reset();
        device.Reset();
        context.Reset();
        window = nullptr;
        physicalSize = {};
        underlaySize = {};
        overlaySize = {};
        underlayKey = 0;
        overlayKey = 0;
        deviceLost = false;
    }

    void releaseSwapChainOnly() {
        renderTarget.Reset();
        swapChain.Reset();
        window = nullptr;
        physicalSize = {};
    }

    GpuCanvasPresentInfo failure(GpuCanvasPresentStatus status, HRESULT hr,
                                 const QSize &size, const QString &detail = {}) {
        last = {};
        last.status = status;
        last.nativeCode = qint64(hr);
        last.physicalSize = size;
        last.sourceReadback = false;
        last.sourceUpload = false;
        Q_UNUSED(detail); // Kept in the local call sites for debugger clarity.
        return last;
    }

    bool ensureDevice(void *rawDevice, void *rawContext, const QSize &size) {
        auto *newDevice = static_cast<ID3D11Device *>(rawDevice);
        auto *newContext = static_cast<ID3D11DeviceContext *>(rawContext);
        if (!newDevice || !newContext) return false;
        if (device.Get() != newDevice || context.Get() != newContext) {
            resetOwned();
            device = newDevice; // ComPtr assignment AddRefs borrowed inputs.
            context = newContext;
        }
        if (FAILED(device->GetDeviceRemovedReason())) {
            resetOwned();
            deviceLost = true;
            return false;
        }
        Q_UNUSED(size);
        return true;
    }

    bool budgetAllows(const QSize &size, const QImage &underlay, const QImage &overlay) {
        quint64 estimate = 0;
        const quint64 pixels = quint64(size.width()) * quint64(size.height());
        if (pixels > std::numeric_limits<quint64>::max() / 8 ||
            !checkedAdd(estimate, pixels * 8) || !checkedAdd(estimate, imageBytes(underlay)) ||
            !checkedAdd(estimate, imageBytes(overlay)))
            return false;

        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIAdapter3> adapter3;
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (FAILED(device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
            FAILED(adapter.As(&adapter3)) ||
            FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
            return true; // WARP/older drivers may not expose a budget query.
        if (info.Budget == 0) return true;
        return info.CurrentUsage <= info.Budget && estimate <= info.Budget - info.CurrentUsage;
    }

    bool compileShaders() {
        if (vertexShader && pixelShader && constants && linearSampler && pointSampler && blendState &&
            rasterizerState)
            return true;

        ComPtr<ID3DBlob> vsCode, psCode, errors;
        HRESULT hr = D3DCompile(kVertexShader, sizeof(kVertexShader) - 1, "GpuCanvasPresenterVS",
                                nullptr, nullptr, "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                0, &vsCode, &errors);
        if (FAILED(hr)) return false;
        errors.Reset();
        hr = D3DCompile(kPixelShader, sizeof(kPixelShader) - 1, "GpuCanvasPresenterPS", nullptr,
                        nullptr, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psCode,
                        &errors);
        if (FAILED(hr)) return false;
        hr = device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr,
                                        &vertexShader);
        if (FAILED(hr)) return false;
        hr = device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr,
                                       &pixelShader);
        if (FAILED(hr)) return false;

        D3D11_BUFFER_DESC bufferDesc{};
        bufferDesc.ByteWidth = sizeof(FrameConstants);
        bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
        bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&bufferDesc, nullptr, &constants))) return false;

        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MinLOD = 0;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device->CreateSamplerState(&samplerDesc, &linearSampler))) return false;
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        if (FAILED(device->CreateSamplerState(&samplerDesc, &pointSampler))) return false;

        D3D11_BLEND_DESC blendDesc{};
        blendDesc.RenderTarget[0].BlendEnable = TRUE;
        blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device->CreateBlendState(&blendDesc, &blendState))) return false;

        D3D11_RASTERIZER_DESC rasterDesc{};
        rasterDesc.FillMode = D3D11_FILL_SOLID;
        rasterDesc.CullMode = D3D11_CULL_NONE;
        rasterDesc.DepthClipEnable = TRUE;
        if (FAILED(device->CreateRasterizerState(&rasterDesc, &rasterizerState))) return false;
        return true;
    }

    bool createRenderTarget() {
        if (!swapChain) return false;
        ComPtr<ID3D11Texture2D> backBuffer;
        if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) return false;
        return SUCCEEDED(device->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTarget));
    }

    bool ensureSwapChain(HWND hwnd, const QSize &size) {
        if (swapChain && window == hwnd && physicalSize == size && renderTarget) return true;
        if (swapChain && window == hwnd) {
            clearBindings();
            renderTarget.Reset();
            const HRESULT hr = swapChain->ResizeBuffers(0, UINT(size.width()), UINT(size.height()),
                                                        DXGI_FORMAT_UNKNOWN, 0);
            if (FAILED(hr)) {
                if (isDeviceLost(hr)) {
                    resetOwned();
                    deviceLost = true;
                }
                return false;
            }
            physicalSize = size;
            return createRenderTarget();
        }

        clearBindings();
        releaseSwapChainOnly();
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
            FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
            return false;

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = UINT(size.width());
        desc.Height = UINT(size.height());
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        HRESULT hr = factory->CreateSwapChainForHwnd(device.Get(), hwnd, &desc, nullptr, nullptr,
                                                     &swapChain);
        if (FAILED(hr)) {
            if (isDeviceLost(hr)) deviceLost = true;
            swapChain.Reset();
            return false;
        }
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        window = hwnd;
        physicalSize = size;
        return createRenderTarget();
    }

    bool ensureChromeTexture(const QImage &source, bool underlay, quint64 &uploadedBytes) {
        uploadedBytes = 0;
        if (source.isNull()) return true;
        QImage image = source.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        if (image.isNull() || image.size().isEmpty()) return false;
        auto &texture = underlay ? underlayTexture : overlayTexture;
        auto &view = underlay ? underlayView : overlayView;
        auto &storedSize = underlay ? underlaySize : overlaySize;
        auto &storedKey = underlay ? underlayKey : overlayKey;
        if (!texture || storedSize != image.size()) {
            texture.Reset();
            view.Reset();
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = UINT(image.width());
            desc.Height = UINT(image.height());
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA data{};
            data.pSysMem = image.constBits();
            data.SysMemPitch = UINT(image.bytesPerLine());
            if (FAILED(device->CreateTexture2D(&desc, &data, &texture)) ||
                FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &view))) {
                texture.Reset();
                view.Reset();
                return false;
            }
            storedSize = image.size();
            storedKey = image.cacheKey();
            uploadedBytes = quint64(image.bytesPerLine()) * quint64(image.height());
            return true;
        }
        if (storedKey != image.cacheKey()) {
            context->UpdateSubresource(texture.Get(), 0, nullptr, image.constBits(),
                                       UINT(image.bytesPerLine()), 0);
            storedKey = image.cacheKey();
            uploadedBytes = quint64(image.bytesPerLine()) * quint64(image.height());
        }
        return true;
    }

    bool ensureCaptureResources(ID3D11Texture2D *backBuffer) {
        if (!captureEnabled || !backBuffer) return true;
        D3D11_TEXTURE2D_DESC desc{};
        backBuffer->GetDesc(&desc);
        if (captureTexture && captureStaging && captureSize == QSize(int(desc.Width), int(desc.Height)) &&
            captureFormat == desc.Format)
            return true;
        captureTexture.Reset();
        captureStaging.Reset();
        captureSize = {};
        captureFormat = DXGI_FORMAT_UNKNOWN;
        D3D11_TEXTURE2D_DESC copyDesc = desc;
        copyDesc.Usage = D3D11_USAGE_DEFAULT;
        copyDesc.BindFlags = 0;
        copyDesc.CPUAccessFlags = 0;
        copyDesc.MiscFlags = 0;
        if (FAILED(device->CreateTexture2D(&copyDesc, nullptr, &captureTexture))) return false;
        D3D11_TEXTURE2D_DESC stagingDesc = copyDesc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &captureStaging))) {
            captureTexture.Reset();
            return false;
        }
        captureSize = QSize(int(desc.Width), int(desc.Height));
        captureFormat = desc.Format;
        return true;
    }

    void captureBeforePresent() {
        if (!captureEnabled || !swapChain) return;
        ComPtr<ID3D11Texture2D> backBuffer;
        if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) return;
        if (!ensureCaptureResources(backBuffer.Get())) return;
        context->CopyResource(captureTexture.Get(), backBuffer.Get());
        // The staging copy is intentionally deferred until the test calls
        // captureRenderedFrameForTesting(), after Present has returned.
        context->CopyResource(captureStaging.Get(), captureTexture.Get());
    }

    QImage capture(void *rawDevice, void *rawContext) {
        auto *requestedDevice = static_cast<ID3D11Device *>(rawDevice);
        auto *requestedContext = static_cast<ID3D11DeviceContext *>(rawContext);
        if (!captureEnabled || !captureStaging || requestedDevice != device.Get() ||
            requestedContext != context.Get() || captureSize.isEmpty())
            return {};
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(captureStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return {};
        QImage result(captureSize, QImage::Format_ARGB32_Premultiplied);
        if (result.isNull()) {
            context->Unmap(captureStaging.Get(), 0);
            return {};
        }
        const int rowBytes = captureSize.width() * 4;
        for (int y = 0; y < captureSize.height(); ++y)
            std::memcpy(result.scanLine(y), static_cast<const char *>(mapped.pData) + y * mapped.RowPitch,
                        size_t(rowBytes));
        context->Unmap(captureStaging.Get(), 0);
        return result;
    }

    bool updateConstants(const QRectF &source, const QRectF &target, const QSize &textureSize,
                         float opacity) {
        if (!constants || textureSize.width() <= 0 || textureSize.height() <= 0 ||
            physicalSize.width() <= 0 || physicalSize.height() <= 0)
            return false;
        FrameConstants values{};
        values.sourceUv[0] = float(source.x() / textureSize.width());
        values.sourceUv[1] = float(source.y() / textureSize.height());
        values.sourceUv[2] = float(source.width() / textureSize.width());
        values.sourceUv[3] = float(source.height() / textureSize.height());
        values.targetPx[0] = float(target.x());
        values.targetPx[1] = float(target.y());
        values.targetPx[2] = float(target.width());
        values.targetPx[3] = float(target.height());
        values.outputSize[0] = float(physicalSize.width());
        values.outputSize[1] = float(physicalSize.height());
        values.opacity = std::clamp(opacity, 0.f, 1.f);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
        std::memcpy(mapped.pData, &values, sizeof(values));
        context->Unmap(constants.Get(), 0);
        ID3D11Buffer *buffer = constants.Get();
        context->VSSetConstantBuffers(0, 1, &buffer);
        context->PSSetConstantBuffers(0, 1, &buffer);
        return true;
    }

    bool spriteSizeAndDevice(ID3D11ShaderResourceView *view, QSize requested, QSize &size) {
        if (!view) return false;
        ComPtr<ID3D11Device> sourceDevice;
        view->GetDevice(&sourceDevice);
        if (!sourceDevice || sourceDevice.Get() != device.Get()) return false;
        size = requested;
        if (size.width() > 0 && size.height() > 0) return true;
        ComPtr<ID3D11Resource> resource;
        view->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        if (!resource || FAILED(resource.As(&texture))) return false;
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        size = QSize(int(desc.Width), int(desc.Height));
        return size.isValid();
    }

    bool drawTexture(ID3D11ShaderResourceView *view, QSize textureSize, QRectF source,
                     const QRectF &target, bool smooth) {
        if (!view || target.isEmpty()) return true;
        if (!spriteSizeAndDevice(view, textureSize, textureSize)) return false;
        const QRectF bounds(QPointF(0, 0), QSizeF(textureSize));
        if (source.isEmpty()) source = bounds;
        source = source.intersected(bounds);
        if (source.isEmpty()) return false;
        if (!updateConstants(source, target, textureSize, 1.f)) return false;
        ID3D11ShaderResourceView *resource = view;
        ID3D11SamplerState *sampler = smooth ? linearSampler.Get() : pointSampler.Get();
        context->PSSetShaderResources(0, 1, &resource);
        context->PSSetSamplers(0, 1, &sampler);
        context->Draw(6, 0);
        ID3D11ShaderResourceView *nullView = nullptr;
        context->PSSetShaderResources(0, 1, &nullView);
        return true;
    }

    void clearBindings() {
        ID3D11ShaderResourceView *nullViews[8]{};
        ID3D11UnorderedAccessView *nullUavs[8]{};
        context->PSSetShaderResources(0, 8, nullViews);
        context->CSSetShaderResources(0, 8, nullViews);
        context->CSSetUnorderedAccessViews(0, 8, nullUavs, nullptr);
        ID3D11Buffer *nullBuffer = nullptr;
        context->CSSetConstantBuffers(0, 1, &nullBuffer);
        context->CSSetShader(nullptr, nullptr, 0);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->VSSetShader(nullptr, nullptr, 0);
        context->PSSetShader(nullptr, nullptr, 0);
        context->IASetInputLayout(nullptr);
        context->RSSetState(nullptr);
        context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
    }

    GpuCanvasPresentInfo present(void *rawDevice, void *rawContext, HWND hwnd, QSize size,
                                 const QImage &underlay, const QImage &overlay,
                                 const QVector<GpuCanvasSprite> &sprites) {
        last = {};
        last.physicalSize = size;
        if (!rawDevice || !rawContext || !hwnd || !IsWindow(hwnd) || !size.isValid() ||
            size.width() <= 0 || size.height() <= 0)
            return failure(GpuCanvasPresentStatus::InvalidArguments, E_INVALIDARG, size);
        if (!ensureDevice(rawDevice, rawContext, size))
            return failure(deviceLost ? GpuCanvasPresentStatus::DeviceLost
                                       : GpuCanvasPresentStatus::Failed,
                           deviceLost ? DXGI_ERROR_DEVICE_REMOVED : E_FAIL, size);
        if (!budgetAllows(size, underlay, overlay))
            return failure(GpuCanvasPresentStatus::BudgetExceeded, E_OUTOFMEMORY, size);
        if (!compileShaders() || !ensureSwapChain(hwnd, size))
            return failure(deviceLost ? GpuCanvasPresentStatus::DeviceLost
                                       : GpuCanvasPresentStatus::Failed,
                           deviceLost ? DXGI_ERROR_DEVICE_REMOVED : E_FAIL, size);

        quint64 underlayBytes = 0, overlayBytes = 0;
        if (!ensureChromeTexture(underlay, true, underlayBytes) ||
            !ensureChromeTexture(overlay, false, overlayBytes))
            return failure(GpuCanvasPresentStatus::Failed, E_FAIL, size);

        // GpuPreview's immediate context can have SRVs/UAVs bound from the
        // compute pass.  Unbind them before drawing, and clear them again at
        // the end so a subsequent compute operation sees no graphics hazard.
        clearBindings();
        const float clearColor[4] = {0.f, 0.f, 0.f, 0.f};
        context->ClearRenderTargetView(renderTarget.Get(), clearColor);
        D3D11_VIEWPORT viewport{};
        viewport.Width = float(size.width());
        viewport.Height = float(size.height());
        viewport.MinDepth = 0.f;
        viewport.MaxDepth = 1.f;
        context->RSSetViewports(1, &viewport);
        context->OMSetRenderTargets(1, renderTarget.GetAddressOf(), nullptr);
        context->RSSetState(rasterizerState.Get());
        const float blendFactor[4] = {};
        context->OMSetBlendState(blendState.Get(), blendFactor, 0xffffffffu);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertexShader.Get(), nullptr, 0);
        context->PSSetShader(pixelShader.Get(), nullptr, 0);

        const QRectF fullTarget(0, 0, size.width(), size.height());
        bool drawOk = true;
        if (underlayView)
            drawOk = drawTexture(underlayView.Get(), underlaySize, {}, fullTarget, false);
        if (drawOk) {
            for (const auto &sprite : sprites) {
                if (!sprite.shaderResourceView) continue;
                if (!drawTexture(static_cast<ID3D11ShaderResourceView *>(sprite.shaderResourceView),
                                 sprite.textureSize, sprite.sourceRect, sprite.targetRect,
                                 sprite.smooth)) {
                    drawOk = false;
                    break;
                }
            }
        }
        if (drawOk && overlayView)
            drawOk = drawTexture(overlayView.Get(), overlaySize, {}, fullTarget, false);
        clearBindings();
        if (!drawOk) return failure(GpuCanvasPresentStatus::Failed, E_FAIL, size);

        captureBeforePresent();

        // DO_NOT_WAIT gives Canvas a non-blocking handoff.  WAS_STILL_DRAWING
        // means the current swap-chain frame remains visible; it is a
        // successful GPU path, never a reason to read document pixels back.
        const HRESULT hr = swapChain->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
        last.nativeCode = qint64(hr);
        last.physicalSize = size;
        last.underlayUploadBytes = underlayBytes;
        last.overlayUploadBytes = overlayBytes;
        last.sourceReadback = false;
        last.sourceUpload = false;
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            last.status = GpuCanvasPresentStatus::PresentWouldBlock;
            return last;
        }
        if (isDeviceLost(hr)) {
            resetOwned();
            return failure(GpuCanvasPresentStatus::DeviceLost, hr, size);
        }
        if (FAILED(hr)) return failure(GpuCanvasPresentStatus::Failed, hr, size);
        last.status = GpuCanvasPresentStatus::Presented;
        return last;
    }
};

const char *gpuCanvasPresentStatusName(GpuCanvasPresentStatus status) noexcept {
    switch (status) {
    case GpuCanvasPresentStatus::Idle: return "idle";
    case GpuCanvasPresentStatus::Presented: return "presented";
    case GpuCanvasPresentStatus::PresentWouldBlock: return "present-would-block";
    case GpuCanvasPresentStatus::InvalidArguments: return "invalid-arguments";
    case GpuCanvasPresentStatus::Unsupported: return "unsupported";
    case GpuCanvasPresentStatus::BudgetExceeded: return "budget-exceeded";
    case GpuCanvasPresentStatus::DeviceLost: return "device-lost";
    case GpuCanvasPresentStatus::Failed: return "failed";
    }
    return "unknown";
}

GpuCanvasPresenter::GpuCanvasPresenter() : impl_(std::make_unique<Impl>()) {}
GpuCanvasPresenter::~GpuCanvasPresenter() = default;

bool GpuCanvasPresenter::present(void *device, void *context,
                                 GpuCanvasWindowHandle window, QSize physicalSize,
                                 const QImage &underlay, const QImage &overlay,
                                 const QVector<GpuCanvasSprite> &sprites) {
    return impl_->present(device, context, window, physicalSize, underlay, overlay, sprites).presented();
}

void GpuCanvasPresenter::setCaptureForTesting(bool enabled) {
    impl_->captureEnabled = enabled;
    if (!enabled) {
        impl_->captureStaging.Reset();
        impl_->captureTexture.Reset();
        impl_->captureSize = {};
        impl_->captureFormat = DXGI_FORMAT_UNKNOWN;
    }
}

QImage GpuCanvasPresenter::captureRenderedFrameForTesting(void *device, void *context) {
    return impl_->capture(device, context);
}

void GpuCanvasPresenter::release() {
    impl_->resetOwned();
    impl_->last = {};
}

GpuCanvasPresentInfo GpuCanvasPresenter::lastPresent() const { return impl_->last; }

bool GpuCanvasPresenter::isReady() const noexcept {
    return impl_->swapChain && impl_->renderTarget && impl_->vertexShader && impl_->pixelShader;
}

} // namespace comp

#else // Q_OS_WIN

namespace comp {
struct GpuCanvasPresenter::Impl {
    GpuCanvasPresentInfo last;
};

const char *gpuCanvasPresentStatusName(GpuCanvasPresentStatus status) noexcept {
    switch (status) {
    case GpuCanvasPresentStatus::Idle: return "idle";
    case GpuCanvasPresentStatus::Presented: return "presented";
    case GpuCanvasPresentStatus::PresentWouldBlock: return "present-would-block";
    case GpuCanvasPresentStatus::InvalidArguments: return "invalid-arguments";
    case GpuCanvasPresentStatus::Unsupported: return "unsupported";
    case GpuCanvasPresentStatus::BudgetExceeded: return "budget-exceeded";
    case GpuCanvasPresentStatus::DeviceLost: return "device-lost";
    case GpuCanvasPresentStatus::Failed: return "failed";
    }
    return "unknown";
}

GpuCanvasPresenter::GpuCanvasPresenter() : impl_(std::make_unique<Impl>()) {}
GpuCanvasPresenter::~GpuCanvasPresenter() = default;

bool GpuCanvasPresenter::present(void *device, void *context,
                                 GpuCanvasWindowHandle window, QSize physicalSize,
                                 const QImage &underlay, const QImage &overlay,
                                 const QVector<GpuCanvasSprite> &sprites) {
    Q_UNUSED(device); Q_UNUSED(context); Q_UNUSED(window); Q_UNUSED(underlay); Q_UNUSED(overlay);
    Q_UNUSED(sprites);
    impl_->last = {};
    impl_->last.status = GpuCanvasPresentStatus::Unsupported;
    impl_->last.physicalSize = physicalSize;
    return false;
}

void GpuCanvasPresenter::setCaptureForTesting(bool enabled) { Q_UNUSED(enabled); }

QImage GpuCanvasPresenter::captureRenderedFrameForTesting(void *device, void *context) {
    Q_UNUSED(device); Q_UNUSED(context);
    return {};
}

void GpuCanvasPresenter::release() { impl_->last = {}; }
GpuCanvasPresentInfo GpuCanvasPresenter::lastPresent() const { return impl_->last; }
bool GpuCanvasPresenter::isReady() const noexcept { return false; }

} // namespace comp

#endif
