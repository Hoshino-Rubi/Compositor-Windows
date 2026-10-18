#include "GpuRetouchStroke.h"
#include "GpuNative.h"
#include "GpuRetouchStrokeShader.h"
#include "Pixels.h"
#include <QDebug>
#include <cstring>
#include <map>
#ifdef Q_OS_WIN
namespace comp {
namespace {
using Microsoft::WRL::ComPtr;
struct Texture {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
};
struct Buffer {
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11UnorderedAccessView> uav;
};
struct Params {
    int width, height;
    float maximum;
    UINT hasMask;
    int x, y, w, h;
    float cx, cy, mx, my;
    float radius, hardness, strength;
    int r;
};
static_assert(sizeof(Params) == 64);
bool texture(ID3D11Device *d, QSize size, DXGI_FORMAT format, Texture &out, bool write, const QImage *image = nullptr) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = size.width();
    desc.Height = size.height();
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (write ? D3D11_BIND_UNORDERED_ACCESS : 0);
    D3D11_SUBRESOURCE_DATA data{};
    if (image) {
        data.pSysMem = image->constBits();
        data.SysMemPitch = UINT(image->bytesPerLine());
    }
    return SUCCEEDED(d->CreateTexture2D(&desc, image ? &data : nullptr, &out.texture)) &&
           SUCCEEDED(d->CreateShaderResourceView(out.texture.Get(), nullptr, &out.srv)) &&
           (!write || SUCCEEDED(d->CreateUnorderedAccessView(out.texture.Get(), nullptr, &out.uav)));
}
bool buffer(ID3D11Device *d, UINT count, Buffer &out) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = count * 16;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = 16;
    D3D11_UNORDERED_ACCESS_VIEW_DESC v{};
    v.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    v.Buffer.NumElements = count;
    return SUCCEEDED(d->CreateBuffer(&desc, nullptr, &out.buffer)) &&
           SUCCEEDED(d->CreateUnorderedAccessView(out.buffer.Get(), &v, &out.uav));
}
void unbind(ID3D11DeviceContext *c) {
    ID3D11ShaderResourceView *srvs[3]{};
    ID3D11UnorderedAccessView *uavs[4]{};
    c->CSSetShaderResources(0, 3, srvs);
    c->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
    c->CSSetShader(nullptr, nullptr, 0);
}
std::map<std::string, ComPtr<ID3D11ComputeShader>> shaders(ID3D11Device *d) {
    static ComPtr<ID3D11Device> owner;
    static std::map<std::string, ComPtr<ID3D11ComputeShader>> cache;
    if (owner.Get() == d && !cache.empty())
        return cache;
    cache.clear();
    owner = d;
    for (const char *name : {"initialize", "pickup", "liquify", "smudge", "packResult"}) {
        ComPtr<ID3DBlob> code, error;
        ComPtr<ID3D11ComputeShader> shader;
        if (FAILED(D3DCompile(gpuRetouchStrokeShader, sizeof(gpuRetouchStrokeShader) - 1, nullptr, nullptr, nullptr,
                              name, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS, 0, &code,
                              &error)) ||
            FAILED(d->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader))) {
            if (error)
                qWarning() << static_cast<const char *>(error->GetBufferPointer());
            cache.clear();
            return {};
        }
        cache.emplace(name, shader);
    }
    return cache;
}
} // namespace
struct GpuRetouchStroke {
    ComPtr<ID3D11Device> device;
    Texture original, field, snapshot, coverage;
    Buffer canvas, carried;
    ComPtr<ID3D11Buffer> constants;
    std::shared_ptr<GpuNativeTexture> reservation;
    Params params{};
    std::map<std::string, ComPtr<ID3D11ComputeShader>> programs;
    bool smudge = false;
};
QImage retouchGpuSegment(std::shared_ptr<GpuRetouchStroke> &owner, const QImage &original, const QImage &current,
                         const QVector<QPointF> &points, float radius, float hardness, float strength, bool smudge,
                         const QImage &coverage) {
    const quint64 area = quint64(original.width()) * original.height();
    if (points.size() < 2 || area > 8ull * 1024 * 1024 || original.width() > 16384 || original.height() > 16384 ||
        radius > 2048)
        return {};
    const int depth = wideImage(original) ? 16 : 8, r = int(std::ceil(radius)), side = 2 * r + 1;
    QRect changed;
    for (qsizetype i = 1; i < points.size(); ++i) {
        if (points[i] == points[i - 1])
            continue;
        const auto p = points[i];
        const QRect rect = smudge ? QRect(qRound(p.x()) - r, qRound(p.y()) - r, side, side)
                                  : QRectF(p - QPointF(radius, radius), QSizeF(radius * 2, radius * 2)).toAlignedRect();
        changed = changed.united(rect.intersected(original.rect()));
    }
    if (changed.isEmpty())
        return current;
    const quint64 work = quint64(changed.width()) * changed.height() * quint64(points.size() - 1);
    // Do not initialize/upload a whole resident layer for a tiny first move.
    // Once initialized, short follow-up segments reuse those resources.
    if (!owner && (work < 65536 || area > work * 8))
        return {};
    const quint64 transient = quint64(changed.width()) * changed.height() * (depth == 16 ? 16 : 8);
    const auto result = runGpuNative([&](ID3D11Device *d, ID3D11DeviceContext *c, quint64 budget) -> GpuNativeResult {
        GpuNativeResult out;
        auto fail = [&]() {
            unbind(c);
            return GpuNativeResult{};
        };
        if (owner && owner->device.Get() != d)
            return fail();
        if (!owner) {
            const quint64 bytes =
                area * (16 + 8 + 8 + (depth == 16 ? 8 : 4) + (coverage.isNull() ? 0 : (depth == 16 ? 2 : 1))) +
                quint64(smudge ? side * side : 1) * 16 + 64;
            if (bytes + transient > budget)
                return fail();
            auto s = std::make_shared<GpuRetouchStroke>();
            s->device = d;
            s->smudge = smudge;
            s->programs = shaders(d);
            if (s->programs.empty())
                return fail();
            const auto input = original.convertToFormat(depth == 16 ? QImage::Format_RGBA64_Premultiplied
                                                                    : QImage::Format_RGBA8888_Premultiplied);
            if (!texture(d, original.size(), depth == 16 ? DXGI_FORMAT_R16G16B16A16_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM,
                         s->original, false, &input) ||
                !texture(d, original.size(), DXGI_FORMAT_R32G32_FLOAT, s->field, true) ||
                !texture(d, original.size(), DXGI_FORMAT_R32G32_FLOAT, s->snapshot, false) ||
                !buffer(d, UINT(area), s->canvas) || !buffer(d, smudge ? UINT(side * side) : 1, s->carried))
                return fail();
            if (!coverage.isNull() &&
                !texture(d, original.size(), depth == 16 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM, s->coverage,
                         false, &coverage))
                return fail();
            D3D11_BUFFER_DESC cb{};
            cb.ByteWidth = sizeof(Params);
            cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            if (FAILED(d->CreateBuffer(&cb, nullptr, &s->constants)))
                return fail();
            s->params = {original.width(),
                         original.height(),
                         depth == 16 ? 65535.f : 255.f,
                         coverage.isNull() ? 0u : 1u,
                         0,
                         0,
                         original.width(),
                         original.height(),
                         float(qRound(points.front().x())),
                         float(qRound(points.front().y())),
                         0,
                         0,
                         radius,
                         hardness,
                         strength,
                         r};
            ID3D11ShaderResourceView *views[]{s->original.srv.Get(), nullptr, s->coverage.srv.Get()};
            ID3D11UnorderedAccessView *outputs[]{s->canvas.uav.Get(), s->field.uav.Get(), s->carried.uav.Get(),
                                                 nullptr};
            auto *constant = s->constants.Get();
            c->CSSetConstantBuffers(0, 1, &constant);
            c->UpdateSubresource(constant, 0, nullptr, &s->params, 0, 0);
            c->CSSetShaderResources(0, 3, views);
            c->CSSetUnorderedAccessViews(0, 4, outputs, nullptr);
            c->CSSetShader(s->programs["initialize"].Get(), nullptr, 0);
            c->Dispatch((original.width() + 15) / 16, (original.height() + 15) / 16, 1);
            ++out.dispatches;
            if (smudge) {
                c->CSSetShader(s->programs["pickup"].Get(), nullptr, 0);
                c->Dispatch((side + 15) / 16, (side + 15) / 16, 1);
                ++out.dispatches;
            }
            unbind(c);
            s->reservation = std::make_shared<GpuNativeTexture>();
            s->reservation->bytes = bytes;
            retainGpuNativeAllocation(s->reservation);
            owner = s;
            out.uploadedBytes = area * ((depth == 16 ? 8 : 4) + (coverage.isNull() ? 0 : (depth == 16 ? 2 : 1)));
        } else if (transient > budget)
            return fail();
        auto &s = *owner;
        auto *constant = s.constants.Get();
        c->CSSetConstantBuffers(0, 1, &constant);
        for (qsizetype i = 1; i < points.size(); ++i) {
            const auto a = points[i - 1], b = points[i];
            if (a == b)
                continue;
            QRect rect = (smudge ? QRect(qRound(b.x()) - r, qRound(b.y()) - r, side, side)
                                 : QRectF(b - QPointF(radius, radius), QSizeF(radius * 2, radius * 2)).toAlignedRect())
                             .intersected(original.rect());
            if (rect.isEmpty())
                continue;
            auto &p = s.params;
            p.x = rect.x();
            p.y = rect.y();
            p.w = rect.width();
            p.h = rect.height();
            p.cx = smudge ? float(qRound(b.x())) : float(b.x() - rect.x());
            p.cy = smudge ? float(qRound(b.y())) : float(b.y() - rect.y());
            p.mx = float(b.x() - a.x()) * strength;
            p.my = float(b.y() - a.y()) * strength;
            if (!smudge) {
                const int margin = int(std::ceil(std::max(std::abs(p.mx), std::abs(p.my)))) + 2;
                const QRect work = rect.adjusted(-margin, -margin, margin, margin).intersected(original.rect());
                D3D11_BOX box{UINT(work.x()), UINT(work.y()), 0, UINT(work.right() + 1), UINT(work.bottom() + 1), 1};
                c->CopySubresourceRegion(s.snapshot.texture.Get(), 0, work.x(), work.y(), 0, s.field.texture.Get(), 0,
                                         &box);
            }
            ID3D11ShaderResourceView *views[]{s.original.srv.Get(), s.snapshot.srv.Get(), s.coverage.srv.Get()};
            ID3D11UnorderedAccessView *outputs[]{s.canvas.uav.Get(), s.field.uav.Get(), s.carried.uav.Get(), nullptr};
            c->UpdateSubresource(constant, 0, nullptr, &p, 0, 0);
            c->CSSetShaderResources(0, 3, views);
            c->CSSetUnorderedAccessViews(0, 4, outputs, nullptr);
            c->CSSetShader(s.programs[smudge ? "smudge" : "liquify"].Get(), nullptr, 0);
            c->Dispatch((p.w + 15) / 16, (p.h + 15) / 16, 1);
            ++out.dispatches;
            unbind(c);
        }
        Texture packed;
        if (!texture(d, changed.size(), depth == 16 ? DXGI_FORMAT_R16G16B16A16_UINT : DXGI_FORMAT_R8G8B8A8_UINT, packed,
                     true))
            return fail();
        D3D11_TEXTURE2D_DESC desc{};
        packed.texture->GetDesc(&desc);
        desc.BindFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(d->CreateTexture2D(&desc, nullptr, &staging)))
            return fail();
        auto &p = s.params;
        p.x = changed.x();
        p.y = changed.y();
        p.w = changed.width();
        p.h = changed.height();
        c->UpdateSubresource(constant, 0, nullptr, &p, 0, 0);
        ID3D11UnorderedAccessView *outputs[]{s.canvas.uav.Get(), nullptr, nullptr, packed.uav.Get()};
        c->CSSetUnorderedAccessViews(0, 4, outputs, nullptr);
        c->CSSetShader(s.programs["packResult"].Get(), nullptr, 0);
        c->Dispatch((p.w + 15) / 16, (p.h + 15) / 16, 1);
        ++out.dispatches;
        unbind(c);
        c->CopyResource(staging.Get(), packed.texture.Get());
        out.image = current.copy();
        if (out.image.isNull())
            return fail();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(c->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return fail();
        const int bytes = depth == 16 ? 8 : 4;
        for (int y = 0; y < changed.height(); ++y)
            std::memcpy(out.image.scanLine(changed.y() + y) + qsizetype(changed.x()) * bytes,
                        static_cast<const char *>(mapped.pData) + size_t(y) * mapped.RowPitch,
                        size_t(changed.width()) * bytes);
        c->Unmap(staging.Get(), 0);
        out.readbackOperations = 1;
        out.readbackBytes = quint64(changed.width()) * changed.height() * bytes;
        return out;
    });
    return result;
}
} // namespace comp
#else
namespace comp {
struct GpuRetouchStroke {};
QImage retouchGpuSegment(std::shared_ptr<GpuRetouchStroke> &, const QImage &, const QImage &, const QVector<QPointF> &,
                         float, float, float, bool, const QImage &) {
    return {};
}
} // namespace comp
#endif
