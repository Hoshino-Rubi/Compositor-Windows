#include "GpuPerspective.h"

#include "GpuNative.h"
#include "GpuPerspectiveShader.h"
#include "GpuRasterGeometry.h"
#include "Pixels.h"
#include "Warp.h"

#include <QTransform>
#include <algorithm>
#include <cmath>
#include <limits>

#ifdef Q_OS_WIN
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#endif

namespace comp {
namespace {

constexpr quint64 GpuMaxPixels = 8ull * 1024 * 1024;

bool finitePoint(const QPointF &point) {
    return std::isfinite(point.x()) && std::isfinite(point.y());
}

bool finiteTransform(const QTransform &transform) {
    return std::isfinite(transform.m11()) && std::isfinite(transform.m12()) &&
           std::isfinite(transform.m13()) && std::isfinite(transform.m21()) &&
           std::isfinite(transform.m22()) && std::isfinite(transform.m23()) &&
           std::isfinite(transform.m31()) && std::isfinite(transform.m32()) &&
           std::isfinite(transform.m33());
}

bool checkedArea(QSize size, quint64 &area) {
    if (!size.isValid() || size.width() <= 0 || size.height() <= 0)
        return false;
    const auto width = quint64(size.width());
    const auto height = quint64(size.height());
    if (height != 0 && width > std::numeric_limits<quint64>::max() / height)
        return false;
    area = width * height;
    return true;
}

void copyMetadata(QImage &result, const QImage &source) {
    result.setColorSpace(source.colorSpace());
    result.setDotsPerMeterX(source.dotsPerMeterX());
    result.setDotsPerMeterY(source.dotsPerMeterY());
}

struct PerspectiveGeometry {
    QTransform inverse;
    QTransform forward;
    QPolygonF corners;
    QRect bounds;
    QSize outputSize;
    int depth = 8;
};

// Construct exactly the geometry used by Warp.cpp/WideWarp.cpp.  The source
// quadrilateral uses image extents (width/height), while the output transform
// is translated into the integer bounding-box coordinate system.
bool makeGeometry(const QImage &source, const QPolygonF &destination, PerspectiveGeometry &geometry) {
    if (source.isNull() || destination.size() != 4)
        return false;
    for (const auto &point : destination)
        if (!finitePoint(point) || std::abs(point.x()) > double(std::numeric_limits<int>::max() / 4) ||
            std::abs(point.y()) > double(std::numeric_limits<int>::max() / 4))
            return false;

    const QPolygonF sourceQuad{{0, 0}, {double(source.width()), 0},
                               {double(source.width()), double(source.height())}, {0, double(source.height())}};
    QTransform forward;
    if (!QTransform::quadToQuad(sourceQuad, destination, forward) || !finiteTransform(forward))
        return false;
    const QRect bounds = destination.boundingRect().toAlignedRect();
    if (!bounds.isValid() || bounds.width() <= 0 || bounds.height() <= 0)
        return false;
    if (bounds.width() > MaxSide || bounds.height() > MaxSide)
        return false;
    quint64 outputPixels = 0;
    if (!checkedArea(bounds.size(), outputPixels) || outputPixels > quint64(MaxSourcePixels))
        return false;

    // Translate the destination into the output image's integer bounding-box
    // coordinates before deriving the inverse.  Re-solving the homography for
    // the local quadrilateral avoids any ambiguity about QTransform's
    // operator* order and is mathematically equivalent to the painter's
    // translate(-bounds.topLeft()) followed by the source-to-destination map.
    const double offsetX = bounds.left();
    const double offsetY = bounds.top();
    QPolygonF localDestination;
    localDestination.reserve(4);
    for (const auto &point : destination)
        localDestination.append(point - QPointF(offsetX, offsetY));
    QTransform local;
    if (!QTransform::quadToQuad(sourceQuad, localDestination, local) || !finiteTransform(local))
        return false;
    bool invertible = false;
    const QTransform inverse = local.inverted(&invertible);
    if (!invertible || !finiteTransform(inverse))
        return false;

    double signedArea = 0;
    for (int index = 0; index < 4; ++index) {
        const QPointF a = localDestination[index];
        const QPointF b = localDestination[(index + 1) & 3];
        signedArea += a.x() * b.y() - a.y() * b.x();
    }
    // A degenerate quadrilateral is not a valid homography even if a driver
    // happens to accept its matrix.  Compute the area after translation to
    // avoid cancellation for a small shape at a very large canvas offset.
    if (!std::isfinite(signedArea) || std::abs(signedArea) < 1e-9)
        return false;

    geometry.inverse = inverse;
    geometry.forward = forward * QTransform::fromTranslate(-bounds.x(), -bounds.y());
    geometry.bounds = bounds;
    geometry.outputSize = bounds.size();
    geometry.depth = wideImage(source) ? 16 : 8;
    geometry.corners.reserve(4);
    geometry.corners = localDestination;
    return true;
}

#ifdef Q_OS_WIN
using Microsoft::WRL::ComPtr;

struct PerspectiveConstants {
    float p[8][4]{};
};
static_assert(sizeof(PerspectiveConstants) == 128);

bool createShader(ID3D11Device *device, ComPtr<ID3D11ComputeShader> &shader) {
    // runGpuNative serializes this callback on the immediate-context mutex.
    // Keep the successful shader for that device so repeated perspective
    // operations do not pay D3DCompile's cost again.  A replacement device
    // invalidates the old COM object before compiling on the new one.
    static ID3D11Device *cachedDevice = nullptr;
    static ComPtr<ID3D11ComputeShader> cachedShader;
    if (cachedDevice != device) {
        cachedDevice = device;
        cachedShader.Reset();
    }
    if (cachedShader) {
        shader = cachedShader;
        return true;
    }
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    const HRESULT compileResult =
        D3DCompile(gpuPerspectiveShader, sizeof(gpuPerspectiveShader) - 1, "CompositorPerspective", nullptr, nullptr,
                   "perspectiveWarp", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS, 0,
                   &code, &errors);
    if (FAILED(compileResult))
        return false;
    if (FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cachedShader)))
        return false;
    shader = cachedShader;
    return true;
}

