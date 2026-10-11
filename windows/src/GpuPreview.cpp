#include "GpuPreview.h"
#include "Renderer.h"
#include "Pixels.h"
#include <QColorSpace>
#include <QPainter>
#include <algorithm>
#include <mutex>
#include <list>
#include <map>
#include <cstring>
#include <QDataStream>
#include <QIODevice>
#include <QSet>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#endif
namespace comp {
#ifdef Q_OS_WIN
namespace {
using Microsoft::WRL::ComPtr;
// All samples are premultiplied. Quantize at layer boundaries like the integer CPU renderer.
const char shaderSource[] = R"hlsl(
Texture2D<float4> backdrop : register(t0);
Texture2D<float4> source : register(t1);
RWTexture2D<float4> outputImage : register(u0);
cbuffer Settings : register(b0) { uint width; uint height; uint mode; float maximum; };
float luminance(float3 c) { return dot(c, float3(.3,.59,.11)); }
float saturation(float3 c) { return max(c.r,max(c.g,c.b))-min(c.r,min(c.g,c.b)); }
float3 setLuminance(float3 c,float l) {
    c += l-luminance(c);
    float n=min(c.r,min(c.g,c.b)),x=max(c.r,max(c.g,c.b));
    if(n<0) c=l+(c-l)*l/(l-n);
    if(x>1) c=l+(c-l)*(1-l)/(x-l);
    return c;
}
float3 setSaturation(float3 c,float s) {
    float n=min(c.r,min(c.g,c.b)),x=max(c.r,max(c.g,c.b));
    return x>n ? (c-n)*s/(x-n) : float3(0,0,0);
}
float burn(float b,float s) { return b>=1?1:s<=0?0:1-min(1,(1-b)/s); }
float dodge(float b,float s) { return b<=0?0:s>=1?1:min(1,b/(1-s)); }
float blend(float b,float s) {
    switch(mode) {
    case 1:return min(b,s); case 2:return b*s; case 3:return burn(b,s);
    case 4:return max(0,b+s-1); case 5:return max(b,s); case 6:return b+s-b*s;
    case 7:return dodge(b,s); case 8:return min(1,b+s);
    case 9:return b<=.5?2*b*s:1-2*(1-b)*(1-s);
    case 10:return s<=.5?b-(1-2*s)*b*(1-b):b+(2*s-1)*((b<=.25?((16*b-12)*b+4)*b:sqrt(b))-b);
    case 11:return s<=.5?2*b*s:1-2*(1-b)*(1-s);
    case 12:return s<=.5?burn(b,2*s):dodge(b,2*s-1);
    case 13:return saturate(b+2*s-1); case 14:return s<=.5?min(b,2*s):max(b,2*s-1);
    case 15:return (s<=.5?burn(b,2*s):dodge(b,2*s-1))<.5?0:1;
    case 16:return abs(b-s); case 17:return b+s-2*b*s;
    case 18:return max(0,b-s); case 19:return s==0?1:min(1,b/s);
    default:return s;
    }
}
[numthreads(16,16,1)]
void main(uint3 p:SV_DispatchThreadID) {
    if(p.x>=width || p.y>=height) return;
    float4 bp=backdrop.Load(int3(p.xy,0)), sp=source.Load(int3(p.xy,0));
    if(sp.a==0) { outputImage[p.xy]=bp; return; }
    float3 b=bp.a>0?bp.rgb/bp.a:float3(0,0,0),s=sp.rgb/sp.a,v;
    if(mode==20) v=setLuminance(setSaturation(s,saturation(b)),luminance(b));
    else if(mode==21) v=setLuminance(setSaturation(b,saturation(s)),luminance(b));
    else if(mode==22) v=setLuminance(s,luminance(b));
    else if(mode==23) v=setLuminance(b,luminance(s));
    else v=float3(blend(b.r,s.r),blend(b.g,s.g),blend(b.b,s.b));
    float a=sp.a+bp.a*(1-sp.a);
    float3 c=(1-sp.a)*bp.a*b+(1-bp.a)*sp.a*s+bp.a*sp.a*v;
    outputImage[p.xy]=floor(float4(clamp(c,0,a),a)*maximum+.5)/maximum;
}
)hlsl";
struct Device {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> settings;
    QString status;
    Device() {
        const D3D_FEATURE_LEVEL wanted[] = {D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL actual;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                      0, wanted, 1, D3D11_SDK_VERSION,
                                      &device, &actual, &context);
        if (FAILED(hr)) { status = "Direct3D 11 hardware unavailable"; return; }
        ComPtr<ID3DBlob> code, errors;
        hr = D3DCompile(shaderSource, sizeof(shaderSource)-1, "CompositorPreview", nullptr, nullptr,
                        "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (FAILED(hr)) {
            status = errors ? QString::fromUtf8(static_cast<const char *>(errors->GetBufferPointer()), int(errors->GetBufferSize()))
                            : "Preview shader compilation failed";
            return;
        }
        hr = device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader);
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = 16; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(hr) || FAILED(device->CreateBuffer(&desc, nullptr, &settings))) {
            shader.Reset(); status = "Preview shader initialization failed"; return;
        }
        status = "Direct3D 11 hardware compute";
    }
    bool ready() const { return shader && SUCCEEDED(device->GetDeviceRemovedReason()); }
};
// One serialized immediate context, rather than device destruction in Windows
// worker-thread TLS callbacks (where graphics-driver teardown can deadlock).
std::mutex gpuMutex;
GpuPreviewMetrics metrics;
Device &gpu() { static Device instance; return instance; }
struct Texture {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11UnorderedAccessView> output;
};
bool createTexture(Device &d, QSize size, DXGI_FORMAT format, Texture &out, const QImage *image = nullptr) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = UINT(size.width()); desc.Height = UINT(size.height());
    desc.MipLevels = desc.ArraySize = 1; desc.Format = format; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (image ? 0 : D3D11_BIND_UNORDERED_ACCESS);
    D3D11_SUBRESOURCE_DATA data{};
    if (image) { data.pSysMem = image->constBits(); data.SysMemPitch = UINT(image->bytesPerLine()); }
    ++metrics.textureAllocations;
    return SUCCEEDED(d.device->CreateTexture2D(&desc, image ? &data : nullptr, &out.texture)) &&
           SUCCEEDED(d.device->CreateShaderResourceView(out.texture.Get(), nullptr, &out.view)) &&
           (image || SUCCEEDED(d.device->CreateUnorderedAccessView(out.texture.Get(), nullptr, &out.output)));
}

