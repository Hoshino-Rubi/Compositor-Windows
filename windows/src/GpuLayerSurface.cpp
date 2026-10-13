#include "GpuLayerSurface.h"

#ifdef Q_OS_WIN

#include "GpuLayerSurfaceShader.h"
#include "Pixels.h"

#include <QColorSpace>
#include <QPolygonF>
#include <QSet>
#include <QTransform>
#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cmath>
#include <d3dcompiler.h>
#include <limits>
#include <memory>

namespace comp {
namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kLayerFlagsMask = 1;
constexpr UINT kLayerFlagsAuxiliary = 2;
constexpr size_t kConstantVectors = 64;

struct Constants {
    std::array<std::array<float, 4>, kConstantVectors> p{};
};
static_assert(sizeof(Constants) == kConstantVectors * 16);

struct Surface {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11UnorderedAccessView> output;
    QSize size;
};

struct Budget {
    quint64 limit = 0;
    quint64 used = 0;

    bool reserve(quint64 bytes) {
        if (bytes > std::numeric_limits<quint64>::max() - used)
            return false;
        if (limit != std::numeric_limits<quint64>::max() && used + bytes > limit)
            return false;
        used += bytes;
        return true;
    }
};

struct Pipeline {
    ComPtr<ID3D11Device> owner;
    ComPtr<ID3D11ComputeShader> layer, multiplyMask, multiplySurface;
    ComPtr<ID3D11Buffer> constants;

    void reset() {
        owner.Reset();
        layer.Reset();
        multiplyMask.Reset();
        multiplySurface.Reset();
        constants.Reset();
    }

    bool ensure(ID3D11Device *device) {
        if (owner.Get() != device)
            reset();
        if (layer && multiplyMask && multiplySurface && constants)
            return true;
        if (!device)
            return false;
        auto compile = [&](const char *entry, ComPtr<ID3D11ComputeShader> &shader) {
            ComPtr<ID3DBlob> code;
            ComPtr<ID3DBlob> errors;
            const HRESULT compiled = D3DCompile(
                gpuLayerSurfaceShader, sizeof(gpuLayerSurfaceShader) - 1, "CompositorLayerSurface", nullptr, nullptr,
                entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
            if (FAILED(compiled))
                return false;
            return SUCCEEDED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader));
        };
        if (!compile("layer", layer) || !compile("multiplyMask", multiplyMask) ||
            !compile("multiplySurface", multiplySurface)) {
            reset();
            return false;
        }
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = sizeof(Constants);
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device->CreateBuffer(&description, nullptr, &constants))) {
            reset();
            return false;
        }
        owner = device;
        return true;
    }
};

Pipeline &pipeline() {
    // The caller serializes this helper with the process-wide GPU mutex.  A
    // single shader set is therefore sufficient for the process's D3D device.
    static Pipeline value;
    return value;
}

bool checkedBytes(QSize size, quint64 bytesPerPixel, quint64 &bytes) {
    if (!size.isValid() || size.width() <= 0 || size.height() <= 0)
        return false;
    const quint64 pixels = quint64(size.width()) * quint64(size.height());
    if (pixels > std::numeric_limits<quint64>::max() / bytesPerPixel)
        return false;
    bytes = pixels * bytesPerPixel;
    return true;
}

bool createUploadedTexture(ID3D11Device *device, const QImage &image, DXGI_FORMAT format, Budget &budget,
                           Surface &surface, GpuNativeResult &stats) {
    if (!device || image.isNull())
        return false;
    const quint64 bytes = quint64(image.bytesPerLine()) * quint64(image.height());
    if (!budget.reserve(bytes))
        return false;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = UINT(image.width());
    description.Height = UINT(image.height());
    description.MipLevels = description.ArraySize = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = image.constBits();
    data.SysMemPitch = UINT(image.bytesPerLine());
    if (FAILED(device->CreateTexture2D(&description, &data, &surface.texture)) ||
        FAILED(device->CreateShaderResourceView(surface.texture.Get(), nullptr, &surface.view)))
        return false;
    surface.size = image.size();
    stats.uploadedBytes += bytes;
    return true;
}