template <typename T>
bool createInputTexture(ID3D11Device *device, const QImage &pixels, DXGI_FORMAT format,
                        ComPtr<ID3D11Texture2D> &texture, ComPtr<ID3D11ShaderResourceView> &view) {
    Q_UNUSED(sizeof(T));
    D3D11_TEXTURE2D_DESC description{};
    description.Width = UINT(pixels.width());
    description.Height = UINT(pixels.height());
    description.MipLevels = description.ArraySize = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = pixels.constBits();
    data.SysMemPitch = UINT(pixels.bytesPerLine());
    return SUCCEEDED(device->CreateTexture2D(&description, &data, &texture)) &&
           SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &view));
}

bool createSpanTexture(ID3D11Device *device, const QVector<QPoint> &spans,
                       ComPtr<ID3D11Texture2D> &texture, ComPtr<ID3D11ShaderResourceView> &view) {
    if (!device || spans.isEmpty())
        return false;
    QVector<qint32> packed;
    packed.resize(spans.size() * 2);
    for (int row = 0; row < spans.size(); ++row) {
        packed[row * 2] = spans[row].x();
        packed[row * 2 + 1] = spans[row].y();
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = 1;
    description.Height = UINT(spans.size());
    description.MipLevels = description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R32G32_SINT;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = packed.constData();
    data.SysMemPitch = UINT(2 * sizeof(qint32));
    return SUCCEEDED(device->CreateTexture2D(&description, &data, &texture)) &&
           SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &view));
}

bool createOutputTextures(ID3D11Device *device, QSize size, ComPtr<ID3D11Texture2D> &texture,
                          ComPtr<ID3D11UnorderedAccessView> &output, ComPtr<ID3D11Texture2D> &staging) {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = UINT(size.width());
    description.Height = UINT(size.height());
    description.MipLevels = description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &texture)) ||
        FAILED(device->CreateUnorderedAccessView(texture.Get(), nullptr, &output)))
        return false;
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return SUCCEEDED(device->CreateTexture2D(&description, nullptr, &staging));
}

