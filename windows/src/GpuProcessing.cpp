#include "GpuProcessing.h"
#ifdef Q_OS_WIN
#include "Filters.h"
#include "CameraRaw.h"
#include "GpuBlurShader.h"
#include "GpuDitherShader.h"
#include "GpuProcessingShader.h"
#include "GpuRawShader.h"
#include "Pixels.h"
#include <bit>
#include <cmath>
#include <d3dcompiler.h>
#include <xmmintrin.h>
namespace comp {
using Microsoft::WRL::ComPtr;
GpuProcessor::GpuProcessor(ID3D11Device *device, ID3D11DeviceContext *context) : device_(device), context_(context) {
    D3D11_FEATURE_DATA_DOUBLES support{};
    precisePredicates_ = SUCCEEDED(device_->CheckFeatureSupport(D3D11_FEATURE_DOUBLES, &support, sizeof(support))) &&
                         support.DoublePrecisionFloatShaderOps;
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = sizeof(Params);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    device_->CreateBuffer(&desc, nullptr, &constants_);
}
bool GpuProcessor::supports(const QString &kind) {
    return QStringList{"Hue/Saturation",  "Black & White", "Color Balance", "Gradient Map", "Add Noise",
                       "Grain",           "Motion Blur",   "Vignette",      "Bloom / Glow", "Tonal Contrast",
                       "Lens Correction", "Dither",        "Scanlines",     "Camera Raw"}
        .contains(kind);
}
bool GpuProcessor::prefersCpu(const QJsonObject &settings, int clipping) {
    const auto kind = settings["kind"].toString();
    if (kind == "Dither")
        return std::clamp(settings["style"].toInt(), 0, 9) <= 1;
    if (kind != "Camera Raw" || clipping || settings["sharpenMask"].toBool() ||
        !settings["enableDetail"].toBool(true))
        return false;
    const auto detail = normalizedCameraRaw(settings)["detail"].toObject();
    // In-place luminance denoising and error diffusion depend on scan order. Keep the whole
    // request on the reference CPU path, avoiding both serial GPU work and intermediate transfers.
    return detail["noiseLuminance"].toDouble() > 0;
}
GpuSurfacePtr GpuProcessor::surface(QSize size) {
    for (const auto &p : pool_)
        if (p.use_count() == 1 && p->size == size && p->output)
            return p;
    quint64 bytes = 0;
    for (const auto &p : pool_)
        bytes += quint64(p->size.width()) * p->size.height() * 16;
    if (bytes + quint64(size.width()) * size.height() * 16 > scratchBudget_) {
        error = "GPU processing scratch budget exceeded";
        return {};
    }
    auto out = std::make_shared<GpuSurface>();
    out->size = size;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = UINT(size.width());
    desc.Height = UINT(size.height());
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &out->texture)) ||
        FAILED(device_->CreateShaderResourceView(out->texture.Get(), nullptr, &out->view)) ||
        FAILED(device_->CreateUnorderedAccessView(out->texture.Get(), nullptr, &out->output)))
        return {};
    pool_.push_back(out);
    return out;
}
GpuSurfacePtr GpuProcessor::pass(const char *entry, const std::vector<GpuSurfacePtr> &inputs, Params params,
                                 QSize size) {
    if (!constants_) {
        error = "GPU parameter buffer unavailable";
        return {};
    }
    for (const auto &p : inputs)
        if (!p || !p->view)
            return {};
    if (!size.isValid()) {
        if (inputs.empty())
            return {};
        size = inputs.front()->size;
    }
    auto &shader = shaders_[entry];
    if (!shader) {
        ComPtr<ID3DBlob> code, errors;
        const QByteArray codeSource =
            (precisePredicates_ ? QByteArray("#define PRECISE_PREDICATES 1\n") : QByteArray()) + gpuProcessingShader +
            gpuRawShader + gpuDitherShader;
        auto hr =
            D3DCompile(codeSource.constData(), size_t(codeSource.size()), "CompositorProcessing", nullptr, nullptr,
                       entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS, 0, &code, &errors);
        if (SUCCEEDED(hr))
            hr = device_->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader);
        if (FAILED(hr)) {
            error = errors ? QString::fromUtf8(static_cast<const char *>(errors->GetBufferPointer()))
                           : "GPU processing shader failed";
            return {};
        }
    }
    auto out = surface(size);
    if (!out)
        return {};
    params[0] = float(size.width());
    params[1] = float(size.height());
    params[2] = depth_ == 16 ? 65535.f : 255.f;
    context_->UpdateSubresource(constants_.Get(), 0, nullptr, params.data(), 0, 0);
    auto *constants = constants_.Get();
    context_->CSSetConstantBuffers(0, 1, &constants);
    context_->CSSetShader(shader.Get(), nullptr, 0);
    ID3D11ShaderResourceView *views[8]{};
    for (size_t i = 0; i < inputs.size(); ++i)
        views[i] = inputs[i]->view.Get();
    context_->CSSetShaderResources(0, 8, views);
    ID3D11UnorderedAccessView *targets[]{out->output.Get(), histogramOutput_.Get()};
    context_->CSSetUnorderedAccessViews(0, 2, targets, nullptr);
    if (QByteArray(entry) == "integerBox")
        context_->Dispatch(((params[4] > 0 ? size.height() : size.width()) + 255) / 256,
                           params[4] > 0 ? size.width() : size.height(), 1);
    else if (QByteArray(entry) == "rawStatistics" || QByteArray(entry) == "rawHistogram")
        context_->Dispatch(size.width(), size.height(), 1);
    else
        context_->Dispatch((size.width() + 15) / 16, QByteArray(entry) == "scanDraw" ? 1 : (size.height() + 15) / 16,
                           1);
    ++dispatches;
    ID3D11ShaderResourceView *empty[8]{};
    context_->CSSetShaderResources(0, 8, empty);
    ID3D11UnorderedAccessView *none[2]{};
    context_->CSSetUnorderedAccessViews(0, 2, none, nullptr);
    return out;
}
GpuSurfacePtr GpuProcessor::resample(const GpuSurfacePtr &input, QSize size, const QTransform &mapping,
                                     bool antialias) {
    if (!input)
        return {};
    bool valid = false;
    const auto inverse = mapping.inverted(&valid);
    if (!valid)
        return {};
    Params p{};
    p[4] = float(inverse.m11());
    p[5] = float(inverse.m21());
    p[6] = float(inverse.m31());
    p[8] = float(inverse.m12());
    p[9] = float(inverse.m22());
    p[10] = float(inverse.m32());
    p[12] = float(inverse.m13());
    p[13] = float(inverse.m23());
    p[14] = float(inverse.m33());
    p[16] = float(input->size.width());
    p[17] = float(input->size.height());
    p[18] = antialias ? 1.f : 0.f;
    const double w = input->size.width(), h = input->size.height();
    const QPolygonF rectangle{{0, 0}, {w, 0}, {w, h}, {0, h}};
    const auto corners = mapping.map(rectangle);
    for (int i = 0; i < 4; ++i) {
        p[20 + i * 2] = float(std::trunc(corners[i].x() * 64) / 64);
        p[21 + i * 2] = float(std::trunc(corners[i].y() * 64) / 64);
    }
    return pass("transformImage", {input}, p, size);
}
GpuSurfacePtr GpuProcessor::alphaFactors() {
    auto &table = alphaFactors_[depth_ == 16 ? 1 : 0];
    if (table)
        return table;
    const int count = depth_ == 16 ? 65536 : 256;
    QVector<float> data(count);
    for (int a = 1; a < count; ++a) {
        auto alpha = _mm_set_ss(float(a)), estimate = _mm_rcp_ss(alpha);
        data[a] = _mm_cvtss_f32(
            _mm_mul_ss(_mm_sub_ss(_mm_add_ss(estimate, estimate), _mm_mul_ss(estimate, _mm_mul_ss(estimate, alpha))),
                       _mm_set_ss(float(count - 1))));
    }
    data[count - 1] = 1;
    auto out = std::make_shared<GpuSurface>();
    out->size = {256, count / 256};
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 256;
    desc.Height = count / 256;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA values{};
    values.pSysMem = data.constData();
    values.SysMemPitch = 256 * sizeof(float);
    if (FAILED(device_->CreateTexture2D(&desc, &values, &out->texture)) ||
        FAILED(device_->CreateShaderResourceView(out->texture.Get(), nullptr, &out->view)))
        return {};
    table = out;
    return table;
}
GpuSurfacePtr GpuProcessor::adjustment(const GpuSurfacePtr &input, const QJsonObject &s) {
    Params p{};
    const auto kind = s["kind"].toString();
    if (kind == "Black & White") {
        p[3] = 1;
        auto o = s["blackWhiteSettings"].toObject();
        QStringList keys{"reds", "yellows", "greens", "cyans", "blues", "magentas"};
        const double defaults[]{40, 60, 40, 60, 20, 80};
        for (int i = 0; i < 6; ++i)
            p[4 + i] = float(o[keys[i]].toDouble(defaults[i]) / 100);
        p[12] = float(o["tintHue"].toDouble(40));
        p[13] = float(o["tintSaturation"].toDouble(20) / 100);
        p[14] = o["tint"].toBool() ? 1.f : 0.f;
    } else if (kind == "Color Balance") {
        p[3] = 2;
        const auto o = s["colorBalanceSettings"].toObject();
        const QStringList keys{"CyanRed", "MagentaGreen", "YellowBlue"};
        for (int i = 0; i < 3; ++i) {
            p[4 + i] = float(o["shadow" + keys[i]].toDouble() / 100);
            p[8 + i] = float(o["mid" + keys[i]].toDouble() / 100);
            p[12 + i] = float(o["highlight" + keys[i]].toDouble() / 100);
        }
        p[16] = o["preserveLuminosity"].toBool(true) ? 1.f : 0.f;
    } else if (kind == "Gradient Map") {
        p[3] = 3;
        const auto o = s["gradientMapSettings"].toObject();
        auto a = o["shadows"].toObject(), b = o["highlights"].toObject();
        if (o["reversed"].toBool())
            std::swap(a, b);
        const QStringList keys{"red", "green", "blue"};
        for (int i = 0; i < 3; ++i) {
            p[4 + i] = float(a[keys[i]].toDouble());
            p[8 + i] = float(b[keys[i]].toDouble(1));
        }
    } else if (kind == "Add Noise") {
        p[3] = 4;
        p[4] = float(s["noiseAmount"].toDouble(10));
        p[5] = s["noiseGaussian"].toBool() ? 1.f : 0.f;
        p[6] = s["noiseMonochromatic"].toBool() ? 1.f : 0.f;
        p[7] = std::bit_cast<float>(quint32(s["noiseSeed"].toDouble()));
    } else if (kind == "Grain") {
        p[3] = 5;
        const auto o = s["grainSettings"].toObject();
        p[4] = float(o["amount"].toDouble(25));
        p[5] = float(o["size"].toDouble(1.5));
        p[6] = float(o["roughness"].toDouble(50));
        p[7] = std::bit_cast<float>(quint32(o["seed"].toDouble()));
        p[8] = float(s["unitsPerPixel"].toDouble(1));
    } else if (kind == "Hue/Saturation") {
        p[3] = 6;
        auto o = s["hsvSettings"].toObject();
        const bool legacy = o.isEmpty();
        auto dictionary = [](QJsonValue v) {
            if (v.isObject())
                return v.toObject();
            QJsonObject o;
            auto a = v.toArray();
            for (int i = 0; i + 1 < a.size(); i += 2)
                o[a[i].toString()] = a[i + 1];
            return o;
        };
        auto values = dictionary(o["adjustments"]), bands = dictionary(o["bands"]);
        if (legacy) {
            values["Master"] = s;
            o["colorize"] = s["colorize"].toBool();
        }
        QStringList names{"Master", "Reds", "Yellows", "Greens", "Cyans", "Blues", "Magentas"};
        const int defaults[7][4] = {{0, 0, 0, 0},        {315, 345, 15, 45},   {15, 45, 75, 105},
                                    {75, 105, 135, 165}, {135, 165, 195, 225}, {195, 225, 255, 285},
                                    {255, 285, 315, 345}};
        p[4] = o["colorize"].toBool() ? 1.f : 0.f;
        p[5] = o["invertRange"].toBool() ? 1.f : 0.f;
        p[6] = float(names.indexOf(o["range"].toString("Master")));
        for (int i = 0; i < 7; ++i) {
            auto a = values[names[i]].toObject(), b = bands[names[i]].toObject();
            p[8 + i * 8] = float(a["hue"].toDouble());
            p[9 + i * 8] = float(a["saturation"].toDouble());
            p[10 + i * 8] = float(a["lightness"].toDouble());
            QStringList keys{"falloffStart", "rangeStart", "rangeEnd", "falloffEnd"};
            for (int j = 0; j < 4; ++j)
                p[12 + i * 8 + j] = float(b[keys[j]].toDouble(defaults[i][j]));
        }
    } else if (kind == "Motion Blur") {
        const double angle = s["motionAngle"].toDouble();
        QTransform rotation;
        rotation.rotate(-angle);
        const auto bounds = rotation.mapRect(QRectF(QPointF(), QSizeF(input->size))).toAlignedRect();
        const auto mapping = QImage::trueMatrix(rotation, input->size.width(), input->size.height());
        auto rotated = resample(input, bounds.size(), mapping, true);
        if (!rotated)
            return {};
        p[4] = float(std::max(1, qRound(std::clamp(s["motionDistance"].toDouble(10), 1., 2000.) / 2)));
        auto blurred = pass("motionBox", {rotated}, p);
        if (!blurred)
            return {};
        QTransform back;
        back.translate(input->size.width() / 2., input->size.height() / 2.);
        back.rotate(angle);
        back.translate(-blurred->size.width() / 2., -blurred->size.height() / 2.);
        return resample(blurred, input->size, back);
    } else
        return {};
    return kind == "Hue/Saturation" ? pass("pointAdjust", {input, alphaFactors()}, p) : pass("pointAdjust", {input}, p);
}
GpuSurfacePtr GpuProcessor::blur(const GpuSurfacePtr &input, double radius) {
    if (!input)
        return {};
    if (radius <= 0)
        return input;
    auto output = input;
    Params p{};
    p[5] = float(std::clamp(qRound(radius * .58), 1, 250));
    for (int i = 0; i < 6; ++i) {
        p[4] = float(i % 2);
        output = pass("integerBox", {output}, p);
        if (!output)
            return {};
    }
    return output;
}
GpuSurfacePtr GpuProcessor::standalone(const GpuSurfacePtr &input, const QJsonObject &s) {
    const auto kind = s["kind"].toString();
    Params p{};
    GpuSurfacePtr base = input;
    if (kind == "Dither")
        return dither(input, s);
    if (kind == "Scanlines")
        return scanlines(input, s);
    if (kind == "Gaussian Blur")
        return blur(input, s["blurRadius"].toDouble(10));
    if (kind == "Vignette") {
        p[3] = 1;
        p[4] = float(s["vignetteAmount"].toDouble(35));
        p[5] = float(s["vignetteMidpoint"].toDouble(50));
        p[6] = float(s["vignetteRoundness"].toDouble(100));
        p[7] = float(s["vignetteFeather"].toDouble(60));
        QColor c(s["vignetteColor"].toString("#000000"));
        if (!c.isValid())
            c = Qt::black;
        p[8] = c.redF();
        p[9] = c.greenF();
        p[10] = c.blueF();
        p[11] = float(s["vignetteHighlights"].toDouble(25));
    } else if (kind == "Bloom / Glow") {
        p[3] = 2;
        p[4] = float(std::clamp(s["bloomAmount"].toDouble(40), 0., 100.) / 50);
        base = blur(input, std::clamp(s["bloomRadius"].toDouble(24), 1., 150.));
    } else if (kind == "Tonal Contrast") {
        p[3] = 3;
        p[4] = float(s["tonalAmount"].toDouble(50));
        p[5] = float(s["tonalShadows"].toDouble(40));
        p[6] = float(s["tonalMidtones"].toDouble(60));
        p[7] = float(s["tonalHighlights"].toDouble(30));
        base = blur(input, std::clamp(s["tonalRadius"].toDouble(16), 1., 150.));
    } else if (kind == "Lens Correction") {
        p[3] = 4;
        p[4] = float(std::clamp(s["distortion"].toDouble(), -100., 100.) * .0035);
    } else
        return {};
    return pass("standaloneFilter", {input, base}, p);
}
GpuSurfacePtr GpuProcessor::effects(ID3D11ShaderResourceView *input, QSize size, int depth, const QJsonObject &effects,
                                    double scale) {
    pool_.clear();
    error.clear();
    size_ = size;
    depth_ = depth;
    auto original = std::make_shared<GpuSurface>();
    original->view = input;
    original->size = size;
    auto back = pass("effectComposite", {original, original});
    auto foreground = original;
    auto enabled = [&](const char *key) {
        return effects[key].isObject() && effects[key].toObject()["enabled"].toBool(true);
    };
    auto color = [](const QJsonObject &o, bool white) {
        return QColor::fromRgbF(o["red"].toDouble(white ? 1 : 0), o["green"].toDouble(white ? 1 : 0),
                                o["blue"].toDouble(white ? 1 : 0), o["opacity"].toDouble(1));
    };
    auto compose = [&](const GpuSurfacePtr &a, const GpuSurfacePtr &b, bool atop = false) {
        Params p{};
        p[4] = atop ? 2.f : 1.f;
        return pass("effectComposite", {a, b}, p);
    };
    for (const auto &key :
         QStringList{"shadow", "outerGlow", "stroke", "colorOverlay", "innerShadow", "innerGlow", "stroke"}) {
        if (!enabled(qPrintable(key)))
            continue;
        auto o = effects[key].toObject();
        const bool inside = key.startsWith("inner") || (key == "stroke" && o["inside"].toBool());
        // Stroke occurs twice in the order, on its selected side only.
        // The two explicit branches below handle it outside this sequence.
        if (key == "stroke")
            continue;
        auto c = color(o, key.endsWith("Glow"));
        Params p{};
        if (key == "colorOverlay") {
            p[4] = c.redF();
            p[5] = c.greenF();
            p[6] = c.blueF();
            p[7] = c.alphaF();
            foreground = pass("effectColor", {foreground}, p);
        } else {
            p[4] = inside ? 1.f : 0.f;
            auto alpha = pass("effectAlpha", {original}, p);
            const bool shadow = key.endsWith("Shadow") || key == "shadow";
            auto image = blur(alpha, o[shadow ? "blur" : "size"].toDouble(inside ? 10 : 20) * scale);
            p = {};
            if (shadow) {
                const double angle = o["angle"].toDouble(90) * 3.141592653589793 / 180,
                             distance = o["distance"].toDouble(inside ? 10 : 20) * scale;
                p[4] = float(-std::cos(angle) * distance);
                p[5] = float(std::sin(angle) * distance);
            }
            p[8] = c.redF();
            p[9] = c.greenF();
            p[10] = c.blueF();
            p[11] = c.alphaF();
            image = pass("effectTint", {image}, p);
            if (inside)
                foreground = compose(foreground, image, true);
            else
                back = compose(back, image);
        }
        if (!back || !foreground) {
            pool_.clear();
            return {};
        }
    }
    if (enabled("stroke")) {
        const auto o = effects["stroke"].toObject();
        const bool inside = o["inside"].toBool();
        const auto c = color(o, false);
        Params p{};
        p[4] = float(o["size"].toDouble(4) * scale);
        p[5] = inside ? 1.f : 0.f;
        p[8] = c.redF();
        p[9] = c.greenF();
        p[10] = c.blueF();
        p[11] = c.alphaF();
        const int segments = std::clamp(int(p[4] * (2 * 3.14 / 6)), 3, 64) - 1;
        p[16] = float(segments);
        constexpr double k = .5522847498307936;
        for (int i = 0; i <= segments; ++i) {
            const double t = double(i) / segments, u = 1 - t;
            p[20 + i * 2] = float(p[4] * (3 * k * t * u * u + 3 * t * t * u + t * t * t));
            p[21 + i * 2] = float(p[4] * (u * u * u + 3 * t * u * u + 3 * k * t * t * u));
        }
        auto rows = pass("strokeRows", {original}, p);
        auto stroke = pass("strokeColumns", {rows, original}, p);
        if (inside)
            foreground = compose(foreground, stroke, true);
        else
            back = compose(back, stroke);
    }
    auto output = compose(back, foreground);
    pool_.clear();
    return output;
}
GpuSurfacePtr GpuProcessor::coverage(const GpuSurfacePtr &input, ID3D11ShaderResourceView *mask, int depth,
                                     double opacity) {
    if (!input)
        return {};
    pool_.clear();
    depth_ = depth;
    Params p{};
    const double maximum = depth == 16 ? 65535 : 255;
    p[4] = float(std::round(opacity * maximum) / maximum);
    p[5] = mask ? 1.f : 0.f;
    auto m = std::make_shared<GpuSurface>();
    m->size = input->size;
    m->view = mask ? mask : input->view.Get();
    auto out = pass("effectOpacity", {input, m}, p);
    pool_.clear();
    return out;
}
GpuSurfacePtr GpuProcessor::filter(ID3D11ShaderResourceView *input, QSize size, int depth, const QJsonObject &settings,
                                   double scale, int clipping) {
    pool_.clear();
    error.clear();
    if (prefersCpu(settings, clipping))
        return {};
    size_ = size;
    depth_ = depth;
    auto source = std::make_shared<GpuSurface>();
    source->view = input;
    source->size = size;
    GpuSurfacePtr out;
    const auto kind = settings["kind"].toString();
    if (QStringList{"Invert", "Exposure", "Levels", "Curves"}.contains(kind)) {
        const int count = depth == 16 ? 65536 : 256;
        QImage ramp(256, count / 256, pixelFormat(depth));
        for (int y = 0; y < ramp.height(); ++y)
            for (int x = 0; x < 256; ++x) {
                const int v = y * 256 + x;
                if (depth == 16)
                    reinterpret_cast<QRgba64 *>(ramp.scanLine(y))[x] = rgba64(v, v, v, 65535);
                else
                    reinterpret_cast<QRgb *>(ramp.scanLine(y))[x] = qRgb(v, v, v);
            }
        const auto adjusted = filtered(ramp, settings).convertToFormat(QImage::Format_RGBA64);
        QVector<float> pixels(count * 4);
        for (int y = 0; y < ramp.height(); ++y) {
            const auto *row = reinterpret_cast<const QRgba64 *>(adjusted.constScanLine(y));
            for (int x = 0; x < 256; ++x) {
                const int i = (y * 256 + x) * 4;
                pixels[i] = row[x].red() / 65535.f;
                pixels[i + 1] = row[x].green() / 65535.f;
                pixels[i + 2] = row[x].blue() / 65535.f;
            }
        }
        out = pass("nativeTone", {source, upload(pixels, ramp.size()), alphaFactors()});
    } else
        out = settings["kind"] == "Camera Raw" ? cameraRaw(source, settings, scale, clipping)
                                               : adjustment(source, settings);
    if (!out && error.isEmpty())
        out = standalone(source, settings);
    pool_.clear();
    return out;
}
} // namespace comp
#endif