bool createOutputTexture(ID3D11Device *device, QSize size, Budget &budget, Surface &surface) {
    quint64 bytes = 0;
    if (!device || !checkedBytes(size, 16, bytes) || !budget.reserve(bytes))
        return false;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = UINT(size.width());
    description.Height = UINT(size.height());
    description.MipLevels = description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &surface.texture)) ||
        FAILED(device->CreateShaderResourceView(surface.texture.Get(), nullptr, &surface.view)) ||
        FAILED(device->CreateUnorderedAccessView(surface.texture.Get(), nullptr, &surface.output)))
        return false;
    surface.size = size;
    return true;
}

void setVector(Constants &constants, int index, float x, float y = 0, float z = 0, float w = 0) {
    constants.p[size_t(index)] = {x, y, z, w};
}

QTransform canvasMapping(const Placement &placement, QSize source, QSize canvas, QSize target) {
    QTransform scale;
    scale.scale(double(target.width()) / qMax(1, canvas.width()), double(target.height()) / qMax(1, canvas.height()));
    // QPainter uses row-vector composition here: paintImage() calls scale(S)
    // and then setTransform(P, true), yielding P*S.  Keep the placement and
    // viewport scaling in that order so translated/rotated layers land at the
    // same target coordinates as the reference raster path.
    return placement.matrix(source) * scale;
}

bool setMapping(Constants &constants, int base, const QTransform &mapping, QSize source, bool antialias,
                int nearestMode) {
    bool invertible = false;
    const auto inverse = mapping.inverted(&invertible);
    if (!invertible || !source.isValid() || source.width() <= 0 || source.height() <= 0)
        return false;
    setVector(constants, base, float(inverse.m11()), float(inverse.m21()), float(inverse.m31()));
    setVector(constants, base + 1, float(inverse.m12()), float(inverse.m22()), float(inverse.m32()));
    setVector(constants, base + 2, float(inverse.m13()), float(inverse.m23()), float(inverse.m33()));
    setVector(constants, base + 3, float(source.width()), float(source.height()), antialias ? 1.f : 0.f,
              float(nearestMode));
    const int corner = base == 1 ? 5 : 13;
    const double width = source.width(), height = source.height();
    const QPolygonF rectangle{{0, 0}, {width, 0}, {width, height}, {0, height}};
    const auto corners = mapping.map(rectangle);
    for (int i = 0; i < 4; ++i)
        setVector(constants, corner + i, float(std::trunc(corners[i].x() * 64) / 64),
                  float(std::trunc(corners[i].y() * 64) / 64));

    // QPainter's non-antialiased TxScale image path first snaps the mapped
    // destination rectangle to integer device coordinates, then samples a
    // source index relative to that snapped rectangle.  Preserve that
    // separate fast-path metadata; the generic inverse matrix remains the
    // right rule for rotated/projective mappings.
    const QPointF topLeft = corners[0];
    const QPointF bottomRight = corners[2];
    const int x1 = qRound(qMin(topLeft.x(), bottomRight.x()));
    const int y1 = qRound(qMin(topLeft.y(), bottomRight.y()));
    const int x2 = qRound(qMax(topLeft.x(), bottomRight.x()));
    const int y2 = qRound(qMax(topLeft.y(), bottomRight.y()));
    if (base == 1 && nearestMode == 2) {
        // Qt's 8-bit affine fast fetcher advances truncated 16.16 source
        // coordinates; preserve its near-integer decisions without FP drift.
        setVector(constants, 22, float(int(inverse.m11() * 65536)), float(int(inverse.m21() * 65536)),
                  float(std::ceil((.5 * inverse.m11() + .5 * inverse.m21() + inverse.dx()) * 65536) - 1));
        setVector(constants, 23, float(int(inverse.m12() * 65536)), float(int(inverse.m22() * 65536)),
                  float(std::ceil((.5 * inverse.m12() + .5 * inverse.m22() + inverse.dy()) * 65536) - 1));
    }
    const int metadata = base == 1 ? 18 : 20;
    setVector(constants, metadata, float(x1), float(y1), float(x2 - x1), float(y2 - y1));
    setVector(constants, metadata + 1, mapping.m11() < 0 ? -1.f : 1.f,
              mapping.m22() < 0 ? -1.f : 1.f, float(qRound(mapping.dx())),
              float(qRound(mapping.dy())));
    return true;
}