QImage runPerspectiveKernel(ID3D11Device *device, ID3D11DeviceContext *context, quint64 budget,
                            const QImage &source, const PerspectiveGeometry &geometry) {
    if (!device || !context)
        return {};
    quint64 sourcePixels = 0;
    quint64 outputPixels = 0;
    if (!checkedArea(source.size(), sourcePixels) || !checkedArea(geometry.outputSize, outputPixels) ||
        sourcePixels > GpuMaxPixels || outputPixels > GpuMaxPixels)
        return {};
    const quint64 sampleBytes = geometry.depth == 16 ? 8 : 4;
    if (sourcePixels > (std::numeric_limits<quint64>::max() - 1024 * 1024) / (sampleBytes + 32))
        return {};
    const quint64 spanBytes = quint64(geometry.outputSize.height()) * 2 * sizeof(qint32);
    const quint64 required = sourcePixels * sampleBytes + outputPixels * 32 + spanBytes + 1024 * 1024;
    if (required > budget)
        return {};

    const QVector<QPoint> spans = gpuRasterSpans(geometry.forward, source.size(), geometry.outputSize, geometry.depth);
    if (spans.size() != geometry.outputSize.height())
        return {};
    const QImage pixels = source.convertToFormat(geometry.depth == 16 ? QImage::Format_RGBA64_Premultiplied
                                                                        : QImage::Format_RGBA8888_Premultiplied);
    if (pixels.isNull() || pixels.size() != source.size())
        return {};

    ComPtr<ID3D11Texture2D> inputTexture;
    ComPtr<ID3D11ShaderResourceView> inputView;
    const DXGI_FORMAT inputFormat = geometry.depth == 16 ? DXGI_FORMAT_R16G16B16A16_UNORM
                                                         : DXGI_FORMAT_R8G8B8A8_UNORM;
    if (geometry.depth == 16
            ? !createInputTexture<quint16>(device, pixels, inputFormat, inputTexture, inputView)
            : !createInputTexture<quint8>(device, pixels, inputFormat, inputTexture, inputView))
        return {};

    ComPtr<ID3D11Texture2D> spanTexture;
    ComPtr<ID3D11ShaderResourceView> spanView;
    if (!createSpanTexture(device, spans, spanTexture, spanView))
        return {};

    ComPtr<ID3D11Texture2D> outputTexture;
    ComPtr<ID3D11UnorderedAccessView> outputView;
    ComPtr<ID3D11Texture2D> staging;
    if (!createOutputTextures(device, geometry.outputSize, outputTexture, outputView, staging))
        return {};
    ComPtr<ID3D11ComputeShader> shader;
    if (!createShader(device, shader))
        return {};
    D3D11_BUFFER_DESC constantsDescription{};
    constantsDescription.ByteWidth = sizeof(PerspectiveConstants);
    constantsDescription.Usage = D3D11_USAGE_DEFAULT;
    constantsDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> constants;
    if (FAILED(device->CreateBuffer(&constantsDescription, nullptr, &constants)))
        return {};

    PerspectiveConstants values{};
    values.p[0][0] = float(geometry.outputSize.width());
    values.p[0][1] = float(geometry.outputSize.height());
    values.p[0][2] = geometry.depth == 16 ? 65535.f : 255.f;
    const auto &inverse = geometry.inverse;
    values.p[1][0] = float(inverse.m11());
    values.p[1][1] = float(inverse.m21());
    values.p[1][2] = float(inverse.m31());
    values.p[2][0] = float(inverse.m12());
    values.p[2][1] = float(inverse.m22());
    values.p[2][2] = float(inverse.m32());
    values.p[3][0] = float(inverse.m13());
    values.p[3][1] = float(inverse.m23());
    values.p[3][2] = float(inverse.m33());
    values.p[4][0] = float(source.width());
    values.p[4][1] = float(source.height());
    // QPainter's reference path enables SmoothPixmapTransform but does not
    // enable geometric Antialiasing.  Use hard inside/outside coverage here;
    // area coverage would write partially opaque pixels just outside the
    // quadrilateral (the largest observed mismatch was an outside alpha 110).
    values.p[4][2] = 0.f;
    values.p[4][3] = 1.f; // transparent outside the destination quadrilateral
    context->UpdateSubresource(constants.Get(), 0, nullptr, &values, 0, 0);
    auto *constantBuffer = constants.Get();
    auto *input = inputView.Get();
    auto *spanInput = spanView.Get();
    auto *target = outputView.Get();
    context->CSSetConstantBuffers(0, 1, &constantBuffer);
    context->CSSetShader(shader.Get(), nullptr, 0);
    ID3D11ShaderResourceView *resources[] = {input, spanInput};
    context->CSSetShaderResources(0, 2, resources);
    context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
    context->Dispatch((geometry.outputSize.width() + 15) / 16, (geometry.outputSize.height() + 15) / 16, 1);
    ID3D11ShaderResourceView *noResources[] = {nullptr, nullptr};
    ID3D11UnorderedAccessView *noTarget = nullptr;
    context->CSSetShaderResources(0, 2, noResources);
    context->CSSetUnorderedAccessViews(0, 1, &noTarget, nullptr);
    context->CopyResource(staging.Get(), outputTexture.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return {};
    QImage result(geometry.outputSize, pixelFormat(geometry.depth));
    if (result.isNull()) {
        context->Unmap(staging.Get(), 0);
        return {};
    }
    const double maximum = geometry.depth == 16 ? 65535. : 255.;
    const size_t rowBytes = size_t(result.width()) * sizeof(float) * 4;
    if (mapped.RowPitch < rowBytes) {
        context->Unmap(staging.Get(), 0);
        return {};
    }
    for (int y = 0; y < result.height(); ++y) {
        const auto *row = reinterpret_cast<const float *>(static_cast<const char *>(mapped.pData) + y * mapped.RowPitch);
        if (geometry.depth == 16) {
            auto *destination = reinterpret_cast<QRgba64 *>(result.scanLine(y));
            for (int x = 0; x < result.width(); ++x) {
                const float *pixel = row + x * 4;
                destination[x] = rgba64(qRound(std::clamp(double(pixel[0]), 0., 1.) * maximum),
                                       qRound(std::clamp(double(pixel[1]), 0., 1.) * maximum),
                                       qRound(std::clamp(double(pixel[2]), 0., 1.) * maximum),
                                       qRound(std::clamp(double(pixel[3]), 0., 1.) * maximum));
            }
        } else {
            auto *destination = reinterpret_cast<QRgb *>(result.scanLine(y));
            for (int x = 0; x < result.width(); ++x) {
                const float *pixel = row + x * 4;
                destination[x] = qRgba(qRound(std::clamp(double(pixel[0]), 0., 1.) * maximum),
                                       qRound(std::clamp(double(pixel[1]), 0., 1.) * maximum),
                                       qRound(std::clamp(double(pixel[2]), 0., 1.) * maximum),
                                       qRound(std::clamp(double(pixel[3]), 0., 1.) * maximum));
            }
        }
    }
    context->Unmap(staging.Get(), 0);
    copyMetadata(result, source);
    return result;
}
#endif

} // namespace

