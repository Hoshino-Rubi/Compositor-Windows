#include "GpuPaint.h"

#include "GpuNative.h"
#include <QHash>

#ifdef Q_OS_WIN
#include "GpuPaintShader.h"
#include "Pixels.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace comp {
namespace {
using Microsoft::WRL::ComPtr;

constexpr int MaxTextureDimension = 16384;
constexpr quint64 MaxPaintPixels = 8ull * 1024 * 1024;

struct Texture {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11UnorderedAccessView> output;
};

struct DabInfo {
    std::int32_t targetX = 0;
    std::int32_t targetY = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t atlasX = 0;
    std::int32_t atlasY = 0;
    std::int32_t reserved0 = 0;
    std::int32_t reserved1 = 0;
    float sourceX = 0;
    float sourceY = 0;
    float flow = 1;
    float reserved2 = 0;
};
static_assert(sizeof(DabInfo) == 48);

struct PaintSettings {
    UINT width = 0;
    UINT height = 0;
    UINT dabCount = 0;
    UINT operation = 0;
    float maximum = 255;
    UINT sourceWidth = 1;
    UINT sourceHeight = 1;
    UINT hasSource = 0;
    UINT hasSelection = 0;
    UINT selectionWidth = 1;
    UINT selectionHeight = 1;
    // Reuses the old maskTarget slot in the native cbuffer.  Bit 0 selects
    // source filtering and bit 1 selects Qt integer-opacity compatibility;
    // bits 2/3 permit direct BGRA target/source uploads.
    UINT sourceFlags = 0;
    float colorR = 0;
    float colorG = 0;
    float colorB = 0;
    float colorA = 1;
};
static_assert(sizeof(PaintSettings) == 64);

struct PreparedDab {
    QRect target;
    QImage coverage;
    QPointF sourceOffset;
    int atlasX = 0;
    int atlasY = 0;
    int coverageOwner = -1;
};

bool finitePoint(const QPointF &point) {
    return std::isfinite(point.x()) && std::isfinite(point.y());
}

bool createTexture(ID3D11Device *device, QSize size, DXGI_FORMAT format, Texture &out, const QImage *image,
                   bool unordered, QPoint origin = {}) {
    if (!device || !size.isValid() || size.width() > MaxTextureDimension || size.height() > MaxTextureDimension)
        return false;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = UINT(size.width());
    desc.Height = UINT(size.height());
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (unordered ? D3D11_BIND_UNORDERED_ACCESS : 0);
    D3D11_SUBRESOURCE_DATA data{};
    if (image) {
        data.pSysMem = image->constScanLine(origin.y()) + qsizetype(origin.x()) * (image->depth() / 8);
        data.SysMemPitch = UINT(image->bytesPerLine());
    }
    if (FAILED(device->CreateTexture2D(&desc, image ? &data : nullptr, &out.texture)) ||
        FAILED(device->CreateShaderResourceView(out.texture.Get(), nullptr, &out.view)))
        return false;
    if (unordered && FAILED(device->CreateUnorderedAccessView(out.texture.Get(), nullptr, &out.output)))
        return false;
    return true;
}

bool createStructuredBuffer(ID3D11Device *device, const std::vector<DabInfo> &values,
                            ComPtr<ID3D11Buffer> &buffer, ComPtr<ID3D11ShaderResourceView> &view) {
    if (!device || values.empty() || values.size() > std::numeric_limits<UINT>::max() / sizeof(DabInfo))
        return false;
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = UINT(values.size() * sizeof(DabInfo));
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = sizeof(DabInfo);
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = values.data();
    if (FAILED(device->CreateBuffer(&desc, &data, &buffer)))
        return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
    viewDesc.Format = DXGI_FORMAT_UNKNOWN;
    viewDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    viewDesc.Buffer.NumElements = UINT(values.size());
    return SUCCEEDED(device->CreateShaderResourceView(buffer.Get(), &viewDesc, &view));
}

bool createStaging(ID3D11Device *device, const Texture &source, ComPtr<ID3D11Texture2D> &staging) {
    if (!device || !source.texture)
        return false;
    D3D11_TEXTURE2D_DESC desc{};
    source.texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging));
}