double quantizedOpacity(double opacity, int depth) {
    const double maximum = depth == 16 ? 65535. : 255.;
    return qRound(std::clamp(opacity, 0., 1.) * maximum) / maximum;
}

// QPainter::setOpacity stores a span alpha in the 0..256 domain and the
// raster composition functions convert it to an 8-bit factor with
// `(alpha256 * 255) >> 8` before multiplying either 8- or 16-bit pixels.
// Keep this separate from QColor::fromRgbF()/DestinationIn coverage, which
// is quantized at the destination image depth.
double quantizedPainterOpacity(double opacity) {
    // The raster span path truncates the floating opacity when it stores the
    // 0..256 constant-alpha value; it does not round it.
    const int alpha256 = int(std::clamp(opacity, 0., 1.) * 256.);
    const int alpha255 = (alpha256 * 255) >> 8;
    return double(alpha255) / 255.;
}

QImage imagePixels(const QImage &image, int depth) {
    return image.convertToFormat(depth == 16 ? QImage::Format_RGBA64_Premultiplied
                                             : QImage::Format_RGBA8888_Premultiplied);
}

QImage maskPixels(const QImage &mask, int depth) {
    return maskImage(mask, depth);
}

bool dispatch(ID3D11DeviceContext *context, ID3D11ComputeShader *shader, ID3D11Buffer *constants,
              const Constants &values, const std::array<ID3D11ShaderResourceView *, 3> &views,
              Surface &output, GpuNativeResult &stats) {
    if (!context || !shader || !constants || !output.output)
        return false;
    context->UpdateSubresource(constants, 0, nullptr, &values, 0, 0);
    context->CSSetConstantBuffers(0, 1, &constants);
    context->CSSetShader(shader, nullptr, 0);
    ID3D11ShaderResourceView *resources[3]{views[0], views[1], views[2]};
    context->CSSetShaderResources(0, 3, resources);
    ID3D11UnorderedAccessView *target = output.output.Get();
    context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
    context->Dispatch((UINT(output.size.width()) + 15) / 16, (UINT(output.size.height()) + 15) / 16, 1);
    ++stats.dispatches;
    ID3D11ShaderResourceView *emptyViews[3]{};
    ID3D11UnorderedAccessView *emptyOutput = nullptr;
    context->CSSetShaderResources(0, 3, emptyViews);
    context->CSSetUnorderedAccessViews(0, 1, &emptyOutput, nullptr);
    context->CSSetShader(nullptr, nullptr, 0);
    return true;
}

void clear(ID3D11DeviceContext *context, Surface &surface) {
    if (!context || !surface.output)
        return;
    const float transparent[4]{0, 0, 0, 0};
    context->ClearUnorderedAccessViewFloat(surface.output.Get(), transparent);
}

std::shared_ptr<GpuNativeTexture> nativeSurface(Surface &&surface, int depth) {
    if (!surface.texture || !surface.view)
        return {};
    auto result = std::make_shared<GpuNativeTexture>();
    result->texture = std::move(surface.texture);
    result->shaderResourceView = std::move(surface.view);
    result->size = surface.size;
    result->depth = depth;
    quint64 bytes = 0;
    if (checkedBytes(surface.size, 16, bytes))
        result->bytes = bytes;
    return result;
}

struct RenderContext {
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    Pipeline *pipeline = nullptr;
    const State &state;
    QSize target;
    int depth = 8;
    double maximum = 255;
    Budget budget;
    GpuNativeResult &stats;
    QSet<int> active;
    int rootIndex = -1;
    std::shared_ptr<GpuNativeTexture> rootSourceOverride;
    bool rootOpacityAsCoverage = false;

