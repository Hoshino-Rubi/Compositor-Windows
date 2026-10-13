#include "GpuHeal.h"
#include "GpuHealShader.h"
#include "GpuNative.h"

#include <QRect>
#include <QColorSpace>
#include <QDebug>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3dcompiler.h>

namespace comp {
namespace {
using Microsoft::WRL::ComPtr;
constexpr quint64 MaxPixels = 8ull * 1024 * 1024;
constexpr quint64 MaxWaveDispatches = 250000;

struct Params {
    uint32_t width = 0, height = 0, workWidth = 0, workHeight = 0;
    int32_t originX = 0, originY = 0, ring = 0, mode = 0;
    float opacity = 1, maximum = 255;
    int32_t offsetX = 0, offsetY = 0;
    uint32_t haveSource = 0, candidateCount = 0, seed = 0, reduceCount = 0;
    int32_t coverageX = 0, coverageY = 0;
    uint32_t reserved[2]{};
    float unusedDetail[4]{};
};
static_assert(sizeof(Params) == 96);

struct Texture {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
};

struct Buffer {
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    UINT count = 0;
    UINT stride = 0;
};

struct Candidate {
    float x = 0, y = 0;
    float penalty = 1;
    float pad = 0;
};

struct NativeRun {
    GpuNativeResult result;
    quint64 dispatches = 0, uploaded = 0, readback = 0;
};

bool makeTexture(ID3D11Device *device, int width, int height, DXGI_FORMAT format, UINT bind,
                 const void *data, UINT pitch, Texture &out) {
    if (!device || width <= 0 || height <= 0)
        return false;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = UINT(width);
    desc.Height = UINT(height);
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = data;
    initial.SysMemPitch = pitch;
    if (FAILED(device->CreateTexture2D(&desc, data ? &initial : nullptr, &out.texture)))
        return false;
    if (bind & D3D11_BIND_SHADER_RESOURCE) {
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = format;
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        if (FAILED(device->CreateShaderResourceView(out.texture.Get(), &view, &out.srv)))
            return false;
    }
    if (bind & D3D11_BIND_UNORDERED_ACCESS) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = format;
        view.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (FAILED(device->CreateUnorderedAccessView(out.texture.Get(), &view, &out.uav)))
            return false;
    }
    return true;
}

bool makeStructured(ID3D11Device *device, UINT count, UINT stride, UINT bind, const void *data,
                    Buffer &out) {
    if (!device || !count || !stride || count > UINT_MAX / stride)
        return false;
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = count * stride;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = stride;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = data;
    if (FAILED(device->CreateBuffer(&desc, data ? &initial : nullptr, &out.buffer)))
        return false;
    if (bind & D3D11_BIND_SHADER_RESOURCE) {
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_UNKNOWN;
        view.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        view.Buffer.NumElements = count;
        if (FAILED(device->CreateShaderResourceView(out.buffer.Get(), &view, &out.srv)))
            return false;
    }
    if (bind & D3D11_BIND_UNORDERED_ACCESS) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_UNKNOWN;
        view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        view.Buffer.NumElements = count;
        if (FAILED(device->CreateUnorderedAccessView(out.buffer.Get(), &view, &out.uav)))
            return false;
    }
    out.count = count;
    out.stride = stride;
    return true;
}

bool compileAll(ID3D11Device *device, std::map<std::string, ComPtr<ID3D11ComputeShader>> &shaders) {
    static ID3D11Device *cachedDevice = nullptr;
    static std::map<std::string, ComPtr<ID3D11ComputeShader>> cached;
    if (cachedDevice == device && cached.size() == 14) {
        shaders = cached;
        return true;
    }
    static const char *entries[] = {"healRole", "healScore", "healReduceScore", "healStats",
                                    "healReduce4", "healCoarsen", "healUpsample", "healInitValue",
                                    "healSeedValue", "healSolveBase", "healSolveTile", "healSolveWave",
                                    "healApply", "healFinalizeStats"};
    std::map<std::string, ComPtr<ID3D11ComputeShader>> fresh;
    for (const char *entry : entries) {
        ComPtr<ID3DBlob> code, errors;
        HRESULT hr = D3DCompile(compGpuHealShader, std::strlen(compGpuHealShader), "CompositorHeal",
                                nullptr, nullptr, entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                &code, &errors);
        if (FAILED(hr) || !code ||
            FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                               &fresh[std::string(entry)]))) {
            if (errors)
                qWarning().noquote() << QString::fromUtf8(static_cast<const char *>(errors->GetBufferPointer()),
                                                           int(errors->GetBufferSize()));
            else
                qWarning() << "CompositorHeal shader initialization failed";
            cachedDevice = nullptr;
            cached.clear();
            return false;
        }
    }
    cachedDevice = device;
    cached = fresh;
    shaders = std::move(fresh);
    return true;
}

