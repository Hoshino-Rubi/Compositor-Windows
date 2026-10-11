#include "GpuPreview.h"
#include "Renderer.h"
#include "Pixels.h"
#include "GpuBlurShader.h"
#include "GpuToneShader.h"
#include "Filters.h"
#include <bit>
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
#include <xmmintrin.h>
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
cbuffer Settings : register(b0) {
    uint width; uint height; uint mode; float maximum;
    uint offsetX; uint offsetY; uint firstLayer; uint padding;
};
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
    p.xy += uint2(offsetX,offsetY);
    float4 bp=firstLayer ? float4(0,0,0,0) : backdrop.Load(int3(p.xy,0));
    float4 sp=source.Load(int3(p.xy,0));
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
// Write native QImage channel order as integer samples, avoiding UNORM rounding
// ambiguity and a full floating-point-to-integer loop on the CPU after readback.
const char packShaderSource[] = R"hlsl(
Texture2D<float4> source : register(t0);
RWTexture2D<uint4> packedImage : register(u0);
cbuffer Settings : register(b0) {
    uint width; uint height; uint mode; float maximum;
    uint offsetX; uint offsetY; uint emptyImage; uint padding;
};
[numthreads(16,16,1)]
void main(uint3 p:SV_DispatchThreadID) {
    if(p.x>=width || p.y>=height) return;
    p.xy += uint2(offsetX,offsetY);
    float4 pixel=emptyImage ? float4(0,0,0,0) : source.Load(int3(p.xy,0));
    uint4 value=(uint4)floor(saturate(pixel)*maximum+.5);
    packedImage[p.xy]=maximum==255 ? value.bgra : value;
}
)hlsl";
struct Settings {
    UINT width, height, mode;
    float maximum;
    UINT offsetX, offsetY, firstLayer, padding = 0;
};
static_assert(sizeof(Settings) == 32);
struct Device {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> shader, packShader, blurShader, blurMixShader, toneShader;
    ComPtr<ID3D11Buffer> settings;
    QString status;
    Device() {
        const D3D_FEATURE_LEVEL wanted[] = {D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL actual;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                      0, wanted, 1, D3D11_SDK_VERSION,
                                      &device, &actual, &context);
        if (FAILED(hr)) { status = "Direct3D 11 hardware unavailable"; return; }
        auto compile = [&](const char *source, size_t size, const char *name, ComPtr<ID3D11ComputeShader> &out) {
            ComPtr<ID3DBlob> code, errors;
            HRESULT result = D3DCompile(source, size, name, nullptr, nullptr, "main", "cs_5_0",
                                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
            if (SUCCEEDED(result))
                result = device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &out);
            if (FAILED(result))
                status = errors ? QString::fromUtf8(static_cast<const char *>(errors->GetBufferPointer()), int(errors->GetBufferSize()))
                                : "Preview shader initialization failed";
            return SUCCEEDED(result);
        };
        if (!compile(shaderSource, sizeof(shaderSource)-1, "CompositorPreview", shader) ||
            !compile(packShaderSource, sizeof(packShaderSource)-1, "CompositorPack", packShader)) return;
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = sizeof(Settings); desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device->CreateBuffer(&desc, nullptr, &settings))) {
            shader.Reset(); status = "Preview shader initialization failed"; return;
        }
        status = "Direct3D 11 hardware compute";
    }
    bool prepareBlur() {
        if (blurShader && blurMixShader) return true;
        auto compile = [&](const char *source, size_t size, ComPtr<ID3D11ComputeShader> &out) {
            ComPtr<ID3DBlob> code, errors;
            HRESULT hr = D3DCompile(source, size, "CompositorBlur", nullptr, nullptr, "main", "cs_5_0",
                                   D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
            if (SUCCEEDED(hr)) hr = device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &out);
            if (FAILED(hr)) status = errors ? QString::fromUtf8(static_cast<const char *>(errors->GetBufferPointer()), int(errors->GetBufferSize()))
                                           : "Blur shader initialization failed";
            return SUCCEEDED(hr);
        };
        return compile(gpuBlurShader, sizeof(gpuBlurShader)-1, blurShader) &&
               compile(gpuBlurMixShader, sizeof(gpuBlurMixShader)-1, blurMixShader);
    }
    bool prepareTone() {
        if (toneShader) return true;
        ComPtr<ID3DBlob> code, errors;
        HRESULT hr = D3DCompile(gpuToneShader, sizeof(gpuToneShader)-1, "CompositorTone", nullptr, nullptr,
                               "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (SUCCEEDED(hr)) hr = device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &toneShader);
        if (FAILED(hr)) status = errors ? QString::fromUtf8(static_cast<const char *>(errors->GetBufferPointer()), int(errors->GetBufferSize()))
                                       : "Tone shader initialization failed";
        return SUCCEEDED(hr);
    }
    bool ready() const { return shader && packShader && settings && SUCCEEDED(device->GetDeviceRemovedReason()); }
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
    Texture source, toneTable;
    QByteArray signature, toneSignature;
    QImage pixels;
};
struct FrameCache {
    QString document;
    QSize canvas, target;
    QPoint origin;
    int depth = 8;
    Texture buffers[2], packed, blur[2], toneAlpha;
    ComPtr<ID3D11Texture2D> staging;
    std::map<QString, SourceCache> layers;
    QByteArray signature, stackSignature;
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
bool toneAdjustment(const QJsonObject &settings) {
    const auto kind = settings["kind"].toString();
    return kind == "Invert" || kind == "Exposure" || kind == "Levels" || kind == "Curves";
}
QImage toneAlphaTable(int depth) {
    // Match Qt's x86 image-conversion reciprocal and nearest-even rounding.
    // Compute these small alpha-only factors once; image pixels stay on GPU.
    QImage table(256, depth == 16 ? 256 : 1, QImage::Format_ARGB32);
    if (table.isNull()) throw Error("tooLarge");
    const int maximum = depth == 16 ? 65535 : 255;
    for (int alpha = 0; alpha <= maximum; ++alpha) {
        float factor = 0;
        if (alpha == maximum) factor = 1;
        else if (alpha) {
            const auto a = _mm_set_ss(float(alpha));
            const auto estimate = _mm_rcp_ss(a);
            const auto error = _mm_mul_ss(estimate, _mm_mul_ss(estimate, a));
            factor = _mm_cvtss_f32(_mm_mul_ss(_mm_sub_ss(_mm_add_ss(estimate, estimate), error), _mm_set_ss(float(maximum))));
        }
        reinterpret_cast<quint32 *>(table.scanLine(alpha / 256))[alpha % 256] = std::bit_cast<quint32>(factor);
    }
    return table;
}
QImage toneTable(const QJsonObject &settings, int depth) {
    // Separable RGB adjustments can be evaluated once for each possible input
    // sample rather than repeating QColor/curve/power work for every image pixel.
    QImage ramp(256, depth == 16 ? 256 : 1, pixelFormat(depth));
    if (ramp.isNull()) throw Error("tooLarge");
    for (int y = 0; y < ramp.height(); ++y) for (int x = 0; x < 256; ++x) {
        const int value = y * 256 + x;
        if (depth == 16) reinterpret_cast<QRgba64 *>(ramp.scanLine(y))[x] = rgba64(value, value, value, 65535);
        else reinterpret_cast<QRgb *>(ramp.scanLine(y))[x] = qRgb(value, value, value);
    }
    return filtered(ramp, settings).convertToFormat(depth == 16 ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
}
struct VisibleLayer {
    int index;
    QVector<int> ancestors;
    QByteArray signature;
    bool adjustment = false;
    double opacity = 1;
};
QImage adjustmentCoverage(const State &state, const VisibleLayer &entry, QSize target) {
    const auto &layer = state.layers[entry.index];
    QImage result(target, pixelFormat(state.bitDepth));
    if (result.isNull()) throw Error("tooLarge");
    result.fill(Qt::white);
    auto multiply = [&](const QImage &mask) {
        QPainter painter(&result);
        painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        painter.drawImage(0, 0, mask);
    };
    if (!layer.mask.isNull() && layer.maskEnabled) multiply(renderMask(layer, state.size, target));
    for (int index : entry.ancestors) {
        const auto &group = state.layers[index];
        if (!group.mask.isNull() && group.maskEnabled) multiply(renderMask(group, state.size, target));
    }
    const int source = state.index(layer.maskSource);
    if (source >= 0) multiply(layerSurface(state, source, target));
    return result;
}
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
    for (const auto &layer : state.layers) {
        if (!layer.metadata["effects"].toObject().isEmpty()) return {};
        if (layer.metadata["adjustment"].isObject()) {
            const auto settings = layer.metadata["adjustment"].toObject();
            if (layer.blend != "Normal" || (settings["kind"] != "Gaussian Blur" && !toneAdjustment(settings))) return {};
        }
    }
    std::lock_guard lock(gpuMutex);
    auto &d = gpu();
    if (!d.ready()) { frames.clear(); return {}; }
    QVector<VisibleLayer> visibleLayers;
    QSet<QString> visibleIds;
    bool hasBlur = false;
    int toneLayers = 0;
    QByteArray signature, stackSignature;
    QDataStream key(&signature, QIODevice::WriteOnly);
    QDataStream stackKey(&stackSignature, QIODevice::WriteOnly);
    key << state.resolution;
    for (int index : state.drawOrder()) {
        const auto &layer = state.layers[index];
        const bool adjustment = layer.metadata["adjustment"].isObject();
        if (layer.group || !layer.visible || (layer.image.isNull() && !adjustment)) continue;
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
        auto fingerprint = sourceSignature(state, index, ancestors);
        fingerprint.append(adjustment ? 'A' : 'R');
        visibleLayers.append({index, ancestors, fingerprint, adjustment, opacity});
        hasBlur = hasBlur || (adjustment && layer.metadata["adjustment"].toObject()["kind"] == "Gaussian Blur");
        if (adjustment && toneAdjustment(layer.metadata["adjustment"].toObject())) ++toneLayers;
        visibleIds.insert(layer.id);
        key << layer.id << layer.blend << fingerprint;
        stackKey << layer.id << layer.blend << adjustment;
        if (adjustment) {
            key << layer.metadata["adjustment"].toObject() << opacity;
            stackKey << layer.metadata["adjustment"].toObject() << opacity;
        }
    }
    const quint64 area = quint64(target.width()) * target.height();
    const quint64 sampleBytes = state.bitDepth == 16 ? 8 : 4;
    const quint64 tableBytes = (state.bitDepth == 16 ? 65536 : 256) * sampleBytes;
    const quint64 required = area * (32 + (hasBlur ? 32 : 0) + sampleBytes * 3 + visibleLayers.size() * sampleBytes * 2) +
                             toneLayers * tableBytes + (toneLayers ? (state.bitDepth == 16 ? 65536 : 256) * 4 : 0);
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
    const bool forceFull = qEnvironmentVariableIntValue("COMPOSITOR_GPU_FULL_FRAME");
    if (!forceFull && frame.signature == signature && !frame.result.isNull()) {
        ++metrics.cacheHits;
        return frame.result;
    }
    auto failed = [&]() -> QImage { frames.erase(found); return {}; };
    if (toneLayers) {
        if (!d.prepareTone()) return failed();
        if (!frame.toneAlpha.texture) {
            const auto table = toneAlphaTable(state.bitDepth);
            if (!createTexture(d, table.size(), DXGI_FORMAT_R32_FLOAT, frame.toneAlpha, &table)) return failed();
        }
    } else frame.toneAlpha = {};
    if (hasBlur) {
        if (!d.prepareBlur()) return failed();
        if (!frame.blur[0].texture &&
            (!createTexture(d, target, DXGI_FORMAT_R32G32B32A32_FLOAT, frame.blur[0]) ||
             !createTexture(d, target, DXGI_FORMAT_R32G32B32A32_FLOAT, frame.blur[1]))) return failed();
    } else {
        frame.blur[0] = {}; frame.blur[1] = {};
    }
    const DXGI_FORMAT packedFormat = state.bitDepth == 16 ? DXGI_FORMAT_R16G16B16A16_UINT : DXGI_FORMAT_R8G8B8A8_UINT;
    if (!frame.buffers[0].texture) {
        UINT support = 0;
        if (FAILED(d.device->CheckFormatSupport(packedFormat, &support)) ||
            !(support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW)) return failed();
        if (!createTexture(d, target, DXGI_FORMAT_R32G32B32A32_FLOAT, frame.buffers[0]) ||
            !createTexture(d, target, DXGI_FORMAT_R32G32B32A32_FLOAT, frame.buffers[1]) ||
            !createTexture(d, target, packedFormat, frame.packed)) return failed();
        D3D11_TEXTURE2D_DESC desc{};
        frame.packed.texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ++metrics.textureAllocations;
        if (FAILED(d.device->CreateTexture2D(&desc, nullptr, &frame.staging))) return failed();
    }
    // A changed stack can affect every pixel. Otherwise the prepared surface diff
    // includes both old/new geometry and all mask/clipping dependencies.
    QRect dirty;
    if (frame.result.isNull() || frame.stackSignature != stackSignature || hasBlur ||
        forceFull) dirty = QRect(QPoint(), target);
    for (const auto &entry : visibleLayers) {
        const int index = entry.index;
        const auto &layer = state.layers[index];
        auto &cached = frame.layers[layer.id];
        if (entry.adjustment && toneAdjustment(layer.metadata["adjustment"].toObject())) {
            QByteArray toneKey;
            QDataStream tableKey(&toneKey, QIODevice::WriteOnly);
            tableKey << layer.metadata["adjustment"].toObject();
            if (!cached.toneTable.texture || cached.toneSignature != toneKey) {
                auto table = toneTable(layer.metadata["adjustment"].toObject(), state.bitDepth);
                ++metrics.toneTableBuilds;
                if (!cached.toneTable.texture) {
                    if (!createTexture(d, table.size(), packedFormat, cached.toneTable, &table)) return failed();
                } else d.context->UpdateSubresource(cached.toneTable.texture.Get(), 0, nullptr,
                                                   table.constBits(), UINT(table.bytesPerLine()), 0);
                ++metrics.toneTableUploads; metrics.toneTableBytes += tableBytes;
                cached.toneSignature = toneKey;
            }
        } else { cached.toneTable = {}; cached.toneSignature.clear(); }
        if (cached.signature != entry.signature || !cached.source.texture) {
            ++metrics.preparedLayers;
            auto image = entry.adjustment ? adjustmentCoverage(state, entry, target) : layerSurface(state, index, target);
            if (!entry.adjustment) {
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
            }
            image = image.convertToFormat(state.bitDepth == 16 ? QImage::Format_RGBA64_Premultiplied
                                                             : QImage::Format_RGBA8888_Premultiplied);
            if (!cached.source.texture) {
                if (!createTexture(d, target, state.bitDepth == 16 ? DXGI_FORMAT_R16G16B16A16_UNORM
                                                                 : DXGI_FORMAT_R8G8B8A8_UNORM, cached.source, &image))
                    return failed();
                ++metrics.sourceUploads; metrics.uploadedBytes += area * sampleBytes;
                dirty = QRect(QPoint(), target);
            } else {
                const auto changed = changedBounds(cached.pixels, image);
                if (!changed.isEmpty()) {
                    dirty = dirty.united(changed);
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
    }
    QImage result = std::move(frame.result);
    if (!dirty.isEmpty()) {
        auto *constants = d.settings.Get();
        d.context->CSSetConstantBuffers(0, 1, &constants);
        d.context->CSSetShader(d.shader.Get(), nullptr, 0);
        ID3D11ShaderResourceView *noViews[4]{};
        ID3D11UnorderedAccessView *noOutput = nullptr;
        auto unbind = [&] {
            d.context->CSSetShaderResources(0, 4, noViews);
            d.context->CSSetUnorderedAccessViews(0, 1, &noOutput, nullptr);
        };
        Settings params{UINT(dirty.width()), UINT(dirty.height()), 0, state.bitDepth == 16 ? 65535.f : 255.f,
                        UINT(dirty.x()), UINT(dirty.y()), 1};
        const quint64 pixels = quint64(dirty.width()) * dirty.height();
        int current = 0;
        for (const auto &entry : visibleLayers) {
            const auto &layer = state.layers[entry.index];
            if (entry.adjustment && toneAdjustment(layer.metadata["adjustment"].toObject())) {
                Settings toneParams = params;
                toneParams.padding = std::bit_cast<UINT>(float(entry.opacity));
                d.context->CSSetShader(d.toneShader.Get(), nullptr, 0);
                d.context->UpdateSubresource(d.settings.Get(), 0, nullptr, &toneParams, 0, 0);
                ID3D11ShaderResourceView *views[]{frame.buffers[current].view.Get(),
                    frame.layers[layer.id].toneTable.view.Get(), frame.layers[layer.id].source.view.Get(), frame.toneAlpha.view.Get()};
                d.context->CSSetShaderResources(0, 4, views);
                auto *out = frame.buffers[1-current].output.Get();
                d.context->CSSetUnorderedAccessViews(0, 1, &out, nullptr);
                d.context->Dispatch((params.width+15)/16, (params.height+15)/16, 1);
                ++metrics.adjustmentDispatches; ++metrics.toneDispatches; metrics.tonedPixels += pixels;
                unbind(); current = 1-current; params.firstLayer = 0;
                continue;
            }
            if (entry.adjustment) {
                if (params.firstLayer) {
                    const float transparent[4]{};
                    d.context->ClearUnorderedAccessViewFloat(frame.buffers[current].output.Get(), transparent);
                }
                const double radius = layer.metadata["adjustment"].toObject()["blurRadius"].toDouble(10) *
                                      target.width() / state.size.width();
                Settings blurParams{UINT(target.width()), UINT(target.height()), 0, params.maximum,
                                    UINT(std::clamp(qRound(radius * .58), 1, 250)), 0, 0};
                auto *input = frame.buffers[current].view.Get();
                if (radius > 0) for (int pass = 0; pass < 6; ++pass) {
                    blurParams.mode = UINT(pass % 2);
                    d.context->CSSetShader(d.blurShader.Get(), nullptr, 0);
                    d.context->UpdateSubresource(d.settings.Get(), 0, nullptr, &blurParams, 0, 0);
                    d.context->CSSetShaderResources(0, 1, &input);
                    auto *out = frame.blur[pass % 2].output.Get();
                    d.context->CSSetUnorderedAccessViews(0, 1, &out, nullptr);
                    const UINT length = blurParams.mode ? blurParams.height : blurParams.width;
                    const UINT rows = blurParams.mode ? blurParams.width : blurParams.height;
                    d.context->Dispatch((length+255)/256, rows, 1);
                    ++metrics.blurDispatches;
                    metrics.blurredPixels += area;
                    unbind();
                    input = frame.blur[pass % 2].view.Get();
                }
                Settings mixParams = params;
                mixParams.padding = std::bit_cast<UINT>(float(entry.opacity));
                d.context->CSSetShader(d.blurMixShader.Get(), nullptr, 0);
                d.context->UpdateSubresource(d.settings.Get(), 0, nullptr, &mixParams, 0, 0);
                ID3D11ShaderResourceView *views[]{frame.buffers[current].view.Get(), input, frame.layers[layer.id].source.view.Get()};
                d.context->CSSetShaderResources(0, 3, views);
                auto *out = frame.buffers[1-current].output.Get();
                d.context->CSSetUnorderedAccessViews(0, 1, &out, nullptr);
                d.context->Dispatch((params.width+15)/16, (params.height+15)/16, 1);
                ++metrics.adjustmentDispatches;
                unbind();
                current = 1-current; params.firstLayer = 0;
                continue;
            }
            d.context->CSSetShader(d.shader.Get(), nullptr, 0);
            params.mode = UINT(std::max(0, int(blendModes().indexOf(layer.blend))));
            d.context->UpdateSubresource(d.settings.Get(), 0, nullptr, &params, 0, 0);
            ID3D11ShaderResourceView *views[]{frame.buffers[current].view.Get(), frame.layers[layer.id].source.view.Get()};
            d.context->CSSetShaderResources(0, 2, views);
            auto *out = frame.buffers[1-current].output.Get();
            d.context->CSSetUnorderedAccessViews(0, 1, &out, nullptr);
            d.context->Dispatch((params.width+15)/16, (params.height+15)/16, 1);
            ++metrics.dispatches;
            metrics.compositedPixels += pixels;
            unbind();
            current = 1-current;
            params.firstLayer = 0;
        }
        // Keep the packed texture and staging allocation resident, but transfer
        // only the dirty rectangle. Empty stacks explicitly write transparency.
        params.firstLayer = visibleLayers.isEmpty() ? 1 : 0;
        d.context->UpdateSubresource(d.settings.Get(), 0, nullptr, &params, 0, 0);
        d.context->CSSetShader(d.packShader.Get(), nullptr, 0);
        auto *input = frame.buffers[current].view.Get();
        auto *output = frame.packed.output.Get();
        d.context->CSSetShaderResources(0, 1, &input);
        d.context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
        d.context->Dispatch((params.width+15)/16, (params.height+15)/16, 1);
        ++metrics.packDispatches;
        metrics.packedPixels += pixels;
        unbind();
        const D3D11_BOX box{UINT(dirty.left()), UINT(dirty.top()), 0,
                            UINT(dirty.right()+1), UINT(dirty.bottom()+1), 1};
        d.context->CopySubresourceRegion(frame.staging.Get(), 0, box.left, box.top, 0,
                                         frame.packed.texture.Get(), 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(d.context->Map(frame.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return failed();
        ++metrics.readbacks;
        metrics.readbackBytes += pixels * sampleBytes;
        if (result.isNull()) result = QImage(target, pixelFormat(state.bitDepth));
        // Detach before modifying the retained image: callers may still display
        // earlier previews. Padding outside the copied region is never accessed.
        auto *destination = result.bits();
        if (!destination) { d.context->Unmap(frame.staging.Get(), 0); return failed(); }
        for (int y = dirty.top(); y <= dirty.bottom(); ++y)
            std::memcpy(destination + y * result.bytesPerLine() + dirty.left() * sampleBytes,
                        static_cast<const char *>(mapped.pData) + y * mapped.RowPitch + dirty.left() * sampleBytes,
                        size_t(dirty.width()) * sampleBytes);
        d.context->Unmap(frame.staging.Get(), 0);
        if (!d.ready()) { frames.clear(); return {}; }
    } else {
        ++metrics.cacheHits;
    }
    result.setColorSpace(QColorSpace::SRgb);
    result.setDotsPerMeterX(qRound(state.resolution / .0254));
    result.setDotsPerMeterY(qRound(state.resolution / .0254));
    frame.result = result;
    frame.signature = signature;
    frame.stackSignature = stackSignature;
    return result;
#else
    Q_UNUSED(state); Q_UNUSED(target); Q_UNUSED(viewportOrigin); return {};
#endif
}
} // namespace comp