struct SourceCache {
    Texture source;
    QByteArray signature;
    QImage pixels;
};
struct FrameCache {
    QString document;
    QSize canvas, target;
    QPoint origin;
    int depth = 8;
    Texture buffers[2];
    ComPtr<ID3D11Texture2D> staging;
    std::map<QString, SourceCache> layers;
    QByteArray signature;
    QImage result;
    quint64 bytes = 0;
};
// Cache the CPU staging mirrors and final readback as well as GPU allocations.
// The estimate excludes driver bookkeeping; the DXGI budget check leaves headroom.
std::list<FrameCache> frames;
quint64 residentBytes() {
    quint64 result = 0;
    for (const auto &frame : frames) result += frame.bytes;
    return result;
}
quint64 cacheBudget(Device &d) {
    constexpr quint64 megabyte = 1024 * 1024;
    quint64 result = 512 * megabyte;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIAdapter3> memory;
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (SUCCEEDED(d.device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter.As(&memory)) &&
        SUCCEEDED(memory->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
        result = std::min(result, info.Budget / 4);
        result = std::min(result, (info.Budget > info.CurrentUsage ? info.Budget - info.CurrentUsage : 0) + residentBytes());
    }
    bool configured = false;
    const int megabytes = qEnvironmentVariableIntValue("COMPOSITOR_GPU_CACHE_MB", &configured);
    if (configured) result = std::min(result, quint64(std::clamp(megabytes, 1, 512)) * megabyte);
    return result;
}
void trimCache(quint64 allowed, const FrameCache *keep = nullptr) {
    for (auto it = frames.begin(); residentBytes() > allowed && it != frames.end();) {
        if (&*it == keep) { ++it; continue; }
        it = frames.erase(it);
        ++metrics.evictions;
    }
}
QByteArray sourceSignature(const State &state, int index, const QVector<int> &ancestors) {
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    auto record = [&](const Layer &l) {
        stream << l.id << l.image.cacheKey() << l.mask.cacheKey() << l.opacity
               << l.placement.json() << l.maskPlacement.json()
               << l.maskEnabled << l.maskLinked << l.maskSource;
    };
    record(state.layers[index]);
    QSet<QString> visited;
    QString source = state.layers[index].maskSource;
    while (!source.isEmpty() && !visited.contains(source)) {
        visited.insert(source);
        const int i = state.index(source);
        if (i < 0) break;
        record(state.layers[i]);
        source = state.layers[i].maskSource;
    }
    for (int ancestor : ancestors) record(state.layers[ancestor]);
    return bytes;
}
QRect changedBounds(const QImage &before, const QImage &after) {
    if (before.size() != after.size() || before.format() != after.format()) return after.rect();
    int left = after.width(), top = after.height(), right = -1, bottom = -1;
    const int stride = after.depth() / 8;
    for (int y = 0; y < after.height(); ++y) {
        const auto *a = before.constScanLine(y), *b = after.constScanLine(y);
        if (!std::memcmp(a, b, size_t(after.width()) * stride)) continue;
        top = std::min(top, y); bottom = y;
        int start = 0, end = after.width() - 1;
        while (start < left && !std::memcmp(a + start * stride, b + start * stride, stride)) ++start;
        while (end > right && !std::memcmp(a + end * stride, b + end * stride, stride)) --end;
        left = std::min(left, start); right = std::max(right, end);
    }
    return right < left ? QRect() : QRect(QPoint(left, top), QPoint(right, bottom));
}
struct VisibleLayer {
    int index;
    QVector<int> ancestors;
    QByteArray signature;
};
} // namespace
#endif
QString gpuPreviewStatus() {
#ifdef Q_OS_WIN
    std::lock_guard lock(gpuMutex);
    return gpu().status;
#else
    return "Direct3D preview requires Windows";
#endif
}
GpuPreviewMetrics gpuPreviewMetrics() {
#ifdef Q_OS_WIN
    std::lock_guard lock(gpuMutex);
    auto result = metrics;
    result.residentBytes = residentBytes();
    result.cachedFrames = frames.size();
    return result;
#else
    return {};
#endif
}
void resetGpuPreviewMetrics() {
#ifdef Q_OS_WIN
    std::lock_guard lock(gpuMutex);
    metrics = {};
#endif
}
void clearGpuPreviewCache() {
#ifdef Q_OS_WIN
    std::lock_guard lock(gpuMutex);
    frames.clear();
#endif
}
QImage renderGpuPreview(const State &state, QSize target, QPoint viewportOrigin) {
#ifdef Q_OS_WIN
    if (qEnvironmentVariableIntValue("COMPOSITOR_DISABLE_GPU")) {
        clearGpuPreviewCache();
        return {};
    }
    if (!target.isValid() || qint64(target.width()) * target.height() > 8 * 1024 * 1024) return {};
    for (const auto &layer : state.layers)
        if (layer.metadata["adjustment"].isObject() || !layer.metadata["effects"].toObject().isEmpty()) return {};
    std::lock_guard lock(gpuMutex);
    auto &d = gpu();
    if (!d.ready()) { frames.clear(); return {}; }
    QVector<VisibleLayer> visibleLayers;
    QSet<QString> visibleIds;
    QByteArray signature;
    QDataStream key(&signature, QIODevice::WriteOnly);
    key << state.resolution;
    for (int index : state.drawOrder()) {
        const auto &layer = state.layers[index];
        if (layer.group || !layer.visible || layer.image.isNull()) continue;
        double opacity = layer.opacity;
        bool visible = true;
        QVector<int> ancestors;
        QString parent = layer.parent;
        while (!parent.isEmpty()) {
            const int i = state.index(parent);
            if (i < 0) break;
            ancestors.append(i); opacity *= state.layers[i].opacity;
            visible = visible && state.layers[i].visible; parent = state.layers[i].parent;
        }
        if (!visible || opacity <= 0) continue;
        const auto fingerprint = sourceSignature(state, index, ancestors);
        visibleLayers.append({index, ancestors, fingerprint});
        visibleIds.insert(layer.id);
        key << layer.id << layer.blend << fingerprint;
    }
    const quint64 area = quint64(target.width()) * target.height();
    const quint64 sampleBytes = state.bitDepth == 16 ? 8 : 4;
    const quint64 required = area * (48 + sampleBytes + visibleLayers.size() * sampleBytes * 2);
    const quint64 budget = cacheBudget(d);
    metrics.budgetBytes = budget;
    trimCache(budget);
    if (required > budget) return {};
    auto found = std::find_if(frames.begin(), frames.end(), [&](const FrameCache &f) {
        return f.document == state.id && f.canvas == state.size && f.target == target &&
               f.origin == viewportOrigin && f.depth == state.bitDepth;
    });
    if (found == frames.end()) {
        trimCache(budget - required);
        frames.emplace_back();
        found = std::prev(frames.end());
        found->document = state.id; found->canvas = state.size; found->target = target;
        found->origin = viewportOrigin; found->depth = state.bitDepth;
    } else {
        frames.splice(frames.end(), frames, found);
        const auto extra = required > found->bytes ? required - found->bytes : 0;
        trimCache(budget - extra, &*found);
    }
    auto &frame = *found;
    frame.bytes = required;
    for (auto it = frame.layers.begin(); it != frame.layers.end();)
        if (!visibleIds.contains(it->first)) it = frame.layers.erase(it);
        else ++it;
    if (frame.signature == signature && !frame.result.isNull()) {
        ++metrics.cacheHits;
        return frame.result;
    }
    auto failed = [&]() -> QImage { frames.erase(found); return {}; };
    if (!frame.buffers[0].texture) {
        if (!createTexture(d, target, DXGI_FORMAT_R32G32B32A32_FLOAT, frame.buffers[0]) ||
            !createTexture(d, target, DXGI_FORMAT_R32G32B32A32_FLOAT, frame.buffers[1])) return failed();
        D3D11_TEXTURE2D_DESC desc{};
        frame.buffers[0].texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ++metrics.textureAllocations;
        if (FAILED(d.device->CreateTexture2D(&desc, nullptr, &frame.staging))) return failed();
    }
    const float transparent[4]{};
    d.context->ClearUnorderedAccessViewFloat(frame.buffers[0].output.Get(), transparent);
    d.context->CSSetShader(d.shader.Get(), nullptr, 0);
    auto *constants = d.settings.Get();
    d.context->CSSetConstantBuffers(0, 1, &constants);
    int current = 0;
    for (const auto &entry : visibleLayers) {
        const int index = entry.index;
        const auto &layer = state.layers[index];
        auto &cached = frame.layers[layer.id];
        if (cached.signature != entry.signature || !cached.source.texture) {
            ++metrics.preparedLayers;
            auto image = layerSurface(state, index, target);
            double groupOpacity = 1;
            for (int ancestor : entry.ancestors) {
                const auto &group = state.layers[ancestor];
                groupOpacity *= group.opacity;
                if (!group.mask.isNull() && group.maskEnabled) {
                    QPainter painter(&image);
                    painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
                    painter.drawImage(0, 0, renderMask(group, state.size, target));
                }
            }
            if (groupOpacity < 1) {
                QPainter painter(&image);
                painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
                painter.fillRect(image.rect(), QColor::fromRgbF(0, 0, 0, groupOpacity));
            }
            image = image.convertToFormat(state.bitDepth == 16 ? QImage::Format_RGBA64_Premultiplied
                                                             : QImage::Format_RGBA8888_Premultiplied);
            if (!cached.source.texture) {
                if (!createTexture(d, target, state.bitDepth == 16 ? DXGI_FORMAT_R16G16B16A16_UNORM
                                                                 : DXGI_FORMAT_R8G8B8A8_UNORM, cached.source, &image))
                    return failed();
                ++metrics.sourceUploads; metrics.uploadedBytes += area * sampleBytes;
            } else {
                const auto changed = changedBounds(cached.pixels, image);
                if (!changed.isEmpty()) {
                    D3D11_BOX box{UINT(changed.left()), UINT(changed.top()), 0,
                                  UINT(changed.right()+1), UINT(changed.bottom()+1), 1};
                    d.context->UpdateSubresource(cached.source.texture.Get(), 0, &box,
                        image.constScanLine(changed.top()) + changed.left() * sampleBytes,
                        UINT(image.bytesPerLine()), 0);
                    ++metrics.sourceUploads;
                    metrics.uploadedBytes += quint64(changed.width()) * changed.height() * sampleBytes;
                }
            }
            cached.pixels = image;
            cached.signature = entry.signature;
        }
        struct { UINT width, height, mode; float maximum; } params{
            UINT(target.width()), UINT(target.height()), UINT(std::max(0, int(blendModes().indexOf(layer.blend)))),
            state.bitDepth == 16 ? 65535.f : 255.f};
        d.context->UpdateSubresource(d.settings.Get(), 0, nullptr, &params, 0, 0);
        ID3D11ShaderResourceView *views[]{frame.buffers[current].view.Get(), cached.source.view.Get()};
        d.context->CSSetShaderResources(0, 2, views);
        auto *out = frame.buffers[1-current].output.Get();
        d.context->CSSetUnorderedAccessViews(0, 1, &out, nullptr);
        d.context->Dispatch((params.width+15)/16, (params.height+15)/16, 1);
        ++metrics.dispatches;
        ID3D11ShaderResourceView *noViews[2]{};
        ID3D11UnorderedAccessView *noOutput = nullptr;
        d.context->CSSetShaderResources(0, 2, noViews);
        d.context->CSSetUnorderedAccessViews(0, 1, &noOutput, nullptr);
        current = 1-current;
    }
    d.context->CopyResource(frame.staging.Get(), frame.buffers[current].texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(d.context->Map(frame.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return failed();
    ++metrics.readbacks;
    QImage result(target, pixelFormat(state.bitDepth));
    if (result.isNull()) { d.context->Unmap(frame.staging.Get(), 0); return failed(); }
    const int maximum = state.bitDepth == 16 ? 65535 : 255;
    for (int y = 0; y < target.height(); ++y) {
        const auto *row = reinterpret_cast<const float *>(static_cast<const char *>(mapped.pData) + y * mapped.RowPitch);
        for (int x = 0; x < target.width(); ++x) {
            int c[4];
            for (int channel = 0; channel < 4; ++channel)
                c[channel] = std::clamp(qRound(row[4*x+channel] * maximum), 0, maximum);
            if (state.bitDepth == 16)
                reinterpret_cast<QRgba64 *>(result.scanLine(y))[x] = QRgba64::fromRgba64(c[0], c[1], c[2], c[3]);
            else reinterpret_cast<QRgb *>(result.scanLine(y))[x] = qRgba(c[0], c[1], c[2], c[3]);
        }
    }
    d.context->Unmap(frame.staging.Get(), 0);
    if (!d.ready()) { frames.clear(); return {}; }
    result.setColorSpace(QColorSpace::SRgb);
    result.setDotsPerMeterX(qRound(state.resolution / .0254));
    result.setDotsPerMeterY(qRound(state.resolution / .0254));
    frame.result = result;
    frame.signature = signature;
    return result;
#else
    Q_UNUSED(state); Q_UNUSED(target); Q_UNUSED(viewportOrigin); return {};
#endif
}
} // namespace comp