    RenderContext(ID3D11Device *d, ID3D11DeviceContext *c, Pipeline *p, const State &s, QSize t, quint64 limit,
                  GpuNativeResult &r, int root, const std::shared_ptr<GpuNativeTexture> &overrideTexture,
                  bool deferRootOpacity)
        : device(d), context(c), pipeline(p), state(s), target(t), depth(s.bitDepth == 16 ? 16 : 8),
          maximum(depth == 16 ? 65535 : 255), budget{limit}, stats(r), rootIndex(root),
          rootSourceOverride(overrideTexture), rootOpacityAsCoverage(deferRootOpacity) {}
};

std::shared_ptr<GpuNativeTexture> renderInternal(RenderContext &render, int index, bool includeAncestors,
                                                  bool includeLayerOpacity);

bool multiplySurface(RenderContext &render, Surface &current, ID3D11ShaderResourceView *auxiliary, double opacity) {
    Surface output;
    if (!createOutputTexture(render.device, render.target, render.budget, output))
        return false;
    Constants constants{};
    setVector(constants, 0, float(render.target.width()), float(render.target.height()), float(render.maximum),
              auxiliary ? float(kLayerFlagsAuxiliary) : 0.f);
    setVector(constants, 17, float(quantizedOpacity(opacity, render.depth)));
    const std::array<ID3D11ShaderResourceView *, 3> views{current.view.Get(), nullptr, auxiliary};
    if (!dispatch(render.context, render.pipeline->multiplySurface.Get(), render.pipeline->constants.Get(), constants,
                  views, output, render.stats))
        return false;
    current = std::move(output);
    return true;
}

bool multiplyMask(RenderContext &render, Surface &current, const QImage &mask, const Placement &placement) {
    const auto pixels = maskPixels(mask, render.depth);
    if (pixels.isNull())
        return false;
    Surface input, output;
    const DXGI_FORMAT format = render.depth == 16 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    if (!createUploadedTexture(render.device, pixels, format, render.budget, input, render.stats) ||
        !createOutputTexture(render.device, render.target, render.budget, output))
        return false;
    Constants constants{};
    setVector(constants, 0, float(render.target.width()), float(render.target.height()), float(render.maximum));
    const auto mapping = canvasMapping(placement, pixels.size(), render.state.size, render.target);
    // QPainter::Antialiasing is never enabled by Renderer::paintImage or
    // renderMask; only SmoothPixmapTransform changes the source sampler.
    // Keep transformed coverage binary with the same top/left half-open edge
    // rule, even when bilinear image/mask taps are selected.
    if (!setMapping(constants, 9, mapping, pixels.size(), false,
                    mapping.type() <= QTransform::TxTranslate ? 1 : 0))
        return false;
    setVector(constants, 17, 1.f);
    const std::array<ID3D11ShaderResourceView *, 3> views{current.view.Get(), input.view.Get(), nullptr};
    if (!dispatch(render.context, render.pipeline->multiplyMask.Get(), render.pipeline->constants.Get(), constants,
                  views, output, render.stats))
        return false;
    current = std::move(output);
    return true;
}