void clearBindings(ID3D11DeviceContext *context) {
    ID3D11ShaderResourceView *views[9]{};
    ID3D11UnorderedAccessView *uavs[8]{};
    UINT counts[8]{};
    context->CSSetShaderResources(0, 9, views);
    context->CSSetUnorderedAccessViews(0, 8, uavs, counts);
    context->CSSetShader(nullptr, nullptr, 0);
}

void setConstants(ID3D11DeviceContext *context, ID3D11Buffer *buffer, const Params &params) {
    context->UpdateSubresource(buffer, 0, nullptr, &params, 0, 0);
    context->CSSetConstantBuffers(0, 1, &buffer);
}

void setViews(ID3D11DeviceContext *context, ID3D11ShaderResourceView *source,
              ID3D11ShaderResourceView *coverage, ID3D11ShaderResourceView *role,
              ID3D11ShaderResourceView *candidates, ID3D11ShaderResourceView *reduce,
              ID3D11ShaderResourceView *value, ID3D11UnorderedAccessView *roleOut,
              ID3D11UnorderedAccessView *scores, ID3D11UnorderedAccessView *diff,
              ID3D11UnorderedAccessView *detail, ID3D11UnorderedAccessView *reduceOut,
              ID3D11UnorderedAccessView *best, ID3D11UnorderedAccessView *valueOut,
              ID3D11UnorderedAccessView *imageOut,
              ID3D11ShaderResourceView *coarseRole = nullptr,
              ID3D11ShaderResourceView *coarseValue = nullptr,
              ID3D11ShaderResourceView *pipelineState = nullptr) {
    ID3D11ShaderResourceView *views[9]{source, coverage, role, candidates, reduce, pipelineState, value,
                                        coarseRole, coarseValue};
    context->CSSetShaderResources(0, 9, views);
    ID3D11UnorderedAccessView *uavs[8]{roleOut, scores, diff, detail, reduceOut, best, valueOut, imageOut};
    UINT counts[8]{};
    context->CSSetUnorderedAccessViews(0, 8, uavs, counts);
}

bool dispatch(ID3D11DeviceContext *context, ID3D11ComputeShader *shader, int gx, int gy,
              NativeRun &run) {
    if (!context || !shader || gx <= 0 || gy <= 0)
        return false;
    context->CSSetShader(shader, nullptr, 0);
    context->Dispatch(UINT(gx), UINT(gy), 1);
    ++run.dispatches;
    return true;
}

bool reduce4(ID3D11Device *device, ID3D11DeviceContext *context, const std::map<std::string, ComPtr<ID3D11ComputeShader>> &shaders,
             ID3D11Buffer *constants, const Buffer &input, UINT count, Buffer &final, Params &params,
             NativeRun &run, quint64 budget, quint64 &scratchBytes) {
    if (!count)
        return false;
    std::vector<Buffer> keep;
    keep.reserve(32);
    const Buffer *current = &input;
    UINT currentCount = count;
    while (currentCount > 1) {
        UINT groups = (currentCount + 255) / 256;
        quint64 bytes = quint64(groups) * sizeof(float) * 4;
        if (bytes > budget || scratchBytes > budget - bytes)
            return false;
        scratchBytes += bytes;
        keep.emplace_back();
        Buffer &next = keep.back();
        if (!makeStructured(device, groups, sizeof(float) * 4, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                            nullptr, next))
            return false;
        params.reduceCount = currentCount;
        setConstants(context, constants, params);
        setViews(context, nullptr, nullptr, nullptr, nullptr, current->srv.Get(), nullptr,
                 nullptr, nullptr, nullptr, nullptr, next.uav.Get(), nullptr, nullptr, nullptr);
        if (!dispatch(context, shaders.at("healReduce4").Get(), groups, 1, run)) {
            clearBindings(context);
            return false;
        }
        clearBindings(context);
        current = &next;
        currentCount = groups;
    }
    final = *current;
    return true;
}

