#include "GpuPreview.h"
#include "Renderer.h"
#include "Pixels.h"
#include "GpuBlurShader.h"
#include "GpuToneShader.h"
#include "GpuAdjustmentBlend.h"
#include "GpuProcessing.h"
#include "GpuLayerSurface.h"
#include "GpuWarp.h"
#include "Filters.h"
#include "CameraRaw.h"
#include "Effects.h"
#include <bit>
#include <QColorSpace>
#include <QPainter>
#include <algorithm>
#include <mutex>
#include <list>
#include <map>
#include <vector>
#include <cstring>
#include <QDataStream>
#include <QIODevice>
#include <QHash>
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
Texture2D<float4> postCoverage : register(t2);
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
    if(padding) sp=floor(sp*postCoverage.Load(int3(p.xy,0)).a*maximum+.5)/maximum;
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
    bool doublePrecision = false;
    std::unique_ptr<GpuProcessor> processor;
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
        D3D11_FEATURE_DATA_DOUBLES doubleSupport{};
        doublePrecision = SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_DOUBLES, &doubleSupport, sizeof(doubleSupport))) && doubleSupport.DoublePrecisionFloatShaderOps;
        processor = std::make_unique<GpuProcessor>(device.Get(), context.Get());
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
        const QByteArray mix = (doublePrecision ? QByteArray("#define EXACT_HARD_MIX 1\n") : QByteArray()) + QByteArray(gpuBlurMixShader).replace("// ADJUSTMENT_BLEND", gpuAdjustmentBlend);
        return compile(gpuBlurShader, sizeof(gpuBlurShader)-1, blurShader) &&
               compile(mix.constData(), size_t(mix.size()), blurMixShader);
    }
    bool prepareTone() {
        if (toneShader) return true;
        ComPtr<ID3DBlob> code, errors;
        const QByteArray source = (doublePrecision ? QByteArray("#define EXACT_HARD_MIX 1\n") : QByteArray()) + QByteArray(gpuToneShader).replace("// ADJUSTMENT_BLEND", gpuAdjustmentBlend);
        HRESULT hr = D3DCompile(source.constData(), size_t(source.size()), "CompositorTone", nullptr, nullptr,
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
bool createTexture(Device &d, QSize size, DXGI_FORMAT format, Texture &out, const QImage *image = nullptr,
                   bool processing = false) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = UINT(size.width()); desc.Height = UINT(size.height());
    desc.MipLevels = desc.ArraySize = 1; desc.Format = format; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (image ? 0 : D3D11_BIND_UNORDERED_ACCESS);
    D3D11_SUBRESOURCE_DATA data{};
    if (image) { data.pSysMem = image->constBits(); data.SysMemPitch = UINT(image->bytesPerLine()); }
    if (processing) ++metrics.processingTextureAllocations;
    else ++metrics.textureAllocations;
    return SUCCEEDED(d.device->CreateTexture2D(&desc, image ? &data : nullptr, &out.texture)) &&
           SUCCEEDED(d.device->CreateShaderResourceView(out.texture.Get(), nullptr, &out.view)) &&
           (image || SUCCEEDED(d.device->CreateUnorderedAccessView(out.texture.Get(), nullptr, &out.output)));
}

struct ProcessingCache {
    QSize size;
    int depth = 8;
    Texture input, packed;
    ComPtr<ID3D11Texture2D> staging;
    quint64 bytes = 0;
};

struct SourceCache {
    Texture source, toneTable, effectCoverage;
    std::shared_ptr<GpuNativeTexture> nativeSource;
    GpuSurfacePtr effected;
    QByteArray effectSignature, nativeSignature;
    QByteArray signature, toneSignature;
    QImage pixels;
};
struct FrameCache {
    QString document;
    QSize canvas, target;
    QPoint origin;
    int depth = 8;
    bool textureMode = false;
    Texture buffers[2], packed, blur[2], toneAlpha;
    ComPtr<ID3D11Texture2D> staging;
    std::map<QString, SourceCache> layers;
    QByteArray signature, stackSignature;
    QByteArray overrideSignature;
    QImage result;
    std::shared_ptr<GpuNativeTexture> textureSnapshot;
    int currentBuffer = 0;
    quint64 bytes = 0;
};
// Cache the CPU staging mirrors and final readback as well as GPU allocations.
// The estimate excludes driver bookkeeping; the DXGI budget check leaves headroom.
std::list<FrameCache> frames;
// Weak records let snapshots retained by a presenter remain visible to budget
// accounting after their frame cache entry is evicted, without keeping them
// alive indefinitely in the preview cache itself.
std::vector<std::weak_ptr<GpuNativeTexture>> textureSnapshots;
// Standalone filters/effects share one transfer set per size/depth.  The input is
// uploaded on every call; only the resource descriptors are retained.
std::list<ProcessingCache> processingCaches;
quint64 residentBytes() {
    quint64 result = 0;
    for (const auto &frame : frames) result += frame.bytes;
    for (const auto &cache : processingCaches) result += cache.bytes;
    for (auto it = textureSnapshots.begin(); it != textureSnapshots.end();) {
        const auto snapshot = it->lock();
        if (!snapshot) {
            it = textureSnapshots.erase(it);
            continue;
        }
        bool cached = false;
        for (const auto &frame : frames) {
            if (frame.textureSnapshot.get() == snapshot.get()) {
                cached = true;
                break;
            }
        }
        if (!cached) result += snapshot->bytes;
        ++it;
    }
    return result;
}
quint64 externalTextureBytes() {
    quint64 result = 0;
    for (auto it = textureSnapshots.begin(); it != textureSnapshots.end();) {
        const auto snapshot = it->lock();
        if (!snapshot) {
            it = textureSnapshots.erase(it);
            continue;
        }
        bool cached = false;
        for (const auto &frame : frames) {
            if (frame.textureSnapshot.get() == snapshot.get()) {
                cached = true;
                break;
            }
        }
        // A cached snapshot has one owner in FrameCache.  Any additional owner
        // is an external presenter; evicted snapshots have no cache owner.
        if (!cached || snapshot.use_count() > 1) result += snapshot->bytes;
        ++it;
    }
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
void trimCache(quint64 allowed, const FrameCache *keep = nullptr, const ProcessingCache *keepProcessing = nullptr) {
    for (auto it = frames.begin(); residentBytes() > allowed && it != frames.end();) {
        if (&*it == keep) { ++it; continue; }
        it = frames.erase(it);
        ++metrics.evictions;
    }
    // Processing transfer resources use the same budget but do not contribute
    // to the preview-cache eviction counter (which historically counts frames).
    for (auto it = processingCaches.begin(); residentBytes() > allowed && it != processingCaches.end();) {
        if (&*it == keepProcessing) { ++it; continue; }
        it = processingCaches.erase(it);
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
namespace {
struct WarpRequest {
    const QVector<QPointF> *points = nullptr;
    double radius = 0;
    double hardness = 1;
    double strength = 1;
    bool smudge = false;
    const QImage *selection = nullptr;
};

QImage processGpuImage(const QImage &source, const QJsonObject &settings, double scale, int clipping, bool effects,
                       const WarpRequest *warp = nullptr) {
#ifdef Q_OS_WIN
    if (qEnvironmentVariableIntValue("COMPOSITOR_DISABLE_GPU")) {
        clearGpuPreviewCache();
        return {};
    }
    const bool isWarp = warp != nullptr;
    if (source.isNull() || (!isWarp && !effects && GpuProcessor::prefersCpu(settings, clipping))) return {};
    if (isWarp && (warp->points == nullptr || warp->points->isEmpty() ||
                   (warp->selection && !warp->selection->isNull() && warp->selection->size() != source.size())))
        return {};
    // Route before device initialization, cache eviction, uploads or shader compilation.
    const quint64 area=quint64(source.width())*source.height();
    if(area>8*1024*1024) return {};
    std::lock_guard lock(gpuMutex);auto &d=gpu();
    if(!d.ready()) { frames.clear(); processingCaches.clear(); return {}; }
    trimCache(cacheBudget(d));
    // Leave space for processing scratch surfaces in addition to retained previews.
    const quint64 scratch=area*(!isWarp && settings["kind"]=="Camera Raw"?192:96)+3*1024*1024;
    const int depth=wideImage(source)?16:8;
    const auto pixels=source.convertToFormat(depth==16?QImage::Format_RGBA64_Premultiplied:QImage::Format_RGBA8888_Premultiplied);
    if (pixels.isNull()) return {};
    QImage selectionPixels;
    if (isWarp && warp->selection && !warp->selection->isNull()) {
        selectionPixels = warp->selection->convertToFormat(QImage::Format_RGBA8888);
        if (selectionPixels.isNull()) return {};
    }
    const quint64 sampleBytes = depth == 16 ? 8 : 4;
    const quint64 transferBytes = area * sampleBytes * 3;
    auto cacheIt = std::find_if(processingCaches.begin(), processingCaches.end(), [&](const ProcessingCache &cache) {
        return cache.size == pixels.size() && cache.depth == depth && cache.bytes == transferBytes &&
               cache.input.texture && cache.packed.texture && cache.staging;
    });
    const bool cacheHit = cacheIt != processingCaches.end();
    if (cacheHit) processingCaches.splice(processingCaches.end(), processingCaches, cacheIt);
    const quint64 selectionBytes = selectionPixels.isNull() ? 0 : area * 4;
    const quint64 budget=cacheBudget(d);
    metrics.budgetBytes = budget;
    const quint64 transientRequired=scratch+area*32+1024*1024+selectionBytes;
    const quint64 retained = cacheHit ? 0 : transferBytes;
    const quint64 required=transientRequired+retained;
    // The cache entry is already resident on a hit, but it still consumes the
    // same budget while the processor's scratch surfaces are live.
    if(transientRequired+transferBytes>budget) {
        trimCache(budget);
        return {};
    }
    cacheIt = cacheHit ? std::prev(processingCaches.end()) : processingCaches.end();
    if (cacheHit) trimCache(budget-required, nullptr, &*cacheIt);
    else trimCache(budget-required);
    if (!cacheHit) {
        processingCaches.emplace_back();
        cacheIt = std::prev(processingCaches.end());
        cacheIt->size = pixels.size(); cacheIt->depth = depth; cacheIt->bytes = transferBytes;
        const auto inputFormat = depth==16?DXGI_FORMAT_R16G16B16A16_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;
        const auto packedFormat = depth==16?DXGI_FORMAT_R16G16B16A16_UINT:DXGI_FORMAT_R8G8B8A8_UINT;
        if (!createTexture(d,pixels.size(),inputFormat,cacheIt->input,&pixels,true) ||
            !createTexture(d,pixels.size(),packedFormat,cacheIt->packed,nullptr,true)) {
            processingCaches.erase(cacheIt); return {};
        }
        D3D11_TEXTURE2D_DESC desc{}; cacheIt->packed.texture->GetDesc(&desc);
        desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if (FAILED(d.device->CreateTexture2D(&desc,nullptr,&cacheIt->staging))) {
            processingCaches.erase(cacheIt); return {};
        }
        ++metrics.processingTextureAllocations;
        ++metrics.processingUploads; metrics.processingUploadedBytes += area * sampleBytes;
    } else {
        auto &input = cacheIt->input;
        d.context->UpdateSubresource(input.texture.Get(),0,nullptr,pixels.constBits(),UINT(pixels.bytesPerLine()),0);
        ++metrics.processingUploads; metrics.processingUploadedBytes += area * sampleBytes;
        ++metrics.processingCacheHits;
    }
    auto failed = [&]() -> QImage { processingCaches.erase(cacheIt); return {}; };
    Texture selectionTexture;
    if (!selectionPixels.isNull()) {
        if (!createTexture(d,selectionPixels.size(),DXGI_FORMAT_R8G8B8A8_UNORM,selectionTexture,&selectionPixels,true))
            return failed();
        ++metrics.processingUploads; metrics.processingUploadedBytes += selectionBytes;
    }
    d.processor->setScratchBudget(scratch);
    const auto before=d.processor->dispatches;
    auto result = isWarp
        ? d.processor->warp(cacheIt->input.view.Get(), pixels.size(), depth, *warp->points,
                            warp->radius, warp->hardness, warp->strength, warp->smudge,
                            selectionTexture.view.Get())
        : effects ? d.processor->effects(cacheIt->input.view.Get(),pixels.size(),depth,settings,scale)
                  : d.processor->filter(cacheIt->input.view.Get(),pixels.size(),depth,settings,scale,clipping);
    metrics.processingDispatches+=d.processor->dispatches-before;
    if(!result){if(!d.processor->error.isEmpty())d.status=d.processor->error;return failed();}
    Settings params{UINT(pixels.width()),UINT(pixels.height()),0,depth==16?65535.f:255.f,0,0,0};
    d.context->UpdateSubresource(d.settings.Get(),0,nullptr,&params,0,0);
    auto *constants=d.settings.Get();d.context->CSSetConstantBuffers(0,1,&constants);d.context->CSSetShader(d.packShader.Get(),nullptr,0);
    auto *view=result->view.Get();auto *out=cacheIt->packed.output.Get();d.context->CSSetShaderResources(0,1,&view);d.context->CSSetUnorderedAccessViews(0,1,&out,nullptr);
    d.context->Dispatch((params.width+15)/16,(params.height+15)/16,1);
    ID3D11ShaderResourceView *none=nullptr;ID3D11UnorderedAccessView *noOutput=nullptr;
    d.context->CSSetShaderResources(0,1,&none);d.context->CSSetUnorderedAccessViews(0,1,&noOutput,nullptr);
    d.context->CopyResource(cacheIt->staging.Get(),cacheIt->packed.texture.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
    if(FAILED(d.context->Map(cacheIt->staging.Get(),0,D3D11_MAP_READ,0,&mapped)))return failed();
    QImage image(pixels.size(),pixelFormat(depth));
    if(image.isNull()) { d.context->Unmap(cacheIt->staging.Get(),0); return failed(); }
    for(int y=0;y<image.height();++y)std::memcpy(image.scanLine(y),static_cast<const char *>(mapped.pData)+y*mapped.RowPitch,size_t(image.width())*sampleBytes);
    d.context->Unmap(cacheIt->staging.Get(),0);++metrics.readbacks;
    metrics.processingReadbackBytes += area * sampleBytes;
    if (!d.ready()) { frames.clear(); processingCaches.clear(); return {}; }
    image.setColorSpace(source.colorSpace());image.setDotsPerMeterX(source.dotsPerMeterX());image.setDotsPerMeterY(source.dotsPerMeterY());
    return image;
#else
    Q_UNUSED(source);Q_UNUSED(settings);Q_UNUSED(scale);Q_UNUSED(clipping);Q_UNUSED(effects);Q_UNUSED(warp);return {};
#endif
}
}

QImage runGpuNative(const GpuNativeCallback &callback) {
#ifdef Q_OS_WIN
    if (!callback) return {};
    if (qEnvironmentVariableIntValue("COMPOSITOR_DISABLE_GPU")) {
        clearGpuPreviewCache();
        return {};
    }
    std::lock_guard lock(gpuMutex);
    auto &d = gpu();
    if (!d.ready()) {
        frames.clear();
        processingCaches.clear();
        return {};
    }
    // cacheBudget() already caps usage to a fraction of the adapter budget;
    // leave a small transient margin for callback-owned UAVs and descriptors.
    // The callback receives only the free bytes after retained preview/cache
    // resources (including externally retained texture snapshots), so it
    // cannot accidentally allocate a second full cache budget.
    constexpr quint64 transientMargin = 4 * 1024 * 1024;
    const quint64 adapterBudget = cacheBudget(d);
    const quint64 allowed = adapterBudget > transientMargin ? adapterBudget - transientMargin : adapterBudget;
    trimCache(allowed);
    const quint64 retained = residentBytes();
    const quint64 budget = allowed > retained ? allowed - retained : 0;
    metrics.budgetBytes = allowed;
    const auto result = callback(d.device.Get(), d.context.Get(), budget);
    metrics.processingDispatches += result.dispatches;
    if (result.uploadedBytes) ++metrics.processingUploads;
    metrics.processingUploadedBytes += result.uploadedBytes;
    if (result.readbackBytes) ++metrics.readbacks;
    metrics.nativeReadbackOperations += result.readbackOperations;
    metrics.processingReadbackBytes += result.readbackBytes;
    metrics.readbackBytes += result.readbackBytes;
    metrics.externalBytes = externalTextureBytes();
    if (!d.ready()) {
        frames.clear();
        processingCaches.clear();
        return {};
    }
    return result.image;
#else
    Q_UNUSED(callback);
    return {};
#endif
}

QImage filterGpuImage(const QImage &source,const QJsonObject &settings,double scale,int clipping) {
    return processGpuImage(source,settings,scale,clipping,false);
}
QImage effectsGpuImage(const QImage &source,const QJsonObject &settings,double scale) {
    return processGpuImage(source,settings,scale,0,true);
}
QImage warpGpuImage(const QImage &source, const QVector<QPointF> &points, double radius, double hardness,
                    double strength, bool smudge, const QImage &selectionMask) {
    const WarpRequest request{&points, radius, hardness, strength, smudge, &selectionMask};
    return processGpuImage(source, {}, 1, 0, false, &request);
}
QImage effectsAccelerated(const QImage &source,const QJsonObject &settings,double scale) {
    if(settings.isEmpty())return source;
    auto image=effectsGpuImage(source,settings,scale);
    return image.isNull()?applyEffects(source,settings,scale):image;
}
QImage filteredAccelerated(const QImage &source,const QJsonObject &settings,double scale,int clipping) {
    auto result=filterGpuImage(source,settings,scale,clipping);
    if(!result.isNull())return result;
    return settings["kind"]=="Camera Raw"?cameraRaw(source,settings,scale,clipping):filtered(source,settings);
}
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
    result.externalBytes = externalTextureBytes();
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
    processingCaches.clear();
    metrics.externalBytes = externalTextureBytes();
#endif
}
QImage renderGpuPreviewImpl(const State &state, QSize target, QPoint viewportOrigin,
                            std::shared_ptr<GpuPreviewTexture> *textureOut = nullptr,
                            const QHash<QString, std::shared_ptr<GpuNativeTexture>> *sourceOverrides = nullptr) {
#ifdef Q_OS_WIN
    const bool textureOnly = textureOut != nullptr;
    if (textureOut) *textureOut = {};
    auto publishTexture = [&](const std::shared_ptr<GpuNativeTexture> &native) {
        if (!textureOut || !native) return;
        auto result = std::make_shared<GpuPreviewTexture>();
        result->native = native;
        result->size = target;
        result->depth = state.bitDepth;
        *textureOut = std::move(result);
    };
    QByteArray overrideSignature;
    if (sourceOverrides && !sourceOverrides->isEmpty()) {
        QStringList ids = sourceOverrides->keys();
        std::sort(ids.begin(), ids.end());
        QDataStream stream(&overrideSignature, QIODevice::WriteOnly);
        for (const auto &id : ids) {
            const auto native = sourceOverrides->value(id);
            stream << id << qulonglong(reinterpret_cast<quintptr>(native.get()));
        }
    }
    if (qEnvironmentVariableIntValue("COMPOSITOR_DISABLE_GPU")) {
        clearGpuPreviewCache();
        return {};
    }
    if (!target.isValid() || qint64(target.width()) * target.height() > 8 * 1024 * 1024) return {};
    for (const auto &folder : state.layers) {
        if (!folder.group || folder.metadata["effects"].toObject().isEmpty()) continue;
        State nested=state; nested.id=state.id+"/"+folder.id;
        QSet<QString> ids; for(int child:state.descendants(folder.id))ids.insert(state.layers[child].id);
        nested.layers.removeIf([&](const Layer &l){return !ids.contains(l.id);});
        for(auto &layer:nested.layers)if(layer.parent==folder.id)layer.parent.clear();
        if (textureOnly) {
            // Render the folder subtree once as an immutable GPU snapshot,
            // then let the outer pass apply the folder's own effects and blend
            // mode without downloading that subtree to a QImage.
            std::shared_ptr<GpuPreviewTexture> nestedTexture;
            renderGpuPreviewImpl(nested, target, viewportOrigin, &nestedTexture, sourceOverrides);
            if (!nestedTexture || !nestedTexture->native) return {};
            State flattened=state;
            flattened.layers.removeIf([&](const Layer &l){return ids.contains(l.id);});
            auto &flat=flattened.layers[flattened.index(folder.id)];
            flat.group=false;
            flat.image=QImage(QSize(1, 1), pixelFormat(state.bitDepth));
            flat.image.fill(Qt::transparent);
            flat.placement={};
            flat.placement.size=state.size;
            flat.originalPng.clear();
            QHash<QString, std::shared_ptr<GpuNativeTexture>> overrides = sourceOverrides ? *sourceOverrides : QHash<QString, std::shared_ptr<GpuNativeTexture>>();
            overrides.insert(flat.id, nestedTexture->native);
            return renderGpuPreviewImpl(flattened, target, viewportOrigin, textureOut, &overrides);
        }
        const auto image=renderGpuPreviewImpl(nested,target,viewportOrigin);if(image.isNull())return {};
        State flattened=state;flattened.layers.removeIf([&](const Layer &l){return ids.contains(l.id);});
        auto &flat=flattened.layers[flattened.index(folder.id)];flat.group=false;flat.image=image;flat.placement={};flat.placement.size=state.size;flat.originalPng.clear();
        return renderGpuPreviewImpl(flattened,target,viewportOrigin);
    }
    for (const auto &layer : state.layers) {
        const auto effects=layer.metadata["effects"].toObject();
        for(auto it=effects.begin();it!=effects.end();++it)
            if(!QStringList{"stroke","shadow","colorOverlay","innerShadow","outerGlow","innerGlow"}.contains(it.key())||!it.value().isObject())return {};
        if (layer.metadata["adjustment"].isObject()) {
            const auto settings = layer.metadata["adjustment"].toObject();
            if (settings["kind"] != "Gaussian Blur" && !toneAdjustment(settings) && !GpuProcessor::supports(settings["kind"].toString())) return {};
        }
    }
    std::lock_guard lock(gpuMutex);
    auto &d = gpu();
    if (!d.ready()) { frames.clear(); processingCaches.clear(); return {}; }
    QVector<VisibleLayer> visibleLayers;
    QSet<QString> visibleIds;
    bool hasBlur = false;
    int toneLayers = 0;
    bool hasProcessing = false, hasSpatial = false, hasRaw = false;
    int effectLayers=0;
    QByteArray signature, stackSignature;
    QDataStream key(&signature, QIODevice::WriteOnly);
    QDataStream stackKey(&stackSignature, QIODevice::WriteOnly);
    key << state.resolution;
    for (int index : state.drawOrder()) {
        const auto &layer = state.layers[index];
        const bool adjustment = layer.metadata["adjustment"].isObject();
        if (adjustment && layer.blend == "Hard Mix" && !d.doublePrecision) return {};
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
        if (textureOnly && sourceOverrides) {
            const auto override = sourceOverrides->value(layer.id);
            if (override)
                fingerprint.append(QByteArray::number(qulonglong(reinterpret_cast<quintptr>(override.get()))));
        }
        const auto effects=adjustment?QJsonObject():layer.metadata["effects"].toObject();
        if(!effects.isEmpty()) {++effectLayers;QDataStream effectKey(&fingerprint,QIODevice::Append);effectKey<<effects;}
        fingerprint.append(adjustment ? 'A' : 'R');
        visibleLayers.append({index, ancestors, fingerprint, adjustment, opacity});
        hasBlur = hasBlur || (adjustment && layer.metadata["adjustment"].toObject()["kind"] == "Gaussian Blur");
        if (adjustment) {
            const auto kind = layer.metadata["adjustment"].toObject()["kind"].toString();
            hasProcessing = hasProcessing || GpuProcessor::supports(kind);
            hasRaw = hasRaw || kind=="Camera Raw";
            hasSpatial = hasSpatial || (GpuProcessor::supports(kind) && kind!="Hue/Saturation" && kind!="Black & White" && kind!="Color Balance" && kind!="Gradient Map");
        }
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
    const quint64 nativeLayerReserve = textureOnly ? area * 16 * quint64(visibleLayers.size()) : 0;
    const quint64 required = ((hasProcessing||effectLayers)?4*1024*1024:0) + area * (32 + (hasBlur ? 32 : 0) + ((hasProcessing || effectLayers) ? (hasRaw?224:128) : 0) + effectLayers*(16+sampleBytes) + sampleBytes * 3 + visibleLayers.size() * sampleBytes * 2) +
                             toneLayers * tableBytes + (toneLayers ? (state.bitDepth == 16 ? 65536 : 256) * 4 : 0) + nativeLayerReserve;
    const quint64 budget = cacheBudget(d);
    metrics.budgetBytes = budget;
    trimCache(budget);
    if (required > budget) return {};
    d.processor->setScratchBudget(area*(hasRaw?192:96)+3*1024*1024);
    auto found = std::find_if(frames.begin(), frames.end(), [&](const FrameCache &f) {
        return f.document == state.id && f.canvas == state.size && f.target == target &&
               f.origin == viewportOrigin && f.depth == state.bitDepth && f.textureMode == textureOnly &&
               f.overrideSignature == overrideSignature;
    });
    if (found == frames.end()) {
        trimCache(budget - required);
        frames.emplace_back();
        found = std::prev(frames.end());
        found->document = state.id; found->canvas = state.size; found->target = target;
        found->origin = viewportOrigin; found->depth = state.bitDepth; found->textureMode = textureOnly;
        found->overrideSignature = overrideSignature;
    } else {
        frames.splice(frames.end(), frames, found);
        const auto extra = required > found->bytes ? required - found->bytes : 0;
        trimCache(budget - extra, &*found);
    }
    auto &frame = *found;
    const bool forceFull = qEnvironmentVariableIntValue("COMPOSITOR_GPU_FULL_FRAME");
    if (frame.signature != signature || forceFull) frame.textureSnapshot.reset();
    frame.bytes = required + (frame.textureSnapshot ? frame.textureSnapshot->bytes : 0);
    for (auto it = frame.layers.begin(); it != frame.layers.end();)
        if (!visibleIds.contains(it->first)) it = frame.layers.erase(it);
        else ++it;
    if (!forceFull && frame.signature == signature && textureOnly && frame.textureSnapshot) {
        ++metrics.cacheHits;
        ++metrics.noReadbackFrames;
        publishTexture(frame.textureSnapshot);
        return {};
    }
    if (!forceFull && frame.signature == signature && !textureOnly && !frame.result.isNull()) {
        ++metrics.cacheHits;
        return frame.result;
    }
    // A texture-only render intentionally leaves frame.result untouched.  If a
    // normal render follows it, force a fresh readback rather than returning an
    // image from an older GPU signature.
    if (!textureOnly && frame.result.isNull()) {
        frame.textureSnapshot.reset();
        frame.bytes = required;
    }
    auto failed = [&]() -> QImage { frames.erase(found); return {}; };
    if (toneLayers) {
        if (!d.prepareTone()) return failed();
        if (!frame.toneAlpha.texture) {
            const auto table = toneAlphaTable(state.bitDepth);
            if (!createTexture(d, table.size(), DXGI_FORMAT_R32_FLOAT, frame.toneAlpha, &table)) return failed();
        }
    } else frame.toneAlpha = {};
    if (hasProcessing && !d.prepareBlur()) return failed();
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
        if (!textureOnly && (FAILED(d.device->CheckFormatSupport(packedFormat, &support)) ||
            !(support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW))) return failed();
        if (!createTexture(d, target, DXGI_FORMAT_R32G32B32A32_FLOAT, frame.buffers[0]) ||
            !createTexture(d, target, DXGI_FORMAT_R32G32B32A32_FLOAT, frame.buffers[1])) return failed();
        if (!textureOnly && !createTexture(d, target, packedFormat, frame.packed)) return failed();
        if (!textureOnly) {
            D3D11_TEXTURE2D_DESC desc{};
            frame.packed.texture->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ++metrics.textureAllocations;
            if (FAILED(d.device->CreateTexture2D(&desc, nullptr, &frame.staging))) return failed();
        }
    } else if (!textureOnly && !frame.packed.texture) {
        if (!createTexture(d, target, packedFormat, frame.packed)) return failed();
        D3D11_TEXTURE2D_DESC desc{};
        frame.packed.texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ++metrics.textureAllocations;
        if (FAILED(d.device->CreateTexture2D(&desc, nullptr, &frame.staging))) return failed();
    }
    // A changed stack can affect every pixel. Otherwise the prepared surface diff
    // includes both old/new geometry and all mask/clipping dependencies.
    QRect dirty;
    if (frame.result.isNull() || frame.stackSignature != stackSignature || hasBlur || hasSpatial || effectLayers ||
        forceFull) dirty = QRect(QPoint(), target);
    auto nativeSurfaceFrom = [&](const GpuSurfacePtr &surface) {
        if (!surface || !surface->texture || !surface->view) return std::shared_ptr<GpuNativeTexture>();
        auto native = std::make_shared<GpuNativeTexture>();
        native->texture = surface->texture;
        native->shaderResourceView = surface->view;
        native->size = target;
        native->depth = state.bitDepth;
        native->bytes = area * 16;
        return native;
    };
    auto gpuSurfaceFrom = [&](const std::shared_ptr<GpuNativeTexture> &native) {
        if (!native || !native->texture || !native->shaderResourceView) return GpuSurfacePtr();
        auto surface = std::make_shared<GpuSurface>();
        surface->texture = native->texture;
        surface->view = native->shaderResourceView;
        surface->size = target;
        return surface;
    };
    auto nativeCoverage = [&](const VisibleLayer &entry, int index) {
        State coverageState = state;
        auto &root = coverageState.layers[index];
        const Placement originalPlacement = root.placement;
        if (root.maskLinked) root.maskPlacement = originalPlacement;
        root.maskLinked = false;
        root.image = QImage(QSize(1, 1), pixelFormat(state.bitDepth));
        if (root.image.isNull()) return std::shared_ptr<GpuNativeTexture>();
        if (state.bitDepth == 16)
            reinterpret_cast<QRgba64 *>(root.image.bits())[0] = rgba64(65535, 65535, 65535, 65535);
        else
            reinterpret_cast<QRgb *>(root.image.bits())[0] = qRgba(255, 255, 255, 255);
        root.placement = {};
        root.placement.size = state.size;
        root.placement.sampling = QStringLiteral("Nearest");
        root.opacity = 1;
        root.metadata = {};
        for (int ancestor : entry.ancestors)
            coverageState.layers[ancestor].opacity = 1;
        GpuNativeResult nativeStats;
        const quint64 resident = residentBytes();
        const quint64 helperBudget = budget > resident ? budget - resident : 0;
        const auto native = renderGpuLayerSurface(d.device.Get(), d.context.Get(), coverageState, index,
                                                  target, true, false, helperBudget, nativeStats);
        metrics.processingDispatches += nativeStats.dispatches;
        if (nativeStats.uploadedBytes) ++metrics.processingUploads;
        metrics.processingUploadedBytes += nativeStats.uploadedBytes;
        metrics.processingReadbackBytes += nativeStats.readbackBytes;
        return native;
    };
    auto nativeAncestorMask = [&](const VisibleLayer &entry, int index, const GpuSurfacePtr &input) {
        if (!input || entry.ancestors.isEmpty()) return input;
        State maskState = state;
        auto &root = maskState.layers[index];
        root.image = QImage(QSize(1, 1), pixelFormat(state.bitDepth));
        if (root.image.isNull()) return GpuSurfacePtr();
        root.image.fill(Qt::white);
        root.placement = {};
        root.placement.size = state.size;
        root.placement.sampling = QStringLiteral("Nearest");
        root.opacity = 1;
        root.mask = {};
        root.maskEnabled = false;
        root.maskSource.clear();
        root.metadata = {};
        for (int ancestor : entry.ancestors)
            maskState.layers[ancestor].opacity = 1;
        GpuNativeResult nativeStats;
        const quint64 resident = residentBytes();
        const quint64 helperBudget = budget > resident ? budget - resident : 0;
        const auto native = renderGpuLayerSurface(d.device.Get(), d.context.Get(), maskState, index, target,
                                                  true, false, helperBudget, nativeStats, nativeSurfaceFrom(input));
        metrics.processingDispatches += nativeStats.dispatches;
        if (nativeStats.uploadedBytes) ++metrics.processingUploads;
        metrics.processingUploadedBytes += nativeStats.uploadedBytes;
        metrics.processingReadbackBytes += nativeStats.readbackBytes;
        return gpuSurfaceFrom(native);
    };
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
        const bool effected = !entry.adjustment && !layer.metadata["effects"].toObject().isEmpty();
        const bool nativeSource = textureOnly;
        const bool sourceNeedsPrepare = nativeSource
            ? (!cached.nativeSource || cached.nativeSignature != entry.signature)
            : (!cached.source.texture || cached.signature != entry.signature);
        if (sourceNeedsPrepare) {
            ++metrics.preparedLayers;
            if (nativeSource) {
                // Native layer preparation keeps transforms, masks, clipping,
                // and ancestor opacity on the GPU.  Effect layers deliberately
                // omit root/ancestor opacity here; the existing GPU coverage
                // sequence below applies it after the effect kernel.
                std::shared_ptr<GpuNativeTexture> prepared;
                if (entry.adjustment) {
                    // Adjustment coverage is a white synthetic layer.  Its
                    // masks, clipping source, and ancestor masks stay native;
                    // ancestor opacity is intentionally left to entry.opacity
                    // in the adjustment shader below.
                    prepared = nativeCoverage(entry, index);
                } else {
                    GpuNativeResult nativeStats;
                    const auto rootOverride = sourceOverrides ? sourceOverrides->value(layer.id)
                                                              : std::shared_ptr<GpuNativeTexture>();
                    // The frame estimate already reserves retained float outputs;
                    // only currently free adapter/cache bytes are available for
                    // this helper's transient source/mask/intermediate work.
                    const quint64 resident = residentBytes();
                    const quint64 helperBudget = budget > resident ? budget - resident : 0;
                    static const QSet<QString> painterBlendModes{
                        "Normal", "Multiply", "Screen", "Overlay", "Darken", "Lighten", "Difference",
                        "Exclusion", "Color Dodge", "Color Burn", "Soft Light", "Hard Light"};
                    bool coverageOpacity = (!layer.mask.isNull() && layer.maskEnabled) ||
                                           !layer.maskSource.isEmpty() || !painterBlendModes.contains(layer.blend);
                    for (int ancestor : entry.ancestors)
                        coverageOpacity = coverageOpacity ||
                            (!state.layers[ancestor].mask.isNull() && state.layers[ancestor].maskEnabled);
                    // Renderer uses DestinationIn after masks/clipping for a
                    // prepared layer, but a bare painter layer receives the
                    // combined root/folder opacity in a single drawImage.
                    State preparationState = state;
                    if (!effected && !coverageOpacity)
                        preparationState.layers[index].opacity = entry.opacity;
                    prepared = renderGpuLayerSurface(
                        d.device.Get(), d.context.Get(), preparationState, index, target,
                        !effected && coverageOpacity, !effected,
                        helperBudget, nativeStats, rootOverride, !effected && coverageOpacity);
                    metrics.processingDispatches += nativeStats.dispatches;
                    if (nativeStats.uploadedBytes) ++metrics.processingUploads;
                    metrics.processingUploadedBytes += nativeStats.uploadedBytes;
                    metrics.processingReadbackBytes += nativeStats.readbackBytes;
                }
                if (!prepared) return failed();
                cached.nativeSource = prepared;
                cached.nativeSignature = entry.signature;
                cached.source = {};
                cached.pixels = {};
                dirty = QRect(QPoint(), target);
            } else {
                cached.nativeSource.reset();
                cached.nativeSignature.clear();
                State sourceState=state;
                if(effected)sourceState.layers[index].opacity=1;
                auto image = entry.adjustment ? adjustmentCoverage(state, entry, target) : layerSurface(sourceState, index, target);
                if (!entry.adjustment && !effected) {
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
        if(!entry.adjustment&&!layer.metadata["effects"].toObject().isEmpty()) {
            if(cached.effectSignature!=entry.signature||!cached.effected) {
                const auto before=d.processor->dispatches;
                auto *effectInput = textureOnly && cached.nativeSource
                    ? cached.nativeSource->shaderResourceView.Get() : cached.source.view.Get();
                cached.effected=d.processor->effects(effectInput,target,state.bitDepth,layer.metadata["effects"].toObject(),double(target.width())/state.size.width());
                metrics.processingDispatches+=d.processor->dispatches-before;
                if(!cached.effected){d.status=d.processor->error;return failed();}
                // Match reference order: effects, layer opacity, each ancestor mask, then folder opacity.
                if(layer.opacity<1)cached.effected=d.processor->coverage(cached.effected,nullptr,state.bitDepth,layer.opacity);
                double groupOpacity=1;
                for(int ancestor:entry.ancestors) groupOpacity*=state.layers[ancestor].opacity;
                if (textureOnly && !entry.ancestors.isEmpty()) {
                    cached.effected=nativeAncestorMask(entry,index,cached.effected);
                    if(!cached.effected)return failed();
                } else for(int ancestor:entry.ancestors){const auto &group=state.layers[ancestor];
                    if(!group.mask.isNull()&&group.maskEnabled){auto mask=renderMask(group,state.size,target).convertToFormat(state.bitDepth==16?QImage::Format_RGBA64_Premultiplied:QImage::Format_RGBA8888_Premultiplied);
                        Texture texture;if(!createTexture(d,target,state.bitDepth==16?DXGI_FORMAT_R16G16B16A16_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM,texture,&mask))return failed();
                        cached.effected=d.processor->coverage(cached.effected,texture.view.Get(),state.bitDepth,1);}}
                if(groupOpacity<1)cached.effected=d.processor->coverage(cached.effected,nullptr,state.bitDepth,groupOpacity);
                if(!cached.effected)return failed();cached.effectCoverage={};
                cached.effectSignature=entry.signature;
            }
        } else {cached.effected.reset();cached.effectCoverage={};cached.effectSignature.clear();}
    }
    QImage result = textureOnly ? frame.result : std::move(frame.result);
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
        if (visibleLayers.isEmpty()) {
            const float transparent[4]{};
            d.context->ClearUnorderedAccessViewFloat(frame.buffers[current].output.Get(), transparent);
        }
        for (const auto &entry : visibleLayers) {
            const auto &layer = state.layers[entry.index];
            if (entry.adjustment && toneAdjustment(layer.metadata["adjustment"].toObject())) {
                Settings toneParams = params;
                toneParams.mode = UINT(std::max(0, int(blendModes().indexOf(layer.blend))));
                toneParams.padding = std::bit_cast<UINT>(float(entry.opacity));
                d.context->CSSetShader(d.toneShader.Get(), nullptr, 0);
                d.context->UpdateSubresource(d.settings.Get(), 0, nullptr, &toneParams, 0, 0);
                auto *coverage = textureOnly && frame.layers[layer.id].nativeSource
                    ? frame.layers[layer.id].nativeSource->shaderResourceView.Get()
                    : frame.layers[layer.id].source.view.Get();
                ID3D11ShaderResourceView *views[]{frame.buffers[current].view.Get(),
                    frame.layers[layer.id].toneTable.view.Get(), coverage, frame.toneAlpha.view.Get()};
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
                const auto adjustment = layer.metadata["adjustment"].toObject();
                GpuSurfacePtr processed;
                auto *input = frame.buffers[current].view.Get();
                if (GpuProcessor::supports(adjustment["kind"].toString())) {
                    const auto before = d.processor->dispatches;
                    processed = d.processor->filter(input, target, state.bitDepth, adjustment);
                    metrics.processingDispatches += d.processor->dispatches - before;
                    if (!processed) { d.status = d.processor->error; return failed(); }
                    input = processed->view.Get();
                    d.context->CSSetConstantBuffers(0, 1, &constants);
                } else {
                const double radius = layer.metadata["adjustment"].toObject()["blurRadius"].toDouble(10) *
                                      target.width() / state.size.width();
                Settings blurParams{UINT(target.width()), UINT(target.height()), 0, params.maximum,
                                    UINT(std::clamp(qRound(radius * .58), 1, 250)), 0, 0};
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
                }
                Settings mixParams = params;
                mixParams.mode = UINT(std::max(0, int(blendModes().indexOf(layer.blend))));
                mixParams.padding = std::bit_cast<UINT>(float(entry.opacity));
                d.context->CSSetShader(d.blurMixShader.Get(), nullptr, 0);
                d.context->UpdateSubresource(d.settings.Get(), 0, nullptr, &mixParams, 0, 0);
                auto *coverage = textureOnly && frame.layers[layer.id].nativeSource
                    ? frame.layers[layer.id].nativeSource->shaderResourceView.Get()
                    : frame.layers[layer.id].source.view.Get();
                ID3D11ShaderResourceView *views[]{frame.buffers[current].view.Get(), input, coverage};
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
            auto &cached=frame.layers[layer.id];
            params.padding=0;
            d.context->UpdateSubresource(d.settings.Get(),0,nullptr,&params,0,0);
            auto *layerView = cached.effected ? cached.effected->view.Get()
                                              : (textureOnly && cached.nativeSource
                                                     ? cached.nativeSource->shaderResourceView.Get()
                                                     : cached.source.view.Get());
            ID3D11ShaderResourceView *views[]{frame.buffers[current].view.Get(), layerView, cached.effectCoverage.view.Get()};
            d.context->CSSetShaderResources(0, 3, views);
            auto *out = frame.buffers[1-current].output.Get();
            d.context->CSSetUnorderedAccessViews(0, 1, &out, nullptr);
            d.context->Dispatch((params.width+15)/16, (params.height+15)/16, 1);
            ++metrics.dispatches;
            metrics.compositedPixels += pixels;
            unbind();
            current = 1-current;
            params.firstLayer = 0;
        }
        frame.currentBuffer = current;
        if (!textureOnly) {
            // Keep the packed texture and staging allocation resident, but
            // transfer only the dirty rectangle. Empty stacks explicitly write
            // transparency.
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
            if (!d.ready()) { frames.clear(); processingCaches.clear(); return {}; }
        } else {
            // The composited float surface is copied to an immutable SRV.  The
            // frame's ping-pong buffers are reused next frame, so returning one
            // of them directly would expose a mutable resource to the presenter.
            const quint64 snapshotBytes = area * 16;
            if (residentBytes() + snapshotBytes > budget) {
                const quint64 allowed = budget > snapshotBytes ? budget - snapshotBytes : 0;
                trimCache(allowed, &frame);
                if (residentBytes() + snapshotBytes > budget) return failed();
            }
            auto snapshot = std::make_shared<GpuNativeTexture>();
            D3D11_TEXTURE2D_DESC desc{};
            frame.buffers[current].texture->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags = 0;
            desc.MiscFlags = 0;
            if (FAILED(d.device->CreateTexture2D(&desc, nullptr, &snapshot->texture)) ||
                FAILED(d.device->CreateShaderResourceView(snapshot->texture.Get(), nullptr,
                                                          &snapshot->shaderResourceView))) return failed();
            d.context->CopyResource(snapshot->texture.Get(), frame.buffers[current].texture.Get());
            snapshot->size = target;
            snapshot->depth = state.bitDepth;
            snapshot->bytes = snapshotBytes;
            frame.textureSnapshot = snapshot;
            textureSnapshots.emplace_back(snapshot);
            frame.bytes += snapshotBytes;
            ++metrics.textureFrameCopies;
            ++metrics.noReadbackFrames;
            metrics.externalBytes = externalTextureBytes();
            publishTexture(snapshot);
            // This render changed GPU state without producing a CPU image.  A
            // later normal preview must therefore not return an old frame.result.
            if (!dirty.isEmpty()) frame.result = {};
            if (!d.ready()) { frames.clear(); processingCaches.clear(); return {}; }
        }
    } else {
        if (textureOnly) {
            const quint64 snapshotBytes = area * 16;
            if (residentBytes() + snapshotBytes > budget) {
                const quint64 allowed = budget > snapshotBytes ? budget - snapshotBytes : 0;
                trimCache(allowed, &frame);
                if (residentBytes() + snapshotBytes > budget) return failed();
            }
            auto snapshot = std::make_shared<GpuNativeTexture>();
            D3D11_TEXTURE2D_DESC desc{};
            frame.buffers[0].texture->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags = 0;
            desc.MiscFlags = 0;
            if (FAILED(d.device->CreateTexture2D(&desc, nullptr, &snapshot->texture)) ||
                FAILED(d.device->CreateShaderResourceView(snapshot->texture.Get(), nullptr,
                                                          &snapshot->shaderResourceView))) return failed();
            d.context->CopyResource(snapshot->texture.Get(), frame.buffers[frame.currentBuffer].texture.Get());
            snapshot->size = target;
            snapshot->depth = state.bitDepth;
            snapshot->bytes = snapshotBytes;
            frame.textureSnapshot = snapshot;
            textureSnapshots.emplace_back(snapshot);
            frame.bytes += snapshotBytes;
            ++metrics.textureFrameCopies;
            ++metrics.noReadbackFrames;
            metrics.externalBytes = externalTextureBytes();
            publishTexture(snapshot);
        } else {
            ++metrics.cacheHits;
        }
    }
    if (textureOnly) {
        metrics.externalBytes = externalTextureBytes();
        frame.signature = signature;
        frame.stackSignature = stackSignature;
        return {};
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

QImage renderGpuPreview(const State &state, QSize target, QPoint viewportOrigin) {
    return renderGpuPreviewImpl(state, target, viewportOrigin);
}

std::shared_ptr<GpuPreviewTexture> renderGpuTexture(const State &state, QSize target, QPoint viewportOrigin) {
    std::shared_ptr<GpuPreviewTexture> result;
    renderGpuPreviewImpl(state, target, viewportOrigin, &result);
    return result;
}
} // namespace comp