std::shared_ptr<GpuNativeTexture> renderInternal(RenderContext &render, int index, bool includeAncestors,
                                                  bool includeLayerOpacity) {
    if (index < 0 || index >= render.state.layers.size() || render.active.contains(index))
        return {};
    const auto &layer = render.state.layers[index];
    const bool deferRootOpacity = render.rootOpacityAsCoverage && render.rootIndex == index && includeLayerOpacity;
    render.active.insert(index);

    auto fail = [&]() -> std::shared_ptr<GpuNativeTexture> {
        render.active.remove(index);
        return {};
    };

    Surface current;
    const bool useRootOverride = render.rootIndex == index && bool(render.rootSourceOverride);
    if (useRootOverride) {
        const auto &overrideTexture = render.rootSourceOverride;
        if (overrideTexture->size != render.target || overrideTexture->depth != render.depth ||
            !overrideTexture->texture || !overrideTexture->shaderResourceView)
            return fail();
        Surface source;
        source.texture = overrideTexture->texture;
        source.view = overrideTexture->shaderResourceView;
        source.size = render.target;
        if (!createOutputTexture(render.device, render.target, render.budget, current))
            return fail();

        Constants constants{};
        const bool hasMask = !layer.mask.isNull() && layer.maskEnabled;
        setVector(constants, 0, float(render.target.width()), float(render.target.height()), float(render.maximum),
                  hasMask ? float(kLayerFlagsMask) : 0.f);
        QTransform identity;
        identity.scale(1, 1);
        if (!setMapping(constants, 1, identity, render.target, false, 1))
            return fail();
        Surface mask;
        if (hasMask) {
            const auto maskImage = maskPixels(layer.mask, render.depth);
            if (maskImage.isNull() ||
                !createUploadedTexture(render.device,
                                       maskImage,
                                       render.depth == 16 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM,
                                       render.budget, mask, render.stats))
                return fail();
            const auto &placement = layer.maskLinked ? layer.placement : layer.maskPlacement;
            const auto maskMapping = canvasMapping(placement, maskImage.size(), render.state.size, render.target);
            if (!setMapping(constants, 9, maskMapping, maskImage.size(), false,
                            maskMapping.type() <= QTransform::TxTranslate ? 1 : 0))
                return fail();
        }
        setVector(constants, 17,
                  float(includeLayerOpacity && !deferRootOpacity ? quantizedPainterOpacity(layer.opacity) : 1.));
        const std::array<ID3D11ShaderResourceView *, 3> views{source.view.Get(), mask.view.Get(), nullptr};
        if (!dispatch(render.context, render.pipeline->layer.Get(), render.pipeline->constants.Get(), constants, views,
                      current, render.stats))
            return fail();
    } else if (layer.image.isNull()) {
        if (!createOutputTexture(render.device, render.target, render.budget, current))
            return fail();
        clear(render.context, current);
    } else {
        const auto image = imagePixels(layer.image, render.depth);
        Surface source;
        if (image.isNull() ||
            !createUploadedTexture(render.device,
                                   image,
                                   render.depth == 16 ? DXGI_FORMAT_R16G16B16A16_UNORM
                                                      : DXGI_FORMAT_R8G8B8A8_UNORM,
                                   render.budget, source, render.stats) ||
            !createOutputTexture(render.device, render.target, render.budget, current))
            return fail();

        Constants constants{};
        const bool hasMask = !layer.mask.isNull() && layer.maskEnabled;
        setVector(constants, 0, float(render.target.width()), float(render.target.height()), float(render.maximum),
                  hasMask ? float(kLayerFlagsMask) : 0.f);
        const auto imageMapping = canvasMapping(layer.placement, image.size(), render.state.size, render.target);
        const int nearestMode = imageMapping.type() <= QTransform::TxTranslate
            ? 1
            : (layer.placement.sampling == "Nearest"
                   ? (imageMapping.type() == QTransform::TxScale && render.depth == 8 ? 3 : 2)
                   : 0);
        if (!setMapping(constants, 1, imageMapping, image.size(), false, nearestMode))
            return fail();

        Surface mask;
        if (hasMask) {
            const auto maskImage = maskPixels(layer.mask, render.depth);
            if (maskImage.isNull() ||
                !createUploadedTexture(render.device,
                                       maskImage,
                                       render.depth == 16 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM,
                                       render.budget, mask, render.stats))
                return fail();
            const auto &placement = layer.maskLinked ? layer.placement : layer.maskPlacement;
            const auto maskMapping = canvasMapping(placement, maskImage.size(), render.state.size, render.target);
            if (!setMapping(constants, 9, maskMapping, maskImage.size(), false,
                            maskMapping.type() <= QTransform::TxTranslate ? 1 : 0))
                return fail();
        }
        setVector(constants, 17,
                  float(includeLayerOpacity && !deferRootOpacity ? quantizedPainterOpacity(layer.opacity) : 1.));
        const std::array<ID3D11ShaderResourceView *, 3> views{source.view.Get(), mask.view.Get(), nullptr};
        if (!dispatch(render.context, render.pipeline->layer.Get(), render.pipeline->constants.Get(), constants, views,
                      current, render.stats))
            return fail();
    }

    // layerSurface() multiplies a clipping source's own prepared surface, not
    // its folder ancestors.  Render that source recursively with its own
    // opacity/mask and guard malformed projects containing a clipping cycle.
    const int sourceIndex = render.state.index(layer.maskSource);
    if (sourceIndex >= 0 && !render.active.contains(sourceIndex)) {
        const auto clipping = renderInternal(render, sourceIndex, false, true);
        if (!clipping || !multiplySurface(render, current, clipping->shaderResourceView.Get(), 1.))
            return fail();
    }

    // The live-effects path applies the root layer opacity to the rendered
    // coverage after its own mask/clipping chain, but before ancestor masks
    // and group opacity.  Clipping recursion remains on the painter-opacity
    // path above because it represents an independent source layer.
    if (deferRootOpacity && std::abs(layer.opacity - 1.) > 1e-12 &&
        !multiplySurface(render, current, nullptr, layer.opacity))
        return fail();

    if (includeAncestors) {
        QVector<int> ancestors;
        QSet<int> visited;
        QString parent = layer.parent;
        bool hidden = false;
        while (!parent.isEmpty()) {
            const int ancestor = render.state.index(parent);
            if (ancestor < 0 || visited.contains(ancestor))
                break;
            visited.insert(ancestor);
            ancestors.append(ancestor);
            const auto &group = render.state.layers[ancestor];
            hidden = hidden || !group.visible;
            parent = group.parent;
        }
        if (hidden) {
            clear(render.context, current);
        } else {
            double groupOpacity = 1;
            for (int ancestor : ancestors) {
                const auto &group = render.state.layers[ancestor];
                groupOpacity *= group.opacity;
                if (!group.mask.isNull() && group.maskEnabled) {
                    const auto &placement = group.maskLinked ? group.placement : group.maskPlacement;
                    if (!multiplyMask(render, current, group.mask, placement))
                        return fail();
                }
            }
            if (std::abs(groupOpacity - 1) > 1e-12 && !multiplySurface(render, current, nullptr, groupOpacity))
                return fail();
        }
    }

    render.active.remove(index);
    return nativeSurface(std::move(current), render.depth);
}

} // namespace

