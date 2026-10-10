#include "CameraRaw.h"
#include "CameraRawTables.h"
#include <QPainter>
#include <cmath>
#include <array>
extern "C" {
#include "AdjustPixels.h"
}
namespace comp {
static double value(const QJsonObject &o, const char *key, double fallback = 0) {
    return o[key].toDouble(fallback);
}

QJsonObject normalizedCameraRaw(QJsonObject s) {
    auto bound = [](QJsonObject &o, QString key, double lo, double hi) {
        if (o.contains(key)) {
            double v = o[key].toDouble();
            o[key] = std::isfinite(v) ? std::clamp(v, lo, hi) : lo;
        }
    };
    bound(s, "exposure", -5, 5);
    for (auto key : {"temperature", "tint", "contrast", "highlights", "shadows", "whites", "blacks", "vibrance",
                     "saturation", "texture", "clarity", "dehaze", "glowRange", "glowSpread", "glowWarmth",
                     "vignetteAmount", "vignetteRoundness"})
        bound(s, key, -100, 100);
    for (auto key : {"glow", "vignetteMidpoint", "vignetteFeather", "vignetteHighlights", "grainAmount", "grainSize",
                     "grainRoughness"})
        bound(s, key, 0, 100);
    for (auto key : {"glowStyle", "vignetteStyle"})
        bound(s, key, 0, 2);
    bound(s, "visualizePoint", -1, 7);
    for (auto key : {"curve", "geometry", "calibration", "detail", "optics", "grading"}) {
        auto o = s[key].toObject();
        for (auto it = o.begin(); it != o.end(); ++it)
            if (it.value().isDouble()) {
                double lo = -100, hi = 100;
                if (QString(key) == "curve" && (it.key().endsWith("Split"))) {
                    lo = 0;
                    hi = 100;
                }
                if (QString(key) == "geometry" && it.key() == "rotate") {
                    lo = -45;
                    hi = 45;
                }
                if (QString(key) == "detail") {
                    lo = 0;
                    hi = it.key() == "sharpenAmount" ? 150 : 100;
                }
                if (QString(key) == "calibration" && it.key() == "process") {
                    lo = 1;
                    hi = 6;
                }
                if (QString(key) == "optics" && it.key().contains("Hue")) {
                    lo = 0;
                    hi = 360;
                }
                bound(o, it.key(), lo, hi);
            }
        if (QString(key) == "curve")
            for (auto channel : {"rgb", "red", "green", "blue"}) {
                auto points = o[channel].toArray();
                if (points.isEmpty())
                    continue;
                double previous = -1;
                bool valid = points.size() >= 2 && points.size() <= 32;
                for (const auto &p : points) {
                    auto v = p.toObject();
                    double x = v["x"].toDouble(-1), y = v["y"].toDouble(-1);
                    valid &= x >= 0 && x <= 1 && x > previous && y >= 0 && y <= 1;
                    previous = x;
                }
                if (!valid)
                    o.remove(channel);
            }
        if (QString(key) == "geometry") {
            QJsonArray guides;
            for (const auto &v : o["guides"].toArray()) {
                auto g = v.toArray();
                if (g.size() != 4 || guides.size() >= 2)
                    continue;
                bool valid = true;
                for (int i = 0; i < 4; ++i) {
                    double n = g[i].toDouble(-1);
                    valid &= std::isfinite(n) && n >= 0 && n <= 1;
                }
                if (valid &&
                    QLineF(QPointF(g[0].toDouble(), g[1].toDouble()), QPointF(g[2].toDouble(), g[3].toDouble()))
                            .length() > .01)
                    guides.append(g);
            }
            o["guides"] = guides;
        }
        if (QString(key) == "grading")
            for (auto wheel : {"shadows", "midtones", "highlights", "global"}) {
                auto w = o[wheel].toObject();
                bound(w, "hue", 0, 360);
                bound(w, "saturation", 0, 100);
                bound(w, "luminance", -100, 100);
                o[wheel] = w;
            }
        s[key] = o;
    }
    auto m = s["mixer"].toObject();
    for (auto key : {"hue", "saturation", "luminance"}) {
        auto a = m[key].toArray();
        while (a.size() > 8)
            a.removeLast();
        for (int i = 0; i < a.size(); ++i)
            a[i] = std::clamp(a[i].toDouble(), -100., 100.);
        m[key] = a;
    }
    auto points = m["points"].toArray();
    while (points.size() > 8)
        points.removeLast();
    for (int i = 0; i < points.size(); ++i) {
        auto p = points[i].toObject();
        bound(p, "hue", 0, 360);
        bound(p, "hueRange", 0, 180);
        for (auto key : {"saturation", "luminance", "saturationRange", "luminanceRange"})
            bound(p, key, 0, 1);
        for (auto key : {"hueShift", "saturationShift", "luminanceShift"})
            bound(p, key, -100, 100);
        points[i] = p;
    }
    m["points"] = points;
    s["mixer"] = m;
    return s;
}
double curveValue(double x, const QJsonArray &points) {
    if (points.size() < 2)
        return x;
    int i = 0;
    while (i + 2 < points.size() && value(points[i + 1].toObject(), "x") <= x)
        ++i;
    auto slope = [&](int k) {
        return (value(points[k + 1].toObject(), "y") - value(points[k].toObject(), "y")) /
               std::max(.00001, value(points[k + 1].toObject(), "x") - value(points[k].toObject(), "x"));
    };
    auto tangent = [&](int k) {
        if (k == 0)
            return slope(0);
        if (k == points.size() - 1)
            return slope(k - 1);
        double a = slope(k - 1), b = slope(k);
        return a * b <= 0 ? 0. : 2 / (1 / a + 1 / b);
    };
    auto a = points[i].toObject(), b = points[i + 1].toObject();
    double h = std::max(.00001, value(b, "x") - value(a, "x")), t = std::clamp((x - value(a, "x")) / h, 0., 1.);
    return std::clamp((2 * t * t * t - 3 * t * t + 1) * value(a, "y") + (t * t * t - 2 * t * t + t) * h * tangent(i) +
                          (-2 * t * t * t + 3 * t * t) * value(b, "y") + (t * t * t - t * t) * h * tangent(i + 1),
                      0., 1.);
}
static QImage geometry(const QImage &source, QJsonObject settings) {
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
            if (abs(angle) > 45)
                vertical += angle > 0 ? 25 : -25;
            else
                horizontal += angle > 0 ? 25 : -25;
        }
    }
    const double w = source.width(), h = source.height(), strength = settings["rectilinear"].toBool() ? .55 : 1,
                 v = vertical / 100 * w * .18 * strength, hz = horizontal / 100 * h * .18 * strength,
                 aspect = 1 + value(settings, "aspect") / 200, zoom = std::max(.01, 1 + value(settings, "scale") / 100),
                 dx = value(settings, "offsetX") / 100 * w * .15, dy = value(settings, "offsetY") / 100 * h * .15;
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
    QImage out(source.size(), QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    {
        QPainter p(&out);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.setTransform(warp);
        p.drawImage(0, 0, source);
    }
    if (settings["constrainCrop"].toBool()) {
        QRect bounds;
        for (int y = 0; y < out.height(); ++y) {
            auto *row = reinterpret_cast<const QRgb *>(out.constScanLine(y));
            for (int x = 0; x < out.width(); ++x)
                if (qAlpha(row[x]))
                    bounds = bounds.united(QRect(x, y, 1, 1));
        }
        if (!bounds.isEmpty() && bounds != out.rect()) {
            auto fitted = out.copy(bounds).scaled(source.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            out.fill(Qt::transparent);
            QPainter p(&out);
            p.drawImage((QPoint(source.width(), source.height()) - QPoint(fitted.width(), fitted.height())) / 2,
                        fitted);
        }
    }
    return out;
}
QImage cameraRaw(const QImage &source, const QJsonObject &settings, double scale, int clipping) {
    const auto s = normalizedCameraRaw(settings);
    QImage result = source;
    if (!s["geometry"].toObject().isEmpty() && s["enableGeometry"].toBool(true))
        result = geometry(result, s["geometry"].toObject());
    result = result.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    if (result.isNull())
        return result;
    auto *pixels = result.bits();
    const int w = result.width(), h = result.height(), stride = result.bytesPerLine();
    auto composed = rawtables::compose(rawtables::stages(s, result));
    const auto calibration = s["calibration"].toObject();
    if (!clipping && !s["sharpenMask"].toBool() && s["enableCalibration"].toBool(true))
        adjust_camera_raw_calibration(pixels, w, h, stride, value(calibration, "shadowTint"), 0, 0, 0, 0, 0, 0,
                                      int(value(calibration, "process", 6)));
    if (clipping)
        adjust_camera_raw(pixels, w, h, stride, composed.isEmpty() ? nullptr : composed.constData(), rawtables::Grid,
                          clipping);
    if (!clipping) {
        if (s["sharpenMask"].toBool()) {
            if (!composed.isEmpty())
                adjust_camera_raw(pixels, w, h, stride, composed.constData(), rawtables::Grid, 0);
            auto d = s["detail"].toObject();
            adjust_camera_raw_sharpen_mask_overlay(pixels, w, h, stride, value(d, "sharpenRadius", 10),
                                                   value(d, "sharpenDetail", 25), value(d, "sharpenMasking"), scale);
            return result.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        }
        auto curve = s["enableCurve"].toBool(true) ? s["curve"].toObject() : QJsonObject();
        auto mixer = s["enableMixer"].toBool(true) ? s["mixer"].toObject() : QJsonObject();
        auto grade = s["enableGrading"].toBool(true) ? s["grading"].toObject() : QJsonObject();
        float tables[4][256], mix[24] = {}, grading[12] = {};
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
                            value(curve, "darkSplit", 50) / 100, value(curve, "darks"),
                            value(curve, "darkSplit", 50) / 100, value(curve, "lights"));
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
            bool curvesChanged = false;
            for (int c = 0; c < 4; ++c)
                for (int i = 0; i < 256; ++i)
                    curvesChanged |= std::abs(tables[c][i] - i / 255.f) > .00001f;
            if (curvesChanged || !mixerStages.isEmpty() || !gradeStages.isEmpty()) {
                if (composed.isEmpty())
                    composed = rawtables::compose({}, true);
                if (curvesChanged)
                    camera_raw_table_curves(composed.data(), rawtables::Grid, tables[0], tables[1], tables[2],
                                            tables[3]);
                const auto stages = mixerStages + gradeStages;
                camera_raw_compose_onto(composed.data(), rawtables::Grid, stages.constData(), int(stages.size()),
                                        rawtables::Size);
            }
            if (!composed.isEmpty())
                adjust_camera_raw(pixels, w, h, stride, composed.constData(), rawtables::Grid, 0);
        } else {
            if (!composed.isEmpty())
                adjust_camera_raw(pixels, w, h, stride, composed.constData(), rawtables::Grid, 0);
            const auto mixerTable = rawtables::compose(mixerStages), gradeTable = rawtables::compose(gradeStages);
            adjust_camera_raw_curve_color(pixels, w, h, stride, tables[0], tables[1], tables[2], tables[3],
                                          value(curve, "refineSaturation") / 100, mix, int(points.size() / 9),
                                          points.constData(), grading, value(grade, "blending", 50) / 100,
                                          value(grade, "balance") / 100, int(value(s, "visualizePoint", -1)),
                                          mixerTable.isEmpty() ? nullptr : mixerTable.constData(),
                                          gradeTable.isEmpty() ? nullptr : gradeTable.constData(), rawtables::Grid);
        }
        if (s["enableEffects"].toBool(true)) {
            adjust_camera_raw_effects(
                pixels, w, h, stride, value(s, "texture"), value(s, "clarity"), value(s, "dehaze"), value(s, "glow"),
                int(value(s, "glowStyle")), value(s, "glowRange"), value(s, "glowSpread"), value(s, "glowWarmth"),
                value(s, "vignetteAmount"), value(s, "vignetteMidpoint", 50), value(s, "vignetteRoundness"),
                value(s, "vignetteFeather", 50), value(s, "vignetteHighlights"), int(value(s, "vignetteStyle")), scale);
            if (value(s, "grainAmount") > 0)
                adjust_grain(pixels, w, h, stride, value(s, "grainAmount"), .5 + value(s, "grainSize", 25) / 100 * 19.5,
                             value(s, "grainRoughness", 50), 0, 0, 0, 1 / std::max(.001, scale));
        }
        auto optics = s["optics"].toObject();
        if (s["enableOptics"].toBool(true)) {
            const bool profile = optics["enableLensProfile"].toBool();
            adjust_camera_raw_optics(
                pixels, w, h, stride, optics["removeChromaticAberration"].toBool(), profile,
                value(optics, "profileDistortion", 100), value(optics, "profileVignetting", 100),
                (value(optics, "distortion") + (profile ? value(optics, "profileDistortion", 100) : 0)) / 100 * .35,
                value(optics, "purpleAmount"), value(optics, "purpleHueLow", 270), value(optics, "purpleHueHigh", 310),
                value(optics, "greenAmount"), value(optics, "greenHueLow", 60), value(optics, "greenHueHigh", 120),
                value(optics, "vignetteAmount"), value(optics, "vignetteMidpoint", 50), scale, rawtables::exposure(),
                rawtables::Size);
        }
        auto d = s["detail"].toObject();
        if (s["enableDetail"].toBool(true))
            adjust_camera_raw_detail(pixels, w, h, stride, value(d, "sharpenAmount"), value(d, "sharpenRadius", 10),
                                     value(d, "sharpenDetail", 25), value(d, "sharpenMasking"),
                                     value(d, "noiseLuminance"), value(d, "noiseLuminanceDetail", 50),
                                     value(d, "noiseLuminanceContrast"), value(d, "noiseColor"),
                                     value(d, "noiseColorDetail", 50), value(d, "noiseColorSmoothness", 50), scale);
        if (s["highlightClip"].toBool() || s["shadowClip"].toBool())
            adjust_camera_raw_clip_overlay(pixels, w, h, stride, s["shadowClip"].toBool(), s["highlightClip"].toBool());
    }
    return result.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}
QJsonObject autoWhiteBalance(const QImage &input) {
    auto image = input.scaled(300, 300, Qt::KeepAspectRatio).convertToFormat(QImage::Format_ARGB32);
    double sum[3] = {}, n = 0;
    auto linear = [](double v) { return v <= .04045 ? v / 12.92 : pow((v + .055) / 1.055, 2.4); };
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            auto c = image.pixelColor(x, y);
            if (!c.alpha())
                continue;
            sum[0] += linear(c.redF());
            sum[1] += linear(c.greenF());
            sum[2] += linear(c.blueF());
            ++n;
        }
    if (n == 0)
        return {};
    auto encode = [](double x) { return x <= .0031308 ? x * 12.92 : 1.055 * pow(x, 1 / 2.4) - .055; };
    return rawtables::neutralize(encode(sum[0] / n), encode(sum[1] / n), encode(sum[2] / n));
}
} // namespace comp
