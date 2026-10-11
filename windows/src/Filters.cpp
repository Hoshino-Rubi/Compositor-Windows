#include "WideProcessing.h"
#include "Pixels.h"
#include "Filters.h"
#include "CameraRaw.h"
#include <QPainter>
#include <QQueue>
#include <cmath>
extern "C" {
#include "AdjustPixels.h"
#include "NoisePixels.h"
#include "DitherPixels.h"
}

namespace comp {
QImage blurImage(const QImage &source, double radius) {
    if (wideImage(source))
        return blurImage16(source, radius);
    if (source.isNull() || radius <= 0)
        return source;
    // Three sliding box passes approximate a Gaussian, with work independent of radius.
    QImage result = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int r = std::clamp(qRound(radius * .58), 1, 250);
    for (int pass = 0; pass < 3; ++pass)
        for (int axis = 0; axis < 2; ++axis) {
            QImage out(result.size(), result.format());
            if (out.isNull())
                throw Error("tooLarge");
            const int n = axis ? result.height() : result.width(), rows = axis ? result.width() : result.height();
            for (int row = 0; row < rows; ++row) {
                qint64 sums[4] = {};
                auto at = [&](int i) {
                    return axis ? reinterpret_cast<const QRgb *>(result.constScanLine(std::clamp(i, 0, n - 1)))[row]
                                : reinterpret_cast<const QRgb *>(result.constScanLine(row))[std::clamp(i, 0, n - 1)];
                };
                auto add = [&](QRgb v, int sign) {
                    sums[0] += qRed(v) * sign;
                    sums[1] += qGreen(v) * sign;
                    sums[2] += qBlue(v) * sign;
                    sums[3] += qAlpha(v) * sign;
                };
                for (int i = -r; i <= r; ++i)
                    add(at(i), 1);
                for (int i = 0; i < n; ++i) {
                    const int count = 2 * r + 1;
                    const QRgb value =
                        qRgba(int(sums[0] / count), int(sums[1] / count), int(sums[2] / count), int(sums[3] / count));
                    if (axis)
                        reinterpret_cast<QRgb *>(out.scanLine(i))[row] = value;
                    else
                        reinterpret_cast<QRgb *>(out.scanLine(row))[i] = value;
                    add(at(i - r), -1);
                    add(at(i + r + 1), 1);
                }
            }
            result = std::move(out);
        }
    return result;
}
bool supportedAdjustment(const QJsonObject &s) {
    const QString kind = s["kind"].toString();
    if (!QStringList{"Invert", "Hue/Saturation", "Exposure", "Levels", "Gaussian Blur", "Add Noise", "Black & White",
                     "Color Balance", "Curves", "Gradient Map", "Grain", "Motion Blur"}
             .contains(kind))
        return false;

    return true;
}
void validateAdjustment(const QJsonObject &settings) {
    if (!supportedAdjustment(settings))
        throw Error("unsupportedFeature", "adjustments");
    auto number = [](const QJsonObject &o, const QString &key, double low, double high) {
        if (!o.contains(key))
            return;
        auto v = o[key];
        if (!v.isDouble() || !std::isfinite(v.toDouble()) || v.toDouble() < low || v.toDouble() > high)
            throw Error("invalidProject");
    };
    number(settings, "hue", -360, 360);
    number(settings, "saturation", -100, 100);
    number(settings, "lightness", -100, 100);
    number(settings, "blurRadius", .1, 250);
    number(settings, "noiseAmount", .1, 400);
    number(settings, "noiseSeed", 0, 4294967295.);
    number(settings, "motionDistance", 1, 2000);
    number(settings, "motionAngle", -90, 90);
    auto hs = settings["hsvSettings"].toObject();
    auto dictionary = [](QJsonValue v) {
        if (v.isObject())
            return v.toObject();
        QJsonObject o;
        auto a = v.toArray();
        if (a.size() % 2)
            throw Error("invalidProject");
        for (int i = 0; i + 1 < a.size(); i += 2)
            o[a[i].toString()] = a[i + 1];
        return o;
    };
    const QStringList names{"Master", "Reds", "Yellows", "Greens", "Cyans", "Blues", "Magentas"};
    for (auto field : {"adjustments", "bands"}) {
        auto values = dictionary(hs[field]);
        for (auto it = values.begin(); it != values.end(); ++it) {
            if (!names.contains(it.key()) || !it.value().isObject())
                throw Error("invalidProject");
            auto o = it.value().toObject();
            if (QString(field) == "adjustments") {
                number(o, "hue", -360, 360);
                number(o, "saturation", -100, 100);
                number(o, "lightness", -100, 100);
            } else
                for (auto key : {"falloffStart", "rangeStart", "rangeEnd", "falloffEnd"})
                    number(o, key, 0, 360);
        }
    }
    const auto exposure = settings["exposureSettings"].toObject();
    number(exposure, "exposure", -20, 20);
    number(exposure, "offset", -.5, .5);
    number(exposure, "gamma", .01, 9.99);
    if (settings.contains("levels")) {
        const auto ranges = settings["levels"].toObject()["ranges"].toArray();
        if (ranges.size() != 4)
            throw Error("invalidProject");
        for (const auto &v : ranges) {
            const auto r = v.toObject();
            number(r, "black", 0, 254);
            number(r, "white", 1, 255);
            number(r, "gamma", .1, 9.99);
            number(r, "outputBlack", 0, 255);
            number(r, "outputWhite", 0, 255);
            if (r["white"].toDouble(255) <= r["black"].toDouble())
                throw Error("invalidProject");
        }
    }
    if (settings.contains("curves")) {
        const auto channels = settings["curves"].toObject()["channels"].toArray();
        if (channels.size() != 4)
            throw Error("invalidProject");
        for (const auto &channel : channels) {
            auto points = channel.toArray();
            if (points.size() < 2 || points.size() > 32 || points.first().toObject()["x"].toDouble(-1) != 0 ||
                points.last().toObject()["x"].toDouble(-1) != 255)
                throw Error("invalidProject");
            double previous = -1;
            for (const auto &point : points) {
                const auto p = point.toObject();
                number(p, "x", 0, 255);
                number(p, "y", 0, 255);
                if (!p.contains("x") || !p.contains("y") || p["x"].toDouble() <= previous)
                    throw Error("invalidProject");
                previous = p["x"].toDouble();
            }
        }
    }
    const auto grain = settings["grainSettings"].toObject();
    number(grain, "amount", 0, 100);
    number(grain, "size", .5, 20);
    number(grain, "roughness", 0, 100);
    number(grain, "seed", 0, 4294967295.);
    const auto bw = settings["blackWhiteSettings"].toObject();
    for (const auto &key : QStringList{"reds", "yellows", "greens", "cyans", "blues", "magentas"})
        number(bw, key, -200, 300);
    number(bw, "tintHue", 0, 360);
    number(bw, "tintSaturation", 0, 100);
    const auto balance = settings["colorBalanceSettings"].toObject();
    for (auto it = balance.begin(); it != balance.end(); ++it)
        if (it.key() != "preserveLuminosity")
            number(balance, it.key(), -100, 100);
    const auto gradient = settings["gradientMapSettings"].toObject();
    for (const auto &end : QStringList{"shadows", "highlights"})
        for (const auto &key : QStringList{"red", "green", "blue"})
            number(gradient[end].toObject(), key, 0, 1);
}
QImage filtered(const QImage &source, const QJsonObject &settings) {
    if (wideImage(source))
        return filtered16(source, settings);
    const QString kind = settings["kind"].toString();
    if (kind == "Camera Raw")
        return cameraRaw(source, settings);
    if (kind == "Scanlines") {
        auto rgba = source.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        auto number = [&](const char *key, double fallback, double low, double high) {
            return std::clamp(settings[key].toDouble(fallback), low, high);
        };
        ScanlinesParams params{};
        params.spacing = qRound(number("lineSpacing", 4, 2, 32));
        params.thickness = float(number("thickness", 70, 5, 100) / 100);
        params.dots = float(number("dots", 0, 0, 100) / 100);
        params.wobble = float(number("wobble", 0, 0, 64));
        params.displace = float(number("displace", 0, -100, 100));
        params.smoothness = float(number("smoothness", 50, 0, 100) / 100);
        params.threshold = float(number("threshold", 0, 0, 100) / 100);
        params.split = float(number("split", 0, 0, 16));
        params.density = float(number("density", 0, -100, 100) / 100);
        params.contrast = float(number("contrast", 0, -100, 100) / 100);
        params.blackLevel = float(number("blackLevel", 0, 0, 100) / 100);
        params.originalColors = settings["colors"].toInt() == 1;
        QColor dark =
            settings["colors"].toInt() == 2 ? QColor(settings["dark"].toString("#000000")) : QColor(Qt::black);
        QColor light =
            settings["colors"].toInt() == 2 ? QColor(settings["light"].toString("#ffffff")) : QColor(Qt::white);
        params.dark[0] = dark.red();
        params.dark[1] = dark.green();
        params.dark[2] = dark.blue();
        params.light[0] = light.red();
        params.light[1] = light.green();
        params.light[2] = light.blue();
        if (!scanlines_apply(rgba.bits(), rgba.width(), rgba.height(), rgba.bytesPerLine(), &params))
            throw Error("tooLarge");
        const double glow = number("glow", 0, 0, 100);
        if (glow > 0) {
            auto bloom = blurImage(rgba, params.spacing * 3 + 3).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
            dither_glow(rgba.bits(), bloom.constBits(), rgba.width(), rgba.height(), rgba.bytesPerLine(),
                        float(glow / 100 * 2.5));
        }
        return rgba.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
    if (kind == "Motion Blur") {
        auto image = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        double distance = std::clamp(settings["motionDistance"].toDouble(10), 1., 2000.),
               angle = settings["motionAngle"].toDouble();
        QTransform rotate;
        rotate.rotate(-angle);
        auto rotated = image.transformed(rotate, Qt::SmoothTransformation);
        if (qint64(rotated.width()) * rotated.height() > MaxSourcePixels * 2)
            throw Error("tooLarge");
        QImage average(rotated.size(), rotated.format());
        if (average.isNull())
            throw Error("tooLarge");
        int radius = std::max(1, qRound(distance / 2)), count = radius * 2 + 1;
        for (int y = 0; y < rotated.height(); ++y) {
            auto *src = reinterpret_cast<const QRgb *>(rotated.constScanLine(y));
            auto *dst = reinterpret_cast<QRgb *>(average.scanLine(y));
            qint64 sum[4] = {};
            auto add = [&](int x, int sign) {
                if (x < 0 || x >= rotated.width())
                    return;
                auto v = src[x];
                sum[0] += qRed(v) * sign;
                sum[1] += qGreen(v) * sign;
                sum[2] += qBlue(v) * sign;
                sum[3] += qAlpha(v) * sign;
            };
            for (int k = -radius; k <= radius; ++k)
                add(k, 1);
            for (int x = 0; x < rotated.width(); ++x) {
                dst[x] = qRgba(int((sum[0] + count / 2) / count), int((sum[1] + count / 2) / count),
                               int((sum[2] + count / 2) / count), int((sum[3] + count / 2) / count));
                add(x - radius, -1);
                add(x + radius + 1, 1);
            }
        }
        QImage result(source.size(), image.format());
        result.fill(Qt::transparent);
        QPainter p(&result);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.translate(source.width() / 2., source.height() / 2.);
        p.rotate(angle);
        p.translate(-average.width() / 2., -average.height() / 2.);
        p.drawImage(0, 0, average);
        p.end();
        return result;
    }
    if (kind == "Gaussian Blur")
        return blurImage(source, settings["blurRadius"].toDouble(10));
    if (QStringList{"Black & White", "Color Balance", "Gradient Map", "Grain", "Add Noise", "Camera Raw"}.contains(
            kind)) {
        QImage rgba = source.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        auto *data = rgba.bits();
        const int w = rgba.width(), h = rgba.height(), stride = rgba.bytesPerLine();
        if (kind == "Black & White") {
            const auto v = settings["blackWhiteSettings"].toObject();
            const QStringList keys{"reds", "yellows", "greens", "cyans", "blues", "magentas"};
            const double defaults[]{40, 60, 40, 60, 20, 80};
            float weights[6];
            for (int i = 0; i < 6; ++i)
                weights[i] = float(v[keys[i]].toDouble(defaults[i]) / 100.);
            adjust_black_white(data, w, h, stride, weights, v["tint"].toBool(), v["tintHue"].toDouble(40),
                               v["tintSaturation"].toDouble(20) / 100.);
        } else if (kind == "Color Balance") {
            const auto v = settings["colorBalanceSettings"].toObject();
            float sh[3], mid[3], hi[3];
            const QStringList keys{"CyanRed", "MagentaGreen", "YellowBlue"};
            for (int i = 0; i < 3; ++i) {
                sh[i] = float(v["shadow" + keys[i]].toDouble() / 100.);
                mid[i] = float(v["mid" + keys[i]].toDouble() / 100.);
                hi[i] = float(v["highlight" + keys[i]].toDouble() / 100.);
            }
            adjust_color_balance(data, w, h, stride, sh, mid, hi, v["preserveLuminosity"].toBool(true));
        } else if (kind == "Gradient Map") {
            const auto v = settings["gradientMapSettings"].toObject();
            auto a = v["shadows"].toObject(), b = v["highlights"].toObject();
            if (v["reversed"].toBool())
                std::swap(a, b);
            uint8_t table[768];
            const QStringList keys{"red", "green", "blue"};
            for (int i = 0; i < 256; ++i)
                for (int c = 0; c < 3; ++c)
                    table[3 * i + c] =
                        uint8_t(std::clamp(qRound(255 * (a[keys[c]].toDouble() +
                                                         (b[keys[c]].toDouble(1) - a[keys[c]].toDouble()) * i / 255.)),
                                           0, 255));
            adjust_gradient_map(data, w, h, stride, table);
        } else if (kind == "Grain") {
            const auto v = settings["grainSettings"].toObject();
            adjust_grain(data, w, h, stride, v["amount"].toDouble(25), v["size"].toDouble(1.5),
                         v["roughness"].toDouble(50), quint32(v["seed"].toDouble()), 0, 0, 1);
        } else
            noise_add(data, w, h, stride, float(settings["noiseAmount"].toDouble(10)),
                      settings["noiseGaussian"].toBool(), settings["noiseMonochromatic"].toBool(),
                      quint32(settings["noiseSeed"].toDouble()));
        return rgba.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
    QImage result = source.convertToFormat(QImage::Format_ARGB32);
    auto clamp = [](double v) { return std::clamp(qRound(v), 0, 255); };
    auto level = [](double v, const QJsonObject &s) {
        double black = s["black"].toDouble(0), white = s["white"].toDouble(255),
               gamma = std::clamp(s["gamma"].toDouble(1), .1, 10.);
        double t = std::pow(std::clamp((v - black) / std::max(1., white - black), 0., 1.), 1 / gamma);
        return s["outputBlack"].toDouble(0) + t * (s["outputWhite"].toDouble(255) - s["outputBlack"].toDouble(0));
    };
    for (int y = 0; y < result.height(); ++y) {
        auto *row = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < result.width(); ++x) {
            QColor c = QColor::fromRgba(row[x]);
            double r = c.red(), g = c.green(), b = c.blue();
            if (kind == "Invert") {
                r = 255 - r;
                g = 255 - g;
                b = 255 - b;
            } else if (kind == "Hue/Saturation") {
                float h, sat, l;
                c.getHslF(&h, &sat, &l);
                double hue = std::max(0.f, h) * 360, shift = settings["hue"].toDouble(),
                       saturation = settings["saturation"].toDouble(), lightness = settings["lightness"].toDouble();
                bool colorize = settings["colorize"].toBool();
                const auto hs = settings["hsvSettings"].toObject();
                if (!hs.isEmpty()) {
                    auto dictionary = [](const QJsonValue &v) {
                        if (v.isObject())
                            return v.toObject();
                        QJsonObject o;
                        auto a = v.toArray();
                        for (int i = 0; i + 1 < a.size(); i += 2)
                            o[a[i].toString()] = a[i + 1];
                        return o;
                    };
                    auto adjustments = dictionary(hs["adjustments"]), bands = dictionary(hs["bands"]);
                    colorize = hs["colorize"].toBool();
                    shift = 0;
                    saturation = 0;
                    lightness = 0;
                    auto forward = [](double a, double b) { return fmod(b - a + 720., 360.); };
                    const QHash<QString, QJsonArray> defaults{
                        {"Reds", {315, 345, 15, 45}},    {"Yellows", {15, 45, 75, 105}},
                        {"Greens", {75, 105, 135, 165}}, {"Cyans", {135, 165, 195, 225}},
                        {"Blues", {195, 225, 255, 285}}, {"Magentas", {255, 285, 315, 345}}};
                    for (auto it = adjustments.begin(); it != adjustments.end(); ++it) {
                        double weight = 1;
                        auto v = it.value().toObject();
                        if (it.key() != "Master") {
                            auto band = bands[it.key()].toObject();
                            auto d = defaults.value(it.key(), {0, 0, 360, 360});
                            double begin = band["falloffStart"].toDouble(d[0].toDouble()),
                                   end = band["falloffEnd"].toDouble(d[3].toDouble()),
                                   rangeStart = band["rangeStart"].toDouble(d[1].toDouble()),
                                   rangeEnd = band["rangeEnd"].toDouble(d[2].toDouble());
                            double span = forward(begin, end), position = forward(begin, hue),
                                   ramp = forward(begin, rangeStart), plateau = forward(begin, rangeEnd);
                            weight = span == 0             ? 1
                                     : position > span     ? 0
                                     : position < ramp     ? position / std::max(.001, ramp)
                                     : position <= plateau ? 1
                                                           : (span - position) / std::max(.001, span - plateau);
                            if (hs["invertRange"].toBool() && hs["range"] == it.key())
                                weight = 1 - weight;
                        }
                        if (colorize)
                            weight = hs["range"].toString("Master") == it.key() ? 1 : 0;
                        shift += v["hue"].toDouble() * weight;
                        saturation += v["saturation"].toDouble() * weight;
                        lightness += v["lightness"].toDouble() * weight;
                    }
                }
                h = fmod((colorize ? shift : hue + shift) + 720., 360.) / 360.;
                const double amount = std::clamp(saturation / 100., -1., 1.);
                sat = colorize      ? std::clamp(saturation / 100., 0., 1.)
                      : amount <= 0 ? sat * (1 + amount)
                      : amount >= 1 ? (sat > 0 ? 1 : 0)
                                    : std::min(1., sat / (1 - amount));
                const double light = std::clamp(lightness / 100., -1., 1.);
                l = light < 0 ? l * (1 + light) : l + (1 - l) * light;
                c.setHslF(h, std::clamp(double(sat), 0., 1.), std::clamp(double(l), 0., 1.), c.alphaF());
                r = c.red();
                g = c.green();
                b = c.blue();
            } else if (kind == "Exposure") {
                const auto e = settings["exposureSettings"].toObject();
                const double gain = std::pow(2., e["exposure"].toDouble()), offset = e["offset"].toDouble() * 255,
                             gamma = std::max(.01, e["gamma"].toDouble(1));
                auto f = [&](double v) {
                    double e = v / 255.;
                    double linear = e <= .04045 ? e / 12.92 : std::pow((e + .055) / 1.055, 2.4);
                    linear = std::pow(std::max(0., linear * gain + offset / 255.), 1 / gamma);
                    return 255 * (linear <= .0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1 / 2.4) - .055);
                };
                r = f(r);
                g = f(g);
                b = f(b);
            } else if (kind == "Levels") {
                const auto ranges = settings["levels"].toObject()["ranges"].toArray();
                if (ranges.size() == 4) {
                    r = level(level(r, ranges[1].toObject()), ranges[0].toObject());
                    g = level(level(g, ranges[2].toObject()), ranges[0].toObject());
                    b = level(level(b, ranges[3].toObject()), ranges[0].toObject());
                }
            } else if (kind == "Curves") {
                const auto channels = settings["curves"].toObject()["channels"].toArray();
                auto curve = [&](double v, int ch) {
                    if (ch >= channels.size())
                        return v;
                    const auto pts = channels[ch].toArray();
                    if (pts.size() < 2)
                        return v;
                    int i = 0;
                    while (i + 2 < pts.size() && pts[i + 1].toObject()["x"].toDouble() <= v)
                        ++i;
                    auto slope = [&](int k) {
                        return (pts[k + 1].toObject()["y"].toDouble() - pts[k].toObject()["y"].toDouble()) /
                               (pts[k + 1].toObject()["x"].toDouble() - pts[k].toObject()["x"].toDouble());
                    };
                    auto tangent = [&](int k) {
                        if (k == 0)
                            return slope(0);
                        if (k == pts.size() - 1)
                            return slope(k - 1);
                        double a = slope(k - 1), b = slope(k);
                        return a * b <= 0 ? 0. : 2 / (1 / a + 1 / b);
                    };
                    auto a = pts[i].toObject(), b = pts[i + 1].toObject();
                    double h = b["x"].toDouble() - a["x"].toDouble(),
                           t = std::clamp((v - a["x"].toDouble()) / h, 0., 1.);
                    return std::clamp((2 * t * t * t - 3 * t * t + 1) * a["y"].toDouble() +
                                          (t * t * t - 2 * t * t + t) * h * tangent(i) +
                                          (-2 * t * t * t + 3 * t * t) * b["y"].toDouble() +
                                          (t * t * t - t * t) * h * tangent(i + 1),
                                      0., 255.);
                };
                r = curve(curve(r, 1), 0);
                g = curve(curve(g, 2), 0);
                b = curve(curve(b, 3), 0);
            }
            row[x] = qRgba(clamp(r), clamp(g), clamp(b), qAlpha(row[x]));
        }
    }
    return result.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}
QPainterPath alphaPath(const QImage &image) {
    if (wideImage(image))
        return alphaPath16(image);
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    const auto rgba = image.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < rgba.height(); ++y) {
        const auto *row = reinterpret_cast<const QRgb *>(rgba.constScanLine(y));
        int begin = -1;
        for (int x = 0; x <= rgba.width(); ++x) {
            const bool hit = x < rgba.width() && qAlpha(row[x]) > 127;
            if (hit && begin < 0)
                begin = x;
            if (!hit && begin >= 0) {
                path.addRect(begin, y, x - begin, 1);
                begin = -1;
            }
        }
    }
    return path.simplified();
}
QPainterPath wandPath(const QImage &image, QPoint point, int tolerance, bool contiguous) {
    if (wideImage(image))
        return wandPath16(image, point, tolerance, contiguous);
    if (!image.rect().contains(point))
        return {};
    const auto rgba = image.convertToFormat(QImage::Format_ARGB32);
    const QRgb reference = rgba.pixel(point);
    const int width = rgba.width(), height = rgba.height();
    QByteArray seen(width * height, 0);
    QQueue<QPoint> queue;
    queue.enqueue(point);
    auto match = [&](int x, int y) {
        QRgb c = reinterpret_cast<const QRgb *>(rgba.constScanLine(y))[x];
        return std::max({abs(qRed(c) - qRed(reference)), abs(qGreen(c) - qGreen(reference)),
                         abs(qBlue(c) - qBlue(reference)), abs(qAlpha(c) - qAlpha(reference))}) <= tolerance;
    };
    if (contiguous) {
        seen[point.y() * width + point.x()] = 2;
        while (!queue.isEmpty()) {
            auto v = queue.dequeue();
            if (!match(v.x(), v.y()))
                continue;
            seen[v.y() * width + v.x()] = 1;
            for (auto d : {QPoint(1, 0), QPoint(-1, 0), QPoint(0, 1), QPoint(0, -1)}) {
                auto n = v + d;
                if (n.x() < 0 || n.x() >= width || n.y() < 0 || n.y() >= height)
                    continue;
                auto &mark = seen[n.y() * width + n.x()];
                if (mark == 0) {
                    mark = 2;
                    queue.enqueue(n);
                }
            }
        }
    } else
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                if (match(x, y))
                    seen[y * width + x] = 1;
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    for (int y = 0; y < height; ++y) {
        int begin = -1;
        for (int x = 0; x <= width; ++x) {
            bool hit = x < width && seen[y * width + x] == 1;
            if (hit && begin < 0)
                begin = x;
            if (!hit && begin >= 0) {
                path.addRect(begin, y, x - begin, 1);
                begin = -1;
            }
        }
    }
    return path.simplified();
}
} // namespace comp