std::shared_ptr<GpuNativeTexture> renderGpuLayerSurface(
    ID3D11Device *device, ID3D11DeviceContext *context, const State &state, int index, QSize target,
    bool includeAncestors, bool includeLayerOpacity, quint64 budget, GpuNativeResult &stats,
    const std::shared_ptr<GpuNativeTexture> &rootSourceOverride, bool rootOpacityAsCoverage) {
    if (!device || !context || !target.isValid() || target.width() <= 0 || target.height() <= 0 ||
        state.size.width() <= 0 || state.size.height() <= 0 || index < 0 || index >= state.layers.size())
        return {};
    // A zero budget is an explicit no-resource request.  The callback's
    // budget is aggregate, so every source/mask upload and intermediate output
    // is charged before creation instead of relying on driver allocation
    // failures as a budget check.
    if (budget == 0)
        return {};
    auto &shaders = pipeline();
    if (!shaders.ensure(device))
        return {};
    try {
        RenderContext render(device, context, &shaders, state, target, budget, stats, index, rootSourceOverride,
                             rootOpacityAsCoverage);
        if (rootSourceOverride) {
            quint64 overrideBytes = rootSourceOverride->bytes;
            if (overrideBytes == 0 && !checkedBytes(target, 16, overrideBytes))
                return {};
            if (!render.budget.reserve(overrideBytes))
                return {};
        }
        return renderInternal(render, index, includeAncestors, includeLayerOpacity);
    } catch (...) {
        // Match the GPU preview contract: any unsupported/oversized source
        // simply asks the caller to route through the reference CPU renderer.
        return {};
    }
}

} // namespace comp

#else

namespace comp {
std::shared_ptr<GpuNativeTexture> renderGpuLayerSurface(
    void *, void *, const State &, int, QSize, bool, bool, quint64, GpuNativeResult &,
    const std::shared_ptr<GpuNativeTexture> &, bool) {
    return {};
}
} // namespace comp

#endif
