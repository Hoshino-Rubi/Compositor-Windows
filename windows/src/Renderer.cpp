#include "WideProcessing.h"
#include "Pixels.h"
#include "Renderer.h"
#include "Filters.h"
#include "Effects.h"
#include <QColorSpace>
#include <QPainter>
#include <QSet>
#include <algorithm>
#include <array>
#include <cmath>
namespace comp {
namespace {
using Color = std::array<double, 3>;
double lum(const Color &c) {
    return .3 * c[0] + .59 * c[1] + .11 * c[2];
}
double sat(const Color &c) {
    return *std::max_element(c.begin(), c.end()) - *std::min_element(c.begin(), c.end());
}
Color setLum(Color c, double l) {
    const double d = l - lum(c);
    for (auto &v : c)
        v += d;
    const double n = *std::min_element(c.begin(), c.end()), x = *std::max_element(c.begin(), c.end());
    if (n < 0)
        for (auto &v : c)
            v = l + (v - l) * l / (l - n);
    if (x > 1)
        for (auto &v : c)
            v = l + (v - l) * (1 - l) / (x - l);
    return c;
}
Color setSat(Color c, double s) {
    std::array<int, 3> order{0, 1, 2};
    std::sort(order.begin(), order.end(), [&](int a, int b) { return c[a] < c[b]; });
    const int low = order[0], mid = order[1], high = order[2];
    if (c[high] > c[low]) {
        c[mid] = (c[mid] - c[low]) * s / (c[high] - c[low]);
        c[high] = s;
    } else
        c[mid] = c[high] = 0;
    c[low] = 0;
    return c;
}
double burn(double b, double s) {
    return b >= 1 ? 1 : s <= 0 ? 0 : 1 - std::min(1., (1 - b) / s);
}
double dodge(double b, double s) {
    return b <= 0 ? 0 : s >= 1 ? 1 : std::min(1., b / (1 - s));
}
double channel(double b, double s, int mode) {
    switch (mode) {
    case 1:
        return std::min(b, s);
    case 2:
        return b * s;
    case 3:
        return burn(b, s);
    case 4:
        return std::max(0., b + s - 1);
    case 5:
        return std::max(b, s);
    case 6:
        return b + s - b * s;
    case 7:
        return dodge(b, s);
    case 8:
        return std::min(1., b + s);
    case 9:
        return b <= .5 ? 2 * b * s : 1 - 2 * (1 - b) * (1 - s);
    case 10:
        return s <= .5 ? b - (1 - 2 * s) * b * (1 - b)
                       : b + (2 * s - 1) * ((b <= .25 ? ((16 * b - 12) * b + 4) * b : std::sqrt(b)) - b);
    case 11:
        return s <= .5 ? 2 * b * s : 1 - 2 * (1 - b) * (1 - s);
    case 12:
        return s <= .5 ? burn(b, 2 * s) : dodge(b, 2 * s - 1);
    case 13:
        return std::clamp(b + 2 * s - 1, 0., 1.);
    case 14:
        return s <= .5 ? std::min(b, 2 * s) : std::max(b, 2 * s - 1);
    case 15:
        return (s <= .5 ? burn(b, 2 * s) : dodge(b, 2 * s - 1)) < .5 ? 0 : 1;
    case 16:
        return std::abs(b - s);
    case 17:
        return b + s - 2 * b * s;
    case 18:
        return std::max(0., b - s);
    case 19:
        return s == 0 ? 1 : std::min(1., b / s);
    default:
        return s;
    }
}
QImage surface(QSize size) {
    QImage result(size, QImage::Format_ARGB32_Premultiplied);
    if (result.isNull())
        throw Error("tooLarge");
    result.fill(Qt::transparent);
    result.setColorSpace(QColorSpace::SRgb);
    return result;
}
bool pixelAligned(const QTransform &t) {
    auto whole = [](double v) { return std::abs(v - std::round(v)) < 1e-9; };
    const bool unitAxes = (std::abs(std::abs(t.m11()) - 1) < 1e-9 && std::abs(std::abs(t.m22()) - 1) < 1e-9 &&
                           std::abs(t.m12()) < 1e-9 && std::abs(t.m21()) < 1e-9) ||
                          (std::abs(std::abs(t.m12()) - 1) < 1e-9 && std::abs(std::abs(t.m21()) - 1) < 1e-9 &&
                           std::abs(t.m11()) < 1e-9 && std::abs(t.m22()) < 1e-9);
    return unitAxes && whole(t.dx()) && whole(t.dy());
}
void paintImage(QPainter &p, const Layer &l, const State &s, QSize target, double opacity = 1) {
    if (l.image.isNull())
        return;
    p.save();
    p.scale(double(target.width()) / s.size.width(), double(target.height()) / s.size.height());
    p.setTransform(l.placement.matrix(l.image.size()), true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, l.placement.sampling != "Nearest" && !pixelAligned(p.transform()));
    p.setOpacity(opacity);
    p.drawImage(QPointF(0, 0), l.image);
    p.restore();
}
void multiply(QImage &image, const QImage &mask) {
    QPainter p(&image);
    p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    p.drawImage(0, 0, mask);
}
QImage ownSurface(const State &s, int i, QSize target) {
    const auto &l = s.layers[i];
    auto image = surface(target);
    {
        QPainter p(&image);
        paintImage(p, l, s, target, l.opacity);
    }
    if (!l.mask.isNull() && l.maskEnabled)
        multiply(image, renderMask(l, s.size, target));
    return image;
}
bool nativeMode(const QString &mode, QPainter::CompositionMode &out) {
    static const QHash<QString, QPainter::CompositionMode> modes{
        {"Normal", QPainter::CompositionMode_SourceOver},      {"Multiply", QPainter::CompositionMode_Multiply},
        {"Screen", QPainter::CompositionMode_Screen},          {"Overlay", QPainter::CompositionMode_Overlay},
        {"Darken", QPainter::CompositionMode_Darken},          {"Lighten", QPainter::CompositionMode_Lighten},
        {"Difference", QPainter::CompositionMode_Difference},  {"Exclusion", QPainter::CompositionMode_Exclusion},
        {"Color Dodge", QPainter::CompositionMode_ColorDodge}, {"Color Burn", QPainter::CompositionMode_ColorBurn},
        {"Soft Light", QPainter::CompositionMode_SoftLight},   {"Hard Light", QPainter::CompositionMode_HardLight}};
    const auto found = modes.constFind(mode);
    if (found == modes.cend())
        return false;
    out = found.value();
    return true;
}
} // namespace
static QRgb blendPixelCode(QRgb backdrop, QRgb source, int mode) {
    // Inputs and output use premultiplied alpha, as QImage's raster engine does.
    const double ab = qAlpha(backdrop) / 255., as = qAlpha(source) / 255.;
    if (as == 0)
        return backdrop;
    Color b{0, 0, 0}, s{qRed(source) / 255. / as, qGreen(source) / 255. / as, qBlue(source) / 255. / as};
    if (ab > 0)
        b = {qRed(backdrop) / 255. / ab, qGreen(backdrop) / 255. / ab, qBlue(backdrop) / 255. / ab};
    Color blend;
    if (mode == 20)
        blend = setLum(setSat(s, sat(b)), lum(b));
    else if (mode == 21)
        blend = setLum(setSat(b, sat(s)), lum(b));
    else if (mode == 22)
        blend = setLum(s, lum(b));
    else if (mode == 23)
        blend = setLum(b, lum(s));
    else
        for (int c = 0; c < 3; ++c)
            blend[c] = channel(b[c], s[c], mode);
    std::array<int, 3> result;
    const int alpha = qRound((as + ab * (1 - as)) * 255);
    for (int c = 0; c < 3; ++c)
        result[c] =
            std::clamp(qRound(((1 - as) * ab * b[c] + (1 - ab) * as * s[c] + ab * as * blend[c]) * 255), 0, alpha);
    return qRgba(result[0], result[1], result[2], alpha);
}
QRgb blendPixel(QRgb backdrop, QRgb source, const QString &mode) {
    return blendPixelCode(backdrop, source, blendModes().indexOf(mode));
}
QImage renderMask(const Layer &l, QSize canvas, QSize target) {
    if (wideImage(l.mask) || wideImage(l.image))
        return renderMask16(l, canvas, target);
    auto result = surface(target);
    if (l.mask.isNull() || !l.maskEnabled) {
        result.fill(Qt::white);
        return result;
    }
    QImage alpha(l.mask.size(), QImage::Format_ARGB32_Premultiplied);
    if (alpha.isNull())
        throw Error("tooLarge");
    const auto gray = l.mask.convertToFormat(QImage::Format_Grayscale8);
    for (int y = 0; y < gray.height(); ++y) {
        const auto *src = gray.constScanLine(y);
        auto *dst = reinterpret_cast<QRgb *>(alpha.scanLine(y));
        for (int x = 0; x < gray.width(); ++x)
            dst[x] = qRgba(src[x], src[x], src[x], src[x]);
    }
    QPainter p(&result);
    p.scale(double(target.width()) / canvas.width(), double(target.height()) / canvas.height());
    p.setTransform((l.maskLinked ? l.placement : l.maskPlacement).matrix(alpha.size()), true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, !pixelAligned(p.transform()));
    p.drawImage(0, 0, alpha);
    return result;
}
QImage layerSurface(const State &s, int index, QSize target) {
    if (hasHdr(s)) return layerSurfaceHdr(s, index, target);
    if (s.bitDepth == 16)
        return layerSurface16(s, index, target);
    auto result = ownSurface(s, index, target);
    // Clipping ignores source visibility and source RGB; every upstream link contributes alpha.
    QString source = s.layers[index].maskSource;
    while (!source.isEmpty()) {
        const int i = s.index(source);
        if (i < 0)
            break;
        multiply(result, ownSurface(s, i, target));
        source = s.layers[i].maskSource;
    }
    return result;
}
QImage render(const State &s, QSize target) {
    if (hasHdr(s)) return renderHdr(s, target);
    if (s.bitDepth == 16)
        return render16(s, target.isValid() ? target : s.size);
    if (!target.isValid())
        target = s.size;
    // Effects on a folder isolate its descendants before applying the folder's mask and opacity.
    for (int group = 0; group < s.layers.size(); ++group) {
        const auto &folder = s.layers[group];
        if (!folder.group || folder.metadata["effects"].toObject().isEmpty())
            continue;
        State nested = s;
        QSet<QString> ids;
        for (int child : s.descendants(folder.id))
            ids.insert(s.layers[child].id);
        nested.layers.removeIf([&](const Layer &l) { return !ids.contains(l.id); });
        for (auto &l : nested.layers)
            if (l.parent == folder.id)
                l.parent.clear();
        QImage image = render(nested, target);
        State replaced = s;
        replaced.layers.removeIf([&](const Layer &l) { return ids.contains(l.id); });
        const int index = replaced.index(folder.id);
        auto &flat = replaced.layers[index];
        flat.group = false;
        flat.image = image;
        flat.placement = {};
        flat.placement.size = s.size;
        flat.originalPng.clear();
        return render(replaced, target);
    }
    auto result = surface(target);
    for (int i : s.drawOrder()) {
        const auto &l = s.layers[i];
        if (l.group || !l.visible || (l.image.isNull() && !l.metadata["adjustment"].isObject()))
            continue;
        double opacity = l.opacity;
        bool visible = true,
             masked = (!l.mask.isNull() && l.maskEnabled) || !l.metadata["effects"].toObject().isEmpty();
        QVector<int> ancestors;
        QString parent = l.parent;
        while (!parent.isEmpty()) {
            const int p = s.index(parent);
            if (p < 0)
                break;
            const auto &group = s.layers[p];
            ancestors.append(p);
            opacity *= group.opacity;
            visible = visible && group.visible;
            masked = masked || (!group.mask.isNull() && group.maskEnabled);
            parent = group.parent;
        }
        if (!visible || opacity <= 0)
            continue;
        if (l.metadata["adjustment"].isObject()) {
            auto settings = l.metadata["adjustment"].toObject();
            if (settings["kind"] == "Gaussian Blur")
                settings["blurRadius"] = settings["blurRadius"].toDouble(10) * target.width() / s.size.width();
            const auto adjusted = filtered(result, settings);
            QImage coverage = surface(target);
            coverage.fill(Qt::white);
            if (!l.mask.isNull() && l.maskEnabled)
                multiply(coverage, renderMask(l, s.size, target));
            for (int ancestor : ancestors) {
                const auto &group = s.layers[ancestor];
                if (!group.mask.isNull() && group.maskEnabled)
                    multiply(coverage, renderMask(group, s.size, target));
            }
            if (!l.maskSource.isEmpty()) {
                const int source = s.index(l.maskSource);
                if (source >= 0)
                    multiply(coverage, layerSurface(s, source, target));
            }
            for (int y = 0; y < target.height(); ++y) {
                auto *dst = reinterpret_cast<QRgb *>(result.scanLine(y));
                const auto *src = reinterpret_cast<const QRgb *>(adjusted.constScanLine(y)),
                           *mask = reinterpret_cast<const QRgb *>(coverage.constScanLine(y));
                for (int x = 0; x < target.width(); ++x) {
                    const double a = opacity * qAlpha(mask[x]) / 255.;
                    QRgb change = src[x];
                    if (l.blend != "Normal")
                        change = blendPixel(dst[x], src[x], l.blend);
                    dst[x] = qRgba(qRound(qRed(dst[x]) * (1 - a) + qRed(change) * a),
                                   qRound(qGreen(dst[x]) * (1 - a) + qGreen(change) * a),
                                   qRound(qBlue(dst[x]) * (1 - a) + qBlue(change) * a),
                                   qRound(qAlpha(dst[x]) * (1 - a) + qAlpha(change) * a));
                }
            }
            continue;
        }
        QPainter::CompositionMode mode;
        const bool native = nativeMode(l.blend, mode);
        if (native && !masked && l.maskSource.isEmpty()) {
            QPainter p(&result);
            p.setCompositionMode(mode);
            paintImage(p, l, s, target, opacity);
            continue;
        }
        State visibleState = s;
        visibleState.layers[i].opacity = 1;
        QImage layer = layerSurface(visibleState, i, target);
        layer = applyEffects(layer, l.metadata["effects"].toObject(), double(target.width()) / s.size.width());
        if (l.opacity < 1) {
            QPainter p(&layer);
            p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            p.fillRect(layer.rect(), QColor(0, 0, 0, qRound(l.opacity * 255)));
        }
        double groupOpacity = 1;
        for (int ancestor : ancestors) {
            const auto &group = s.layers[ancestor];
            groupOpacity *= group.opacity;
            if (!group.mask.isNull() && group.maskEnabled)
                multiply(layer, renderMask(group, s.size, target));
        }
        if (groupOpacity < 1) {
            QPainter p(&layer);
            p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            p.fillRect(layer.rect(), QColor(0, 0, 0, qRound(groupOpacity * 255)));
        }
        if (native) {
            QPainter p(&result);
            p.setCompositionMode(mode);
            p.drawImage(0, 0, layer);
        } else {
            const int code = blendModes().indexOf(l.blend);
            for (int y = 0; y < target.height(); ++y) {
                auto *dst = reinterpret_cast<QRgb *>(result.scanLine(y));
                const auto *src = reinterpret_cast<const QRgb *>(layer.constScanLine(y));
                for (int x = 0; x < target.width(); ++x)
                    dst[x] = blendPixelCode(dst[x], src[x], code);
            }
        }
    }
    result.setDotsPerMeterX(qRound(s.resolution / .0254));
    result.setDotsPerMeterY(qRound(s.resolution / .0254));
    return result;
}
} // namespace comp