bool bounds(const QImage &coverage, QRect &active) {
    int x0 = coverage.width(), y0 = coverage.height(), x1 = 0, y1 = 0;
    for (int y = 0; y < coverage.height(); ++y) {
        const uchar *row = coverage.constScanLine(y);
        for (int x = 0; x < coverage.width(); ++x) {
            if (!row[x])
                continue;
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x + 1);
            y1 = std::max(y1, y + 1);
        }
    }
    if (x1 <= x0 || y1 <= y0) {
        active = {};
        return false;
    }
    active = QRect(x0, y0, x1 - x0, y1 - y0);
    return true;
}

bool scoreCandidates(ID3D11DeviceContext *context, const std::map<std::string, ComPtr<ID3D11ComputeShader>> &shaders,
                     ID3D11Buffer *constants, ID3D11ShaderResourceView *source, ID3D11ShaderResourceView *role,
                     Buffer &candidateBuffer, Buffer &scoreBuffer, Buffer &bestBuffer,
                     const std::vector<Candidate> &candidates, Params &params, NativeRun &run) {
    if (candidates.empty() || candidates.size() > 128)
        return false;
    std::array<Candidate, 128> paddedCandidates{};
    std::copy(candidates.begin(), candidates.end(), paddedCandidates.begin());
    context->UpdateSubresource(candidateBuffer.buffer.Get(), 0, nullptr, paddedCandidates.data(), 0, 0);
    run.uploaded += sizeof(paddedCandidates);
    params.candidateCount = UINT(candidates.size());
    setConstants(context, constants, params);
    setViews(context, source, nullptr, role, candidateBuffer.srv.Get(), nullptr, nullptr,
             nullptr, scoreBuffer.uav.Get(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
             nullptr, nullptr, bestBuffer.srv.Get());
    if (!dispatch(context, shaders.at("healScore").Get(), 1, 1, run)) {
        clearBindings(context);
        return false;
    }
    clearBindings(context);
    setConstants(context, constants, params);
    setViews(context, nullptr, nullptr, nullptr, candidateBuffer.srv.Get(), nullptr, nullptr, nullptr,
             scoreBuffer.uav.Get(), nullptr, nullptr, nullptr, bestBuffer.uav.Get(), nullptr, nullptr);
    if (!dispatch(context, shaders.at("healReduceScore").Get(), 1, 1, run)) {
        clearBindings(context);
        return false;
    }
    clearBindings(context);
    return true;
}

bool solvePyramid(ID3D11Device *device, ID3D11DeviceContext *context,
                  const std::map<std::string, ComPtr<ID3D11ComputeShader>> &shaders,
                  ID3D11Buffer *constants, Texture &role, Texture &value, int workWidth,
                  int workHeight, int originX, int originY, int depth, Params params,
                  NativeRun &run, quint64 budget, quint64 &scratchBytes) {
    const bool coarse = workWidth > 32 && workHeight > 32 && depth < 16;
    const bool pixelWave = qEnvironmentVariableIntValue("COMPOSITOR_HEAL_PIXEL_WAVE") != 0;
    Texture coarseRole, coarseValue;
    int coarseWidth = 0, coarseHeight = 0;
    if (coarse) {
        coarseWidth = (workWidth + 1) / 2;
        coarseHeight = (workHeight + 1) / 2;
        const quint64 extra = quint64(coarseWidth) * quint64(coarseHeight) * 20;
        if (extra > budget || scratchBytes > budget - extra)
            return false;
        scratchBytes += extra;
        if (!makeTexture(device, coarseWidth, coarseHeight, DXGI_FORMAT_R8_UINT,
                         D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, nullptr, 0, coarseRole) ||
            !makeTexture(device, coarseWidth, coarseHeight, DXGI_FORMAT_R32G32B32A32_FLOAT,
                         D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, nullptr, 0, coarseValue))
            return false;
        Params stage = params;
        stage.originX = originX;
        stage.originY = originY;
        stage.workWidth = UINT(coarseWidth);
        stage.workHeight = UINT(coarseHeight);
        setConstants(context, constants, stage);
        setViews(context, nullptr, nullptr, role.srv.Get(), nullptr, nullptr, value.srv.Get(),
                 coarseRole.uav.Get(), nullptr, nullptr, nullptr, nullptr, nullptr,
                 coarseValue.uav.Get(), nullptr);
        if (!dispatch(context, shaders.at("healCoarsen").Get(), (coarseWidth + 15) / 16,
                      (coarseHeight + 15) / 16, run)) {
            clearBindings(context);
            return false;
        }
        clearBindings(context);
        if (!solvePyramid(device, context, shaders, constants, coarseRole, coarseValue,
                          coarseWidth, coarseHeight, 0, 0, depth + 1, stage, run, budget, scratchBytes))
            return false;
        Params up = params;
        up.originX = originX;
        up.originY = originY;
        up.workWidth = UINT(workWidth);
        up.workHeight = UINT(workHeight);
        setConstants(context, constants, up);
        setViews(context, nullptr, nullptr, role.srv.Get(), nullptr, nullptr, nullptr,
                 nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, value.uav.Get(), nullptr,
                 coarseRole.srv.Get(), coarseValue.srv.Get());
        if (!dispatch(context, shaders.at("healUpsample").Get(), (workWidth + 15) / 16,
                      (workHeight + 15) / 16, run)) {
            clearBindings(context);
            return false;
        }
        clearBindings(context);
    }
    params.originX = originX;
    params.originY = originY;
    params.workWidth = UINT(workWidth);
    params.workHeight = UINT(workHeight);
    if (!coarse && !pixelWave) {
        params.reduceCount = 300;
        params.offsetX = 0;
        setConstants(context, constants, params);
        setViews(context, nullptr, nullptr, role.srv.Get(), nullptr, nullptr, nullptr,
                 nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, value.uav.Get(), nullptr);
        if (!dispatch(context, shaders.at("healSolveBase").Get(), 1, 1, run)) {
            clearBindings(context);
            return false;
        }
        clearBindings(context);
        return true;
    }
    const int iterations = coarse ? 40 : 300;
    if (pixelWave) {
        const int diagonals = workWidth + workHeight - 1;
        for (int it = 0; it < iterations; ++it) {
            for (int diagonal = 0; diagonal < diagonals; ++diagonal) {
                params.offsetX = diagonal;
                setConstants(context, constants, params);
                setViews(context, nullptr, nullptr, role.srv.Get(), nullptr, nullptr, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, value.uav.Get(), nullptr);
                const int length = std::min(workWidth, std::min(diagonal + 1, diagonals - diagonal));
                if (!dispatch(context, shaders.at("healSolveWave").Get(), (length + 63) / 64, 1, run)) {
                    clearBindings(context);
                    return false;
                }
                clearBindings(context);
            }
        }
        return true;
    }
    const int tilesX = (workWidth + 15) / 16, tilesY = (workHeight + 15) / 16;
    const int tileDiagonals = tilesX + tilesY - 1;
    // Consecutive dispatches keep the same resources and shader. Rebinding
    // nine SRVs/eight UAVs (and clearing them again) per diagonal adds CPU
    // preparation without changing the ordered solver dependencies.
    setViews(context, nullptr, nullptr, role.srv.Get(), nullptr, nullptr, nullptr,
             nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, value.uav.Get(), nullptr);
    context->CSSetShader(shaders.at("healSolveTile").Get(), nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &constants);
    for (int it = 0; it < iterations; ++it) {
        for (int diagonal = 0; diagonal < tileDiagonals; ++diagonal) {
            const int tx0 = std::max(0, diagonal - (tilesY - 1));
            const int tx1 = std::min(tilesX - 1, diagonal);
            params.offsetX = tx0;
            params.offsetY = diagonal;
            context->UpdateSubresource(constants, 0, nullptr, &params, 0, 0);
            context->Dispatch(UINT(tx1 - tx0 + 1), 1, 1);
            ++run.dispatches;
        }
    }
    clearBindings(context);
    return true;
}

bool runPipeline(ID3D11Device *device, ID3D11DeviceContext *context, const QImage &source,
                 const QImage &coverage, const QRect &active, float opacity, int mode, uint32_t seed, quint64 budget,
                 NativeRun &run) {
    if (!device || !context || source.isNull() || coverage.isNull() || source.size() != coverage.size())
        return false;
    const int width = source.width(), height = source.height();
    const quint64 pixels = quint64(width) * quint64(height);
    if (pixels == 0 || pixels > MaxPixels)
        return false;
    const int size = std::max(active.width(), active.height());
    const int ring = std::clamp(size / 8, 2, 16);
    const int ox = std::max(0, active.left() - ring);
    const int oy = std::max(0, active.top() - ring);
    const int wx = std::min(width, active.right() + 1 + ring) - ox;
    const int wy = std::min(height, active.bottom() + 1 + ring) - oy;
    const quint64 work = quint64(wx) * quint64(wy);
    const int depth = source.format() == QImage::Format_RGBA64_Premultiplied ? 16 : 8;
    const DXGI_FORMAT sourceFormat = depth == 16 ? DXGI_FORMAT_R16G16B16A16_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    const quint64 bpp = depth == 16 ? 8 : 4;
    const quint64 outputPixels = quint64(active.width()) * quint64(active.height());
    quint64 estimate = pixels * (bpp + 1 + 16) + work * 33 + outputPixels * bpp * 2 + 16 * 128 * 2;
    if (estimate > budget)
        return false;
    if (quint64(wx + wy - 1) * quint64(wx > 32 && wy > 32 ? 40 : 300) > MaxWaveDispatches)
        return false;

    Texture sourceTex, coverageTex, roleTex, valueTex, outputTex;
    if (!makeTexture(device, width, height, sourceFormat, D3D11_BIND_SHADER_RESOURCE, source.constBits(),
                     UINT(source.bytesPerLine()), sourceTex) ||
        !makeTexture(device, wx, wy, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE,
                     coverage.constScanLine(oy) + ox, UINT(coverage.bytesPerLine()), coverageTex) ||
        !makeTexture(device, width, height, DXGI_FORMAT_R8_UINT,
                     D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, nullptr, 0, roleTex) ||
        !makeTexture(device, width, height, DXGI_FORMAT_R32G32B32A32_FLOAT,
                     D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, nullptr, 0, valueTex) ||
        !makeTexture(device, active.width(), active.height(), sourceFormat, D3D11_BIND_UNORDERED_ACCESS, nullptr, 0, outputTex))
        return false;
    run.uploaded += pixels * bpp + work;

    std::map<std::string, ComPtr<ID3D11ComputeShader>> shaders;
    if (!compileAll(device, shaders))
        return false;
    D3D11_BUFFER_DESC constantDesc{};
    constantDesc.ByteWidth = sizeof(Params);
    constantDesc.Usage = D3D11_USAGE_DEFAULT;
    constantDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> constants;
    if (FAILED(device->CreateBuffer(&constantDesc, nullptr, &constants)))
        return false;

    Params params{};
    params.width = UINT(width); params.height = UINT(height);
    params.workWidth = UINT(wx); params.workHeight = UINT(wy);
    params.originX = ox; params.originY = oy; params.ring = ring; params.mode = mode;
    params.coverageX = ox; params.coverageY = oy;
    params.opacity = opacity; params.maximum = depth == 16 ? 65535.f : 255.f; params.seed = seed;

    setConstants(context, constants.Get(), params);
    setViews(context, nullptr, coverageTex.srv.Get(), nullptr, nullptr, nullptr, nullptr,
             roleTex.uav.Get(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!dispatch(context, shaders.at("healRole").Get(), (width + 15) / 16, (height + 15) / 16, run)) {
        clearBindings(context);
        return false;
    }
    clearBindings(context);

    std::array<Candidate, 128> candidateInit{};
    Buffer candidateBuffer, scoreBuffer, bestBuffer;
    const std::array<float, 12> initialState{};
    if (!makeStructured(device, 128, sizeof(Candidate), D3D11_BIND_SHADER_RESOURCE, candidateInit.data(), candidateBuffer) ||
        !makeStructured(device, 128, sizeof(float), D3D11_BIND_UNORDERED_ACCESS, nullptr, scoreBuffer) ||
        !makeStructured(device, 3, sizeof(float) * 4, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, initialState.data(), bestBuffer))
        return false;
    run.uploaded += sizeof(candidateInit) + sizeof(initialState);
    if (mode != 1) {
        static const double factors[5] = {1.05, 1.35, 1.75, 2.25, 2.8};
        const int factorCount = mode == 2 ? 2 : 5;
        std::vector<Candidate> candidates;
        candidates.reserve(120);
        for (int f = 0; f < factorCount; ++f)
            for (int a = 0; a < 24; ++a) {
                const double angle = a * 3.14159265358979323846 / 12.0;
                Candidate c;
                c.x = float(std::lround(std::cos(angle) * factors[f] * wx));
                c.y = float(std::lround(std::sin(angle) * factors[f] * wy));
                c.penalty = float(mode == 2 ? 1.0 + 0.6 * f : 1.0 + 0.1 * f);
                candidates.push_back(c);
            }
        if (!scoreCandidates(context, shaders, constants.Get(), sourceTex.srv.Get(), roleTex.srv.Get(),
                             candidateBuffer, scoreBuffer, bestBuffer, candidates, params, run))
            return false;
        std::vector<Candidate> refine;
        refine.reserve(49);
        for (int y = -3; y <= 3; ++y)
            for (int x = -3; x <= 3; ++x)
                refine.push_back(Candidate{float(x), float(y), 1, 0});
        params.haveSource = 1; // Refine around the device-resident winning offset.
        if (!scoreCandidates(context, shaders, constants.Get(), sourceTex.srv.Get(), roleTex.srv.Get(),
                             candidateBuffer, scoreBuffer, bestBuffer, refine, params, run))
            return false;
    }
    Buffer statsDiff, statsDetail;
    if (!makeStructured(device, UINT(work), sizeof(float) * 4, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, nullptr, statsDiff) ||
        !makeStructured(device, UINT(work), sizeof(float) * 4, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, nullptr, statsDetail))
        return false;
    setConstants(context, constants.Get(), params);
    setViews(context, sourceTex.srv.Get(), nullptr, roleTex.srv.Get(), nullptr, nullptr, nullptr,
             nullptr, nullptr, statsDiff.uav.Get(), statsDetail.uav.Get(), nullptr, nullptr, nullptr, nullptr,
             nullptr, nullptr, bestBuffer.srv.Get());
    if (!dispatch(context, shaders.at("healStats").Get(), (wx + 15) / 16, (wy + 15) / 16, run)) {
        clearBindings(context);
        return false;
    }
    clearBindings(context);
    quint64 scratch = estimate;
    Buffer reducedDiff, reducedDetail;
    if (!reduce4(device, context, shaders, constants.Get(), statsDiff, UINT(work), reducedDiff, params, run, budget, scratch) ||
        !reduce4(device, context, shaders, constants.Get(), statsDetail, UINT(work), reducedDetail, params, run, budget, scratch))
        return false;
    setConstants(context, constants.Get(), params);
    setViews(context, nullptr, nullptr, nullptr, reducedDetail.srv.Get(), reducedDiff.srv.Get(), nullptr,
             nullptr, nullptr, nullptr, nullptr, nullptr, bestBuffer.uav.Get(), nullptr, nullptr);
    if (!dispatch(context, shaders.at("healFinalizeStats").Get(), 1, 1, run)) {
        clearBindings(context);
        return false;
    }
    clearBindings(context);

    setConstants(context, constants.Get(), params);
    setViews(context, sourceTex.srv.Get(), nullptr, roleTex.srv.Get(), nullptr, nullptr, nullptr,
             nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, valueTex.uav.Get(), nullptr,
             nullptr, nullptr, bestBuffer.srv.Get());
    if (!dispatch(context, shaders.at("healInitValue").Get(), (wx + 15) / 16, (wy + 15) / 16, run)) {
        clearBindings(context);
        return false;
    }
    clearBindings(context);
    setConstants(context, constants.Get(), params);
    setViews(context, nullptr, nullptr, roleTex.srv.Get(), nullptr, nullptr, nullptr,
             nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, valueTex.uav.Get(), nullptr,
             nullptr, nullptr, bestBuffer.srv.Get());
    if (!dispatch(context, shaders.at("healSeedValue").Get(), (wx + 15) / 16, (wy + 15) / 16, run)) {
        clearBindings(context);
        return false;
    }
    clearBindings(context);

    if (!solvePyramid(device, context, shaders, constants.Get(), roleTex, valueTex, wx, wy, ox, oy,
                      0, params, run, budget, scratch))
        return false;
    params.originX = active.x(); params.originY = active.y();
    params.workWidth = UINT(active.width()); params.workHeight = UINT(active.height());
    setConstants(context, constants.Get(), params);
    setViews(context, sourceTex.srv.Get(), coverageTex.srv.Get(), roleTex.srv.Get(), nullptr, nullptr,
             valueTex.srv.Get(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, outputTex.uav.Get(),
             nullptr, nullptr, bestBuffer.srv.Get());
    if (!dispatch(context, shaders.at("healApply").Get(), (active.width() + 15) / 16, (active.height() + 15) / 16, run)) {
        clearBindings(context);
        return false;
    }
    clearBindings(context);

    D3D11_TEXTURE2D_DESC outputDesc{};
    outputTex.texture->GetDesc(&outputDesc);
    outputDesc.Usage = D3D11_USAGE_STAGING;
    outputDesc.BindFlags = 0;
    outputDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&outputDesc, nullptr, &staging)))
        return false;
    context->CopyResource(staging.Get(), outputTex.texture.Get());
    // Detach before the only blocking Map, then copy straight into the final
    // image. Untouched pixels retain the source bytes, with no full-frame GPU
    // output, staging allocation or round trip.
    QImage result = source.copy();
    if (result.isNull())
        return false;
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)))
        return false;
    const size_t rowBytes = size_t(active.width()) * size_t(bpp);
    if (map.RowPitch < rowBytes) {
        context->Unmap(staging.Get(), 0);
        return false;
    }
    for (int y = 0; y < active.height(); ++y)
        std::memcpy(result.scanLine(active.y() + y) + size_t(active.x()) * size_t(bpp),
                    static_cast<const uchar *>(map.pData) + size_t(y) * map.RowPitch, rowBytes);
    context->Unmap(staging.Get(), 0);
    run.readback += outputPixels * bpp;
    run.result.readbackOperations = 1;
    run.result.image = result;
    run.result.dispatches = run.dispatches;
    run.result.uploadedBytes = run.uploaded;
    run.result.readbackBytes = run.readback;
    return true;
}
} // namespace

QImage healGpuImage(const QImage &source, const QImage &coverage, float opacity, int mode, uint32_t seed) {
    if (source.isNull() || coverage.isNull() || source.size() != coverage.size() ||
        (source.format() != QImage::Format_RGBA8888_Premultiplied &&
         source.format() != QImage::Format_RGBA64_Premultiplied) ||
        coverage.format() != QImage::Format_Grayscale8 || mode < 0 || mode > 2 ||
        !std::isfinite(opacity))
        return {};
    QRect active;
    if (!bounds(coverage, active))
        return source;
    QImage result = runGpuNative([&](ID3D11Device *device, ID3D11DeviceContext *context, quint64 budget) {
        NativeRun run;
        runPipeline(device, context, source, coverage, active, opacity, mode, seed, budget, run);
        return run.result;
    });
    if (result.isNull() || result.size() != source.size() || result.format() != source.format())
        return {};
    result.setColorSpace(source.colorSpace());
    result.setDotsPerMeterX(source.dotsPerMeterX());
    result.setDotsPerMeterY(source.dotsPerMeterY());
    return result;
}
} // namespace comp
#else
namespace comp {
QImage healGpuImage(const QImage &, const QImage &, float, int, uint32_t) { return {}; }
} // namespace comp
#endif