bool prepareShader(ID3D11Device *device, ComPtr<ID3D11ComputeShader> &shader) {
    static ID3D11Device *cachedDevice = nullptr;
    static ComPtr<ID3D11ComputeShader> cachedShader;
    if (cachedDevice != device) {
        cachedDevice = device;
        cachedShader.Reset();
    }
    if (!cachedShader) {
        ComPtr<ID3DBlob> code;
        ComPtr<ID3DBlob> errors;
        const HRESULT hr = D3DCompile(gpuPaintShader, sizeof(gpuPaintShader) - 1, "CompositorPaint", nullptr, nullptr,
                                      "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (FAILED(hr) || FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                                              &cachedShader)))
            return false;
    }
    shader = cachedShader;
    return true;
}

bool prepareConstants(ID3D11Device *device, ComPtr<ID3D11Buffer> &constants) {
    static ID3D11Device *cachedDevice = nullptr;
    static ComPtr<ID3D11Buffer> cachedConstants;
    if (cachedDevice != device) {
        cachedDevice = device;
        cachedConstants.Reset();
    }
    if (!cachedConstants) {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = sizeof(PaintSettings);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device->CreateBuffer(&desc, nullptr, &cachedConstants)))
            return false;
    }
    constants = cachedConstants;
    return true;
}

QImage canonical(const QImage &image, int depth) {
    return image.convertToFormat(depth == 16 ? QImage::Format_RGBA64_Premultiplied
                                             : QImage::Format_RGBA8888_Premultiplied);
}

QImage canonicalCoverage(const QImage &image, int depth) {
    return image.convertToFormat(depth == 16 ? QImage::Format_Grayscale16 : QImage::Format_Grayscale8);
}

bool nativePixelLayout(const QImage &image, int depth) {
    return depth == 16 ? image.format() == QImage::Format_RGBA64_Premultiplied
                       : image.format() == QImage::Format_RGBA8888_Premultiplied ||
                             image.format() == QImage::Format_ARGB32_Premultiplied;
}

bool checkedArea(QSize size, quint64 &area) {
    if (!size.isValid() || size.width() <= 0 || size.height() <= 0)
        return false;
    const quint64 width = quint64(size.width()), height = quint64(size.height());
    if (height > std::numeric_limits<quint64>::max() / width)
        return false;
    area = width * height;
    return area <= MaxPaintPixels;
}

