#include "GpuProcessing.h"
#ifdef Q_OS_WIN
#include "CameraRaw.h"
#include "CameraRawTables.h"
#include <cmath>
#include <cstring>
namespace comp {
using Microsoft::WRL::ComPtr;
namespace {
double value(const QJsonObject &o, const char *key, double fallback = 0) { return o[key].toDouble(fallback); }
} // namespace
GpuSurfacePtr GpuProcessor::upload(const QVector<float> &rgba, QSize size) {
    if (rgba.size() != size.width() * size.height() * 4)
        return {};
    quint64 used = 0;
    for (const auto &p : pool_)
        used += quint64(p->size.width()) * p->size.height() * 16;
    if (used + quint64(size.width()) * size.height() * 16 > scratchBudget_) {
        error = "GPU lookup-table budget exceeded";
        return {};
    }
    auto out = std::make_shared<GpuSurface>();
    out->size = size;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = size.width();
    desc.Height = size.height();
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = rgba.constData();
    data.SysMemPitch = UINT(size.width() * 16);
    if (FAILED(device_->CreateTexture2D(&desc, &data, &out->texture)) ||
        FAILED(device_->CreateShaderResourceView(out->texture.Get(), nullptr, &out->view)))
        return {};
    pool_.push_back(out);
    return out;
}
QVector<float> GpuProcessor::download(const GpuSurfacePtr &input) {
    if (!input || !input->texture)
        return {};
    D3D11_TEXTURE2D_DESC desc{};
    input->texture->GetDesc(&desc);
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &staging)))
        return {};
    context_->CopyResource(staging.Get(), input->texture.Get());
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)))
        return {};
    QVector<float> data(input->size.width() * input->size.height() * 4);
    for (int y = 0; y < input->size.height(); ++y)
        std::memcpy(data.data() + y * input->size.width() * 4, static_cast<const char *>(map.pData) + y * map.RowPitch,
                    size_t(input->size.width()) * 16);
    context_->Unmap(staging.Get(), 0);
    return data;
}
std::array<double, 3> GpuProcessor::statistics(const GpuSurfacePtr &input) {
    if (!input) {
        error = "GPU processing input unavailable";
        return {.5, .5, .5};
    }
    Params p{};
    p[4] = float(input->size.width());
    p[5] = float(input->size.height());
    auto reduced =
        pass("rawStatistics", {input}, p, {(input->size.width() + 15) / 16, (input->size.height() + 15) / 16});
    const auto values = download(reduced);
    if (values.isEmpty()) {
        if (error.isEmpty())
            error = "GPU statistics readback failed";
        return {.5, .5, .5};
    }
    double sum[4]{};
    for (int i = 0; i < values.size(); ++i)
        sum[i % 4] += values[i];
    auto encode = [](double v) {
        v = std::clamp(v, 0., 1.);
        return v <= .0031308 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - .055;
    };
    if (sum[3] <= 0)
        return {.5, .5, .5};
    return {encode(sum[0] / sum[3]), encode(sum[1] / sum[3]), encode(std::exp(sum[2] / sum[3]) - 3e-4)};
}
GpuSurfacePtr GpuProcessor::floatBox(const GpuSurfacePtr &input, int radius) {
    if (!input)
        return {};
    Params p{};
    p[4] = float(std::max(1, radius));
    auto row = pass("rawBox", {input}, p);
    p[5] = 1;
    return pass("rawBox", {row}, p);
}
GpuSurfacePtr GpuProcessor::guided(const GpuSurfacePtr &input, int radius) {
    if (!input)
        return {};
    auto mean = floatBox(input, radius);
    auto squared = pass("rawSquare", {input});
    auto squareMean = floatBox(squared, radius);
    auto coefficients = pass("rawGuidedCoefficients", {mean, squareMean});
    mean.reset();
    squared.reset();
    squareMean.reset();
    auto smooth = floatBox(coefficients, radius);
    coefficients.reset();
    return pass("rawGuidedFinish", {input, smooth});
}
GpuSurfacePtr GpuProcessor::cameraRaw(const GpuSurfacePtr &input, const QJsonObject &settings, double scale,
                                      int clipping) {
    const auto s = normalizedCameraRaw(settings);
    auto current = input;
    if (s["enableGeometry"].toBool(true) && !s["geometry"].toObject().isEmpty())
        current = rawGeometry(current, s["geometry"].toObject());
    if (!current)
        return {};
    const auto brightness = statistics(current);
    if (!error.isEmpty())
        return {};
    auto composed = rawtables::compose(rawtables::stagesWithStatistics(s, brightness.data()));
    auto table = [&](const QVector<float> &rgb) {
        QVector<float> rgba(rawtables::Grid * rawtables::Grid * rawtables::Grid * 4);
        for (int i = 0; i < rgba.size() / 4; ++i)
            for (int c = 0; c < 3; ++c)
                rgba[i * 4 + c] = rgb[i * 3 + c];
        return upload(rgba, {rawtables::Grid * rawtables::Grid, rawtables::Grid});
    };
    auto applyTable = [&](const GpuSurfacePtr &source, const QVector<float> &data, int clip = 0) {
        Params p{};
        p[4] = float(rawtables::Grid);
        p[5] = float(clip);
        return pass("rawTable", {source, table(data.isEmpty() ? rawtables::compose({}, true) : data)}, p);
    };
    if (!clipping && !s["sharpenMask"].toBool() && s["enableCalibration"].toBool(true)) {
        const auto cal = s["calibration"].toObject();
        Params p{};
        p[4] = float(value(cal, "shadowTint"));
        p[5] = float(value(cal, "process", 6));
        current = pass("rawCalibration", {current}, p);
    }
    if (clipping)
        return applyTable(current, composed, clipping);
    if (s["sharpenMask"].toBool()) {
        if (!composed.isEmpty())
            current = applyTable(current, composed);
        return rawDetail(current, s["detail"].toObject(), scale, true);
    }
    auto curve = s["enableCurve"].toBool(true) ? s["curve"].toObject() : QJsonObject();
    auto mixer = s["enableMixer"].toBool(true) ? s["mixer"].toObject() : QJsonObject();
    auto grade = s["enableGrading"].toBool(true) ? s["grading"].toObject() : QJsonObject();
    float tables[4][256];
    const QStringList curves{"rgb", "red", "green", "blue"};
    auto bend = [](double t, double lower, double low, double upper, double high) {
        if (t < lower && lower > 0)
            return lower * pow(t / lower, pow(2, -low / 100 * 1.66));
        if (t > upper && upper < 1)
            return 1 - (1 - upper) * pow((1 - t) / (1 - upper), pow(2, high / 100 * 1.66));
        return t;
    };
    QJsonArray anchors;
    for (int k = 0; k <= 32; ++k) {
        double x = k / 32.;
        double y = bend(bend(x, value(curve, "shadowSplit", 25) / 100, value(curve, "shadows"),
                             value(curve, "lightSplit", 75) / 100, value(curve, "highlights")),
                        value(curve, "darkSplit", 50) / 100, value(curve, "darks"), value(curve, "darkSplit", 50) / 100,
                        value(curve, "lights"));
        anchors.append(QJsonObject{{"x", x}, {"y", y}});
    }
    for (int c = 0; c < 4; ++c)
        for (int i = 0; i < 256; ++i) {
            double t = i / 255.;
            if (c == 0 && !rawtables::measuredCurve(curve))
                t = curveValue(t, anchors);
            tables[c][i] = float(curveValue(t, curve[curves[c]].toArray()));
        }
    QVector<float> points;
    for (const auto &entry : mixer["points"].toArray()) {
        if (points.size() >= 72)
            break;
        auto p = entry.toObject();
        points.append(float(value(p, "hue") / 360));
        points.append(float(value(p, "saturation")));
        points.append(float(value(p, "luminance")));
        points.append(float(value(p, "hueShift") / 100));
        points.append(float(value(p, "saturationShift") / 100));
        points.append(float(value(p, "luminanceShift") / 100));
        points.append(float(value(p, "hueRange", 30) / 360));
        points.append(float(value(p, "saturationRange", .4)));
        points.append(float(value(p, "luminanceRange", .4)));
    }
    const auto mixerStages = rawtables::mixer(mixer), gradeStages = rawtables::grading(grade);

    if (points.isEmpty() && value(curve, "refineSaturation") == 0 && value(s, "visualizePoint", -1) < 0) {
        bool changed = false;
        for (int c = 0; c < 4; ++c)
            for (int i = 0; i < 256; ++i)
                changed |= std::abs(tables[c][i] - i / 255.f) > .00001f;
        if (changed || !mixerStages.isEmpty() || !gradeStages.isEmpty()) {
            if (composed.isEmpty())
                composed = rawtables::compose({}, true);
            if (changed)
                camera_raw_table_curves(composed.data(), rawtables::Grid, tables[0], tables[1], tables[2], tables[3]);
            auto stages = mixerStages + gradeStages;
            camera_raw_compose_onto(composed.data(), rawtables::Grid, stages.constData(), int(stages.size()),
                                    rawtables::Size);
        }
        if (!composed.isEmpty())
            current = applyTable(current, composed);
    } else {
        if (!composed.isEmpty())
            current = applyTable(current, composed);
        QVector<float> curvePixels(1024);
        for (int i = 0; i < 256; ++i)
            for (int c = 0; c < 4; ++c)
                curvePixels[i * 4 + c] = tables[c][i];
        Params p{};
        p[4] = float(value(curve, "refineSaturation") / 100);
        p[5] = float(points.size() / 9);
        p[6] = float(value(s, "visualizePoint", -1));
        p[7] = float(rawtables::Grid);
        for (int i = 0; i < points.size(); ++i)
            p[16 + i] = points[i];
        auto mixerCube = rawtables::compose(mixerStages, true), gradeCube = rawtables::compose(gradeStages, true);
        current =
            pass("rawCurveColor", {current, table(mixerCube), table(gradeCube), upload(curvePixels, {256, 1})}, p);
    }
    if (!current)
        return {};
    if (s["enableEffects"].toBool(true)) {
        current = rawEffects(current, s, scale);
        if (!current)
            return {};
        if (value(s, "grainAmount") > 0)
            current = adjustment(current,
                                 {{"kind", "Grain"},
                                  {"unitsPerPixel", 1 / std::max(.001, scale)},
                                  {"grainSettings", QJsonObject{{"amount", value(s, "grainAmount")},
                                                                {"size", .5 + value(s, "grainSize", 25) / 100 * 19.5},
                                                                {"roughness", value(s, "grainRoughness", 50)}}}});
    }
    if (s["enableOptics"].toBool(true))
        current = rawOptics(current, s["optics"].toObject(), scale);
    if (s["enableDetail"].toBool(true))
        current = rawDetail(current, s["detail"].toObject(), scale, false);
    if (s["shadowClip"].toBool() || s["highlightClip"].toBool()) {
        Params p{};
        p[4] = s["shadowClip"].toBool() ? 1.f : 0.f;
        p[5] = s["highlightClip"].toBool() ? 1.f : 0.f;
        current = pass("rawClip", {current}, p);
    }
    return current;
}
std::array<float, 3> GpuProcessor::haze(const GpuSurfacePtr &input) {
    if (!histogramBuffer_) {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = 256 * 8;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        view.Buffer.NumElements = 512;
        view.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        if (FAILED(device_->CreateBuffer(&desc, nullptr, &histogramBuffer_)) ||
            FAILED(device_->CreateUnorderedAccessView(histogramBuffer_.Get(), &view, &histogramOutput_))) {
            error = "GPU histogram allocation failed";
            return {1, 1, 1};
        }
    }
    const UINT zero[4]{};
    context_->ClearUnorderedAccessViewUint(histogramOutput_.Get(), zero);
    Params p{};
    p[4] = float(input->size.width());
    p[5] = float(input->size.height());
    auto hist = pass("rawHistogram", {input}, p, {(input->size.width() + 15) / 16, (input->size.height() + 15) / 16});
    if (!hist)
        return {1, 1, 1};
    D3D11_BUFFER_DESC desc{};
    histogramBuffer_->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> staging;
    if (FAILED(device_->CreateBuffer(&desc, nullptr, &staging))) {
        error = "GPU histogram staging failed";
        return {1, 1, 1};
    }
    context_->CopyResource(staging.Get(), histogramBuffer_.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        error = "GPU histogram readback failed";
        return {1, 1, 1};
    }
    double bins[256]{}, total = 0;
    const auto *raw = static_cast<const quint32 *>(mapped.pData);
    for (int i = 0; i < 256; ++i) {
        bins[i] = double((quint64(raw[i * 2 + 1]) << 32) | raw[i * 2]);
        total += bins[i];
    }
    context_->Unmap(staging.Get(), 0);
    int threshold = 255;
    for (double above = 0; threshold > 0 && above + bins[threshold] < total * .01; --threshold)
        above += bins[threshold];
    p[6] = 1;
    p[7] = float(threshold);
    auto reduced = pass("rawStatistics", {input}, p, hist->size);
    const auto values = download(reduced);
    double sums[4]{};
    if (values.isEmpty()) {
        error = "GPU haze reduction failed";
        return {1, 1, 1};
    }
    for (int i = 0; i < values.size(); ++i)
        sums[i % 4] += values[i];
    if (sums[3] <= 0)
        return {1, 1, 1};
    return {float(sums[0] / sums[3]), float(sums[1] / sums[3]), float(sums[2] / sums[3])};
}
GpuSurfacePtr GpuProcessor::rawEffects(const GpuSurfacePtr &input, const QJsonObject &s, double scale) {
    if (!input)
        return {};
    const double texture = value(s, "texture"), clarity = value(s, "clarity"), dehaze = value(s, "dehaze"),
                 glow = value(s, "glow"), vignette = value(s, "vignetteAmount");
    if (!texture && !clarity && !dehaze && glow <= 0 && !vignette)
        return input;
    auto fine = input, coarse = input, glowPlane = input;
    if (texture || clarity || glow > 0) {
        const auto luma = pass("rawLuma", {input});
        const int side = std::max(input->size.width(), input->size.height());
        if (texture)
            fine = guided(luma, std::max(1, qRound(side * 8. / 1456)));
        if (clarity)
            coarse = guided(luma, std::max(2, qRound(side * 32. / 1456)));
        if (glow > 0) {
            Params p{};
            p[4] = float(.55 + .4 * value(s, "glowRange") / 100);
            auto threshold = pass("rawGlowThreshold", {luma}, p);
            const double base = value(s, "glowStyle") == 1 ? 2 : 5;
            int radius = std::clamp(
                qRound(std::max(1., base * (1 + value(s, "glowSpread") / 100)) * std::max(.001, scale)), 1, 64);
            glowPlane = floatBox(threshold, radius);
        }
    }
    auto air = dehaze ? haze(input) : std::array<float, 3>{1, 1, 1};
    if (!error.isEmpty())
        return {};
    Params p{};
    p[4] = float(texture);
    p[5] = float(clarity);
    p[6] = float(dehaze);
    p[7] = float(glow);
    for (int i = 0; i < 3; ++i)
        p[8 + i] = air[i];
    const double warmth = value(s, "glowWarmth") / 100;
    int style = int(value(s, "glowStyle"));
    p[12] = float(style == 2 ? 1 : .75 + .25 * warmth);
    p[13] = float(style == 2 ? .35 - .3 * warmth : .6 + .2 * warmth);
    p[14] = float(style == 2 ? .2 - .2 * warmth : .75 - .6 * warmth);
    p[15] = style == 1 ? 1.4f : 1.f;
    p[20] = float(vignette);
    p[21] = float(value(s, "vignetteMidpoint", 50));
    p[22] = float(value(s, "vignetteRoundness"));
    p[23] = float(value(s, "vignetteFeather", 50));
    p[24] = float(value(s, "vignetteHighlights"));
    p[25] = float(value(s, "vignetteStyle"));
    return pass("rawEffects", {input, fine, coarse, glowPlane}, p);
}
GpuSurfacePtr GpuProcessor::rawDetail(const GpuSurfacePtr &input, const QJsonObject &s, double scale, bool mask) {
    if (!input)
        return {};
    auto current = input;
    const double amount = value(s, "sharpenAmount"), luminance = value(s, "noiseLuminance"),
                 color = value(s, "noiseColor");
    if (!mask && !amount && !luminance && !color)
        return current;
    if (!mask && luminance > 0)
        return {}; // The complete Camera Raw request must use the reference CPU path.
    GpuSurfacePtr luma;
    if (!mask && color > 0) {
        const int radius = std::clamp(
            qRound((1.5 + color / 100 * 4 * (.5 + value(s, "noiseColorSmoothness", 50) / 100)) * scale), 1, 64);
        auto differences = pass("rawChromaPlanes", {current});
        auto smoothed = floatBox(differences, radius);
        auto normalized = pass("rawChromaNormalize", {smoothed});
        Params p{};
        p[4] = float(radius);
        p[5] = float(color / 100);
        p[6] = float(value(s, "noiseColorDetail", 50) / 100);
        current = pass("rawColorNoise", {current, normalized}, p);
    }
    if (mask || amount > 0) {
        luma = pass("rawLuma", {current});
        const int radius =
            std::clamp(qRound(std::clamp((.5 + value(s, "sharpenRadius", 10) / 100 * 2.5) * scale, .5, 64.)), 1, 64);
        auto coarse = mask ? luma : floatBox(luma, radius);
        Params p{};
        p[4] = float(radius);
        p[5] = float(amount / 100);
        p[6] = float(value(s, "sharpenDetail", 25) / 100);
        p[7] = float(value(s, "sharpenMasking") / 100);
        p[8] = mask ? 1.f : 0.f;
        current = pass("rawSharpen", {current, luma, coarse}, p);
    }
    return current;
}
GpuSurfacePtr GpuProcessor::rawOptics(const GpuSurfacePtr &input, const QJsonObject &s, double scale) {
    Q_UNUSED(scale);
    if (!input)
        return {};
    auto current = input;
    const bool profile = s["enableLensProfile"].toBool();
    const double distortion = (value(s, "distortion") + (profile ? value(s, "profileDistortion", 100) : 0)) * .0035;
    if (distortion != 0) {
        Params p{};
        p[3] = 4;
        p[4] = float(distortion);
        current = pass("standaloneFilter", {current, current}, p);
    }
    if (s["removeChromaticAberration"].toBool())
        current = pass("rawChromatic", {current});
    const double vignette = value(s, "vignetteAmount") + (profile ? value(s, "profileVignetting", 100) * .35 : 0);
    if (!vignette && !value(s, "purpleAmount") && !value(s, "greenAmount"))
        return current;
    const auto *table = rawtables::exposure();
    QVector<float> values(21 * 17 * 17 * 17 * 4);
    for (int i = 0; i < values.size() / 4; ++i)
        for (int c = 0; c < 3; ++c)
            values[i * 4 + c] = table[i * 3 + c] / 255.f;
    auto exposure = upload(values, {17 * 17, 21 * 17});
    Params p{};
    p[4] = float(value(s, "purpleAmount"));
    p[5] = float(value(s, "purpleHueLow", 270));
    p[6] = float(value(s, "purpleHueHigh", 310));
    p[8] = float(value(s, "greenAmount"));
    p[9] = float(value(s, "greenHueLow", 60));
    p[10] = float(value(s, "greenHueHigh", 120));
    p[12] = float(vignette);
    p[13] = float(value(s, "vignetteMidpoint", 50));
    return pass("rawOptics", {current, exposure}, p);
}
GpuSurfacePtr GpuProcessor::rawGeometry(const GpuSurfacePtr &source, const QJsonObject &settings) {
    if (!source)
        return {};
    double vertical = value(settings, "vertical"), horizontal = value(settings, "horizontal"),
           rotation = value(settings, "rotate");
    auto guides = settings["guides"].toArray();
    if (settings["upright"].toBool() && !guides.isEmpty()) {
        auto line = guides[0].toArray();
        if (line.size() == 4) {
            rotation -= atan2(line[3].toDouble() - line[1].toDouble(), line[2].toDouble() - line[0].toDouble()) * 180 /
                        3.141592653589793;
            if (rotation > 45)
                rotation -= 90;
            if (rotation < -45)
                rotation += 90;
        }
        if (guides.size() > 1) {
            auto l = guides[1].toArray();
            double angle =
                atan2(l[3].toDouble() - l[1].toDouble(), l[2].toDouble() - l[0].toDouble()) * 180 / 3.141592653589793;
            if (std::abs(angle) > 45)
                vertical += angle > 0 ? 25 : -25;
            else
                horizontal += angle > 0 ? 25 : -25;
        }
    }
    const double w = source->size.width(), h = source->size.height(),
                 strength = settings["rectilinear"].toBool() ? .55 : 1;
    const double v = vertical / 100 * w * .18 * strength, hz = horizontal / 100 * h * .18 * strength,
                 aspect = 1 + value(settings, "aspect") / 200, zoom = std::max(.01, 1 + value(settings, "scale") / 100);
    const double dx = value(settings, "offsetX") / 100 * w * .15, dy = value(settings, "offsetY") / 100 * h * .15;
    QPolygonF from{{0, 0}, {w, 0}, {w, h}, {0, h}},
        to{{-v + dx, -dy}, {w + v + dx, -dy}, {w + hz + dx, h + dy}, {-hz + dx, h + dy}};
    QTransform mapping;
    mapping.translate(w / 2 + dx, h / 2 - dy);
    mapping.scale(aspect * zoom, zoom / aspect);
    mapping.rotate(-rotation);
    mapping.translate(-w / 2 - dx, -h / 2 + dy);
    to = mapping.map(to);
    QTransform warp;
    if (!QTransform::quadToQuad(from, to, warp))
        return source;
    auto out = resample(source, source->size, warp);
    if (!out || !settings["constrainCrop"].toBool())
        return out;
    Params p{};
    p[4] = float(w);
    p[5] = float(h);
    p[6] = 2;
    auto bounds = pass("rawStatistics", {out}, p, {(int(w) + 15) / 16, (int(h) + 15) / 16});
    const auto partial = download(bounds);
    if (partial.isEmpty())
        return {};
    int left = int(w), top = int(h), right = -1, bottom = -1;
    for (int i = 0; i < partial.size(); i += 4) {
        left = std::min(left, int(partial[i]));
        top = std::min(top, int(partial[i + 1]));
        right = std::max(right, int(partial[i + 2]));
        bottom = std::max(bottom, int(partial[i + 3]));
    }
    if (right < left || bottom < top)
        return out;
    QRect rect(QPoint(left, top), QPoint(right, bottom));
    if (rect == QRect(QPoint(), source->size))
        return out;
    const QSize fitted = rect.size().scaled(source->size, Qt::KeepAspectRatio);
    QTransform fit;
    fit.translate((source->size.width() - fitted.width()) / 2, (source->size.height() - fitted.height()) / 2);
    fit.scale(double(fitted.width()) / rect.width(), double(fitted.height()) / rect.height());
    p = {};
    p[4] = float(left);
    p[5] = float(top);
    auto cropped = pass("rawCrop", {out}, p, rect.size());
    return resample(cropped, source->size, fit);
}
} // namespace comp
#endif