QImage perspectiveGpuImage(const QImage &source, const QPolygonF &destination) {
#ifndef Q_OS_WIN
    Q_UNUSED(source);
    Q_UNUSED(destination);
    return {};
#else
    if (qEnvironmentVariableIntValue("COMPOSITOR_DISABLE_GPU"))
        return {};
    PerspectiveGeometry geometry;
    if (!makeGeometry(source, destination, geometry))
        return {};
    const QImage result = runGpuNative([&](ID3D11Device *device, ID3D11DeviceContext *context, quint64 budget) {
        GpuNativeResult result;
        result.image = runPerspectiveKernel(device, context, budget, source, geometry);
        if (!result.image.isNull()) {
            quint64 sourcePixels = 0;
            quint64 outputPixels = 0;
            checkedArea(source.size(), sourcePixels);
            checkedArea(geometry.outputSize, outputPixels);
            result.dispatches = 1;
            result.uploadedBytes = sourcePixels * (geometry.depth == 16 ? 8 : 4) +
                                   quint64(geometry.outputSize.height()) * 2 * sizeof(qint32);
            result.readbackBytes = outputPixels * (geometry.depth == 16 ? 8 : 4);
        }
        return result;
    });
    if (result.isNull() || result.size() != geometry.outputSize || result.format() != pixelFormat(geometry.depth))
        return {};
    return result;
#endif
}

QImage perspectiveWarpAccelerated(const QImage &source, const QPolygonF &destination) {
    QImage result = perspectiveGpuImage(source, destination);
    if (result.isNull()) {
        // Keep the reference renderer as the semantic authority for invalid
        // homographies and size limits; it throws the documented Error key.
        result = perspectiveWarp(source, destination);
    }
    copyMetadata(result, source);
    return result;
}

} // namespace comp