GpuNativeResult paintNative(const QImage &target, const QImage &source, const QVector<GpuPaintDab> &dabs,
                            const GpuPaintOptions &options, ID3D11Device *device, ID3D11DeviceContext *context,
                            quint64 budget) {
    GpuNativeResult failed;
    if (!device || !context || target.isNull() || dabs.isEmpty())
        return failed;

    const int depth = options.depth == 16 || (options.depth == 0 && wideImage(target)) ? 16 : 8;
    if (options.depth != 0 && options.depth != 8 && options.depth != 16)
        return failed;
    const auto operation = options.operation;
    if (operation != GpuPaintOperation::Brush && operation != GpuPaintOperation::Eraser &&
        operation != GpuPaintOperation::Clone && operation != GpuPaintOperation::Blur)
        return failed;
    if (!std::isfinite(options.flow))
        return failed;
    const double flow = std::clamp(options.flow, 0., 1.);
    const bool needsSource = operation == GpuPaintOperation::Clone || operation == GpuPaintOperation::Blur;
    if (needsSource && source.isNull())
        return failed;
    if (target.width() > MaxTextureDimension || target.height() > MaxTextureDimension)
        return failed;

    QRect targetRoi;
    QVector<PreparedDab> prepared;
    prepared.reserve(dabs.size());
    QHash<qint64, QImage> convertedCoverage;
    const double coordinateLimit = double(std::numeric_limits<int>::max()) / 4.;
    for (const auto &dab : dabs) {
        if (!dab.coverage.isNull() && dab.targetRect.size() == dab.coverage.size() && dab.targetRect.width() > 0 &&
            dab.targetRect.height() > 0 && finitePoint(dab.sourceOffset) &&
            std::abs(dab.sourceOffset.x()) <= coordinateLimit && std::abs(dab.sourceOffset.y()) <= coordinateLimit) {
            const QRect clipped = dab.targetRect.intersected(target.rect());
            if (clipped.isEmpty())
                continue;
            PreparedDab item;
            item.target = dab.targetRect;
            auto cached = convertedCoverage.constFind(dab.coverage.cacheKey());
            if (cached == convertedCoverage.cend()) {
                item.coverage = canonicalCoverage(dab.coverage, depth);
                convertedCoverage.insert(dab.coverage.cacheKey(), item.coverage);
            } else {
                item.coverage = cached.value();
            }
            item.sourceOffset = dab.sourceOffset;
            if (item.coverage.isNull())
                return failed;
            targetRoi = targetRoi.isEmpty() ? clipped : targetRoi.united(clipped);
            prepared.push_back(std::move(item));
        } else {
            return failed;
        }
    }
    if (prepared.isEmpty() || targetRoi.isEmpty() || targetRoi.width() > MaxTextureDimension ||
        targetRoi.height() > MaxTextureDimension)
        return failed;

    quint64 roiPixels = 0;
    if (!checkedArea(targetRoi.size(), roiPixels))
        return failed;

    const int sampleBytes = depth == 16 ? 8 : 4;

    QRect sourceRoi;
    if (needsSource) {
        for (const auto &dab : prepared) {
            const QRectF sourceBounds(QPointF(dab.target.left(), dab.target.top()) + dab.sourceOffset - QPointF(1, 1),
                                      QSizeF(dab.target.width() + 2, dab.target.height() + 2));
            const QRect clipped = sourceBounds.toAlignedRect().intersected(source.rect());
            if (!clipped.isEmpty())
                sourceRoi = sourceRoi.isEmpty() ? clipped : sourceRoi.united(clipped);
        }
        if (sourceRoi.isEmpty())
            sourceRoi = QRect(0, 0, 1, 1);
    } else {
        sourceRoi = QRect(0, 0, 1, 1);
    }
    if (sourceRoi.width() > MaxTextureDimension || sourceRoi.height() > MaxTextureDimension)
        return failed;

    // Pack all CPU-generated coverage rasters into one read-only atlas.  A
    // shelf layout keeps upload/readback count independent of dab count.
    const QImage::Format coverageFormat = depth == 16 ? QImage::Format_Grayscale16 : QImage::Format_Grayscale8;
    int maxWidth = 1;
    QHash<qint64, int> coverageOwners;
    std::vector<int> uniqueCoverage;
    for (int i = 0; i < prepared.size(); ++i) {
        auto &dab = prepared[i];
        const auto found = coverageOwners.constFind(dab.coverage.cacheKey());
        if (found != coverageOwners.cend()) {
            dab.coverageOwner = found.value();
        } else {
            coverageOwners.insert(dab.coverage.cacheKey(), i);
            uniqueCoverage.push_back(i);
            maxWidth = std::max(maxWidth, dab.coverage.width());
        }
    }
    if (maxWidth > MaxTextureDimension)
        return failed;
    // Keep the atlas close to the actual coverage footprint.  A fixed 4096px
    // row would make a long sequence of small dabs exceed the transfer cap
    // even though their total coverage is tiny.
    const int atlasWidth = maxWidth;
    int atlasX = 0, atlasY = 0, rowHeight = 0;
    for (const auto i : uniqueCoverage) {
        auto &dab = prepared[i];
        if (atlasX + dab.coverage.width() > atlasWidth) {
            atlasX = 0;
            atlasY += rowHeight;
            rowHeight = 0;
        }
        if (atlasY + dab.coverage.height() > MaxTextureDimension)
            return failed;
        dab.atlasX = atlasX;
        dab.atlasY = atlasY;
        atlasX += dab.coverage.width();
        rowHeight = std::max(rowHeight, dab.coverage.height());
    }
    for (auto &dab : prepared)
        if (dab.coverageOwner >= 0) {
            dab.atlasX = prepared[dab.coverageOwner].atlasX;
            dab.atlasY = prepared[dab.coverageOwner].atlasY;
        }
    const QSize atlasSize(atlasWidth, std::max(1, atlasY + rowHeight));
    quint64 atlasPixels = 0;
    if (!checkedArea(atlasSize, atlasPixels))
        return failed;
    QImage atlas(atlasSize, coverageFormat);
    if (atlas.isNull())
        return failed;
    atlas.fill(0);
    const int coverageBytes = depth == 16 ? 2 : 1;
    for (const auto i : uniqueCoverage) {
        const auto &dab = prepared[i];
        for (int y = 0; y < dab.coverage.height(); ++y)
            std::memcpy(atlas.scanLine(dab.atlasY + y) + dab.atlasX * coverageBytes,
                        dab.coverage.constScanLine(y), size_t(dab.coverage.width()) * coverageBytes);
    }

    QImage targetPixels = target;
    QRect targetUpload = targetRoi;
    if (!nativePixelLayout(target, depth)) {
        targetPixels = canonical(target.copy(targetRoi), depth);
        targetUpload = targetPixels.rect();
    }
    if (targetPixels.isNull())
        return failed;
    QImage sourcePixels;
    QRect sourceUpload;
    if (needsSource) {
        sourcePixels = source;
        sourceUpload = sourceRoi;
        if (!nativePixelLayout(source, depth)) {
            sourcePixels = canonical(source.copy(sourceRoi), depth);
            sourceUpload = sourcePixels.rect();
        }
        if (sourcePixels.isNull())
            return failed;
    }

    QImage selectionPixels;
    QRect selectionUpload;
    const bool hasSelection = !options.selectionMask.isNull();
    if (hasSelection) {
        if (options.selectionMask.size() != target.size())
            return failed;
        selectionPixels = options.selectionMask;
        selectionUpload = targetRoi;
        if (selectionPixels.format() != coverageFormat) {
            selectionPixels = options.selectionMask.copy(targetRoi).convertToFormat(coverageFormat);
            selectionUpload = selectionPixels.rect();
        }
        if (selectionPixels.isNull())
            return failed;
    }

    std::vector<DabInfo> metadata;
    metadata.reserve(size_t(prepared.size()));
    const QPoint targetOrigin = targetRoi.topLeft();
    const QPoint sourceOrigin = sourceRoi.topLeft();
    for (const auto &dab : prepared) {
        DabInfo info;
        info.targetX = dab.target.x() - targetOrigin.x();
        info.targetY = dab.target.y() - targetOrigin.y();
        info.width = dab.target.width();
        info.height = dab.target.height();
        info.atlasX = dab.atlasX;
        info.atlasY = dab.atlasY;
        const QPointF sourceAdjust = dab.sourceOffset + QPointF(targetOrigin - sourceOrigin);
        info.sourceX = float(sourceAdjust.x());
        info.sourceY = float(sourceAdjust.y());
        info.flow = float(flow);
        if (!std::isfinite(info.sourceX) || !std::isfinite(info.sourceY))
            return failed;
        metadata.push_back(info);
    }

    quint64 sourcePixelsCount = 0;
    if (needsSource && !checkedArea(sourceUpload.size(), sourcePixelsCount))
        return failed;
    const quint64 targetBytes = roiPixels * quint64(sampleBytes);
    const quint64 sourceBytes = sourcePixelsCount * quint64(sampleBytes);
    const quint64 atlasBytes = atlasPixels * quint64(coverageBytes);
    const quint64 selectionBytes = hasSelection ? roiPixels * quint64(coverageBytes) : 0;
    const quint64 metadataBytes = quint64(metadata.size() * sizeof(DabInfo));
    const quint64 outputBytes = targetBytes;
    const quint64 required = targetBytes + sourceBytes + atlasBytes + selectionBytes + metadataBytes + outputBytes * 2;
    if (required > budget)
        return failed;

    const DXGI_FORMAT pixelTextureFormat = depth == 16 ? DXGI_FORMAT_R16G16B16A16_UINT : DXGI_FORMAT_R8G8B8A8_UINT;
    const DXGI_FORMAT coverageTextureFormat = depth == 16 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    Texture targetTexture, sourceTexture, coverageTexture, selectionTexture, outputTexture;
    if (!createTexture(device, targetUpload.size(), pixelTextureFormat, targetTexture, &targetPixels, false,
                       targetUpload.topLeft()) ||
        (needsSource && !createTexture(device, sourceUpload.size(), pixelTextureFormat, sourceTexture, &sourcePixels,
                                      false, sourceUpload.topLeft())) ||
        !createTexture(device, atlas.size(), coverageTextureFormat, coverageTexture, &atlas, false) ||
        (hasSelection && !createTexture(device, selectionUpload.size(), coverageTextureFormat, selectionTexture,
                                       &selectionPixels, false, selectionUpload.topLeft())) ||
        !createTexture(device, targetRoi.size(), pixelTextureFormat, outputTexture, nullptr, true))
        return failed;
    ComPtr<ID3D11Texture2D> staging;
    if (!createStaging(device, outputTexture, staging))
        return failed;
    ComPtr<ID3D11Buffer> metadataBuffer;
    ComPtr<ID3D11ShaderResourceView> metadataView;
    if (!createStructuredBuffer(device, metadata, metadataBuffer, metadataView))
        return failed;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> constants;
    if (!prepareShader(device, shader) || !prepareConstants(device, constants))
        return failed;

    PaintSettings settings;
    settings.width = UINT(targetRoi.width());
    settings.height = UINT(targetRoi.height());
    settings.dabCount = UINT(metadata.size());
    settings.operation = UINT(operation);
    settings.maximum = depth == 16 ? 65535.f : 255.f;
    settings.sourceWidth = UINT(sourceUpload.width());
    settings.sourceHeight = UINT(sourceUpload.height());
    settings.hasSource = needsSource ? 1u : 0u;
    settings.hasSelection = hasSelection ? 1u : 0u;
    settings.selectionWidth = UINT(selectionUpload.width());
    settings.selectionHeight = UINT(selectionUpload.height());
    // Keep the cbuffer ABI stable: its former maskTarget slot is now the
    // source filtering, Qt opacity and native channel layout. The maskTarget flag controls
    // color preparation in the Canvas wrapper and needs no shader branch.
    settings.sourceFlags = (options.smoothSource ? 1u : 0u) | (options.qtOpacity ? 2u : 0u) |
                           (targetPixels.format() == QImage::Format_ARGB32_Premultiplied ? 4u : 0u) |
                           (sourcePixels.format() == QImage::Format_ARGB32_Premultiplied ? 8u : 0u);
    QColor color = options.color;
    if (!color.isValid())
        color = Qt::black;
    settings.colorR = float(color.redF());
    settings.colorG = float(color.greenF());
    settings.colorB = float(color.blueF());
    settings.colorA = float(color.alphaF());
    context->UpdateSubresource(constants.Get(), 0, nullptr, &settings, 0, 0);
    auto *constantBuffer = constants.Get();
    context->CSSetConstantBuffers(0, 1, &constantBuffer);
    context->CSSetShader(shader.Get(), nullptr, 0);
    ID3D11ShaderResourceView *views[]{targetTexture.view.Get(), sourceTexture.view.Get(), coverageTexture.view.Get(),
                                      selectionTexture.view.Get(), metadataView.Get()};
    context->CSSetShaderResources(0, 5, views);
    auto *output = outputTexture.output.Get();
    context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    context->Dispatch((settings.width + 15) / 16, (settings.height + 15) / 16, 1);
    ID3D11ShaderResourceView *noneViews[5]{};
    ID3D11UnorderedAccessView *noneOutput = nullptr;
    context->CSSetShaderResources(0, 5, noneViews);
    context->CSSetUnorderedAccessViews(0, 1, &noneOutput, nullptr);
    context->CSSetShader(nullptr, nullptr, 0);
    context->CopyResource(staging.Get(), outputTexture.texture.Get());

    // Keep only the unavoidable immutable-result copy. For native layouts,
    // staging rows go straight into the final ROI: no patch image, channel
    // conversion or second CPU patch copy.
    QImage result = target.copy();
    const bool directOutput = nativePixelLayout(target, depth);
    QImage patch;
    if (!directOutput)
        patch = QImage(targetRoi.size(), targetPixels.format());
    if (result.isNull() || (!directOutput && patch.isNull()))
        return failed;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return failed;
    const size_t rowBytes = size_t(targetRoi.width()) * size_t(sampleBytes);
    const bool validPitch = mapped.RowPitch >= rowBytes;
    if (validPitch)
        for (int y = 0; y < targetRoi.height(); ++y) {
            auto *destination = directOutput ? result.scanLine(targetRoi.y() + y) + size_t(targetRoi.x()) * sampleBytes
                                             : patch.scanLine(y);
            std::memcpy(destination, static_cast<const char *>(mapped.pData) + size_t(y) * mapped.RowPitch, rowBytes);
        }
    context->Unmap(staging.Get(), 0);
    if (!validPitch)
        return failed;
    if (!directOutput) {
        patch = patch.convertToFormat(target.format());
        const int bytesPerPixel = target.depth() / 8;
        if (patch.isNull() || bytesPerPixel <= 0)
            return failed;
        for (int y = 0; y < patch.height(); ++y)
            std::memcpy(result.scanLine(targetRoi.y() + y) + size_t(targetRoi.x()) * bytesPerPixel,
                        patch.constScanLine(y), size_t(targetRoi.width()) * bytesPerPixel);
    }

    failed.image = result;
    failed.dispatches = 1;
    failed.uploadedBytes = targetBytes + sourceBytes + atlasBytes + selectionBytes + metadataBytes;
    failed.readbackBytes = outputBytes;
    failed.readbackOperations = 1;
    return failed;
}

} // namespace

QImage paintGpuImage(const QImage &target, const QImage &source, const QVector<GpuPaintDab> &dabs,
                     const GpuPaintOptions &options) {
    if (target.isNull() || dabs.isEmpty() || qEnvironmentVariableIntValue("COMPOSITOR_DISABLE_GPU"))
        return {};
    const auto callback = [&](ID3D11Device *device, ID3D11DeviceContext *context, quint64 budget) {
        return paintNative(target, source, dabs, options, device, context, budget);
    };
    return runGpuNative(callback);
}

} // namespace comp

#else

namespace comp {
QImage paintGpuImage(const QImage &, const QImage &, const QVector<GpuPaintDab> &, const GpuPaintOptions &) {
    return {};
}
} // namespace comp

#endif
