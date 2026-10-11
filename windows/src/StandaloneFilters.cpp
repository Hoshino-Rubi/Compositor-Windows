#include "Filters.h"
#include "Pixels.h"
#include "WideProcessing.h"
#include "WideKernels.h"
#include <QFontDatabase>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <numeric>
extern "C" {
#include "AdjustPixels.h"
#include "DitherPixels.h"
#include "LensPixels.h"
}
namespace comp {
namespace {
QColor chosenColor(const QJsonObject &s, const char *key, QColor fallback) {
    QColor c(s[QLatin1String(key)].toString());
    return c.isValid() ? c : fallback;
}
QImage dithered(const QImage &source, const QJsonObject &s) {
    const int block = std::clamp(qRound(s["pixelSize"].toDouble(2)), 1, 32);
    const int style = std::clamp(s["style"].toInt(), 0, 9);
    const int actualBlock = style == DITHER_GLYPHS ? 1 : block;
    QSize small(std::max(1, (source.width() + actualBlock - 1) / actualBlock),
                std::max(1, (source.height() + actualBlock - 1) / actualBlock));
    QImage input = (actualBlock == 1 ? source : source.scaled(small, Qt::IgnoreAspectRatio, Qt::SmoothTransformation))
                       .convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    DitherParams p{};
    p.style = style;
    p.levels = std::clamp(qRound(s["levels"].toDouble(2)), 2, 8);
    p.diffusion = float(std::clamp(s["diffusion"].toDouble(100) / 100., 0., 1.));
    p.density = float(std::clamp(s["density"].toDouble() / 100., -1., 1.));
    p.contrast = float(std::clamp(s["contrast"].toDouble() / 100., -1., 1.));
    p.cell = std::clamp(qRound(s["cellSize"].toDouble(8)), 4, 64);
    p.angle = float(s["angle"].toDouble(45) * 3.14159265358979323846 / 180.);
    p.lightOnDark = s["lightOnDark"].toBool(true);
    p.originalColors = s["colors"].toInt() == 2;
    QColor dark = s["colors"].toInt() == 1 ? chosenColor(s, "dark", Qt::black) : QColor(Qt::black);
    QColor light = s["colors"].toInt() == 1 ? chosenColor(s, "light", Qt::white) : QColor(Qt::white);
    p.dark[0] = dark.red(); p.dark[1] = dark.green(); p.dark[2] = dark.blue();
    p.light[0] = light.red(); p.light[1] = light.green(); p.light[2] = light.blue();
    std::vector<std::pair<QByteArray, float>> glyphs;
    std::vector<quint8> maps;
    std::vector<float> coverages;
    if (style == DITHER_GLYPHS) {
        QString chars = s["characters"].toString(" .:-=+*#%@");
        if (chars.isEmpty()) chars = " .:-=+*#%@";
        chars = chars.left(64);
        QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        p.glyphHeight = std::clamp(qRound(s["textSize"].toDouble(14)), 6, 64);
        font.setPixelSize(std::max(5, p.glyphHeight - 2));
        font.setBold(true);
        QFontMetrics metrics(font);
        p.glyphWidth = std::max(1, metrics.horizontalAdvance('M'));
        for (const QChar ch : chars) {
            QImage glyph(p.glyphWidth, p.glyphHeight, QImage::Format_Grayscale8);
            glyph.fill(0);
            QPainter painter(&glyph);
            painter.setFont(font);
            painter.setPen(Qt::white);
            painter.drawText(QRect(0, 0, glyph.width(), glyph.height()), Qt::AlignCenter, QString(ch));
            painter.end();
            QByteArray map;
            int ink = 0;
            for (int y = 0; y < glyph.height(); ++y)
                for (int x = 0; x < glyph.width(); ++x) {
                    const quint8 value = glyph.constScanLine(y)[x];
                    map.append(char(value));
                    ink += value;
                }
            glyphs.emplace_back(map, float(ink) / float(255 * glyph.width() * glyph.height()));
        }
        std::sort(glyphs.begin(), glyphs.end(), [](const auto &a, const auto &b) { return a.second < b.second; });
        for (const auto &[map, coverage] : glyphs) {
            for (const char byte : map) maps.push_back(quint8(byte));
            coverages.push_back(coverage);
        }
        p.glyphs = maps.data(); p.glyphCoverage = coverages.data(); p.glyphCount = int(glyphs.size());
    }
    if (!dither_apply(input.bits(), input.width(), input.height(), input.bytesPerLine(), &p))
        throw Error("tooLarge");
    if (actualBlock > 1) {
        input = input.scaled(source.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation);
        if (s["pixelShape"].toInt() == 1) {
            const quint8 gap[3]{p.dark[0], p.dark[1], p.dark[2]};
            dither_dots(input.bits(), input.width(), input.height(), input.bytesPerLine(), block, gap);
        }
    }
    if (!wideImage(source)) return input.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QImage result = input.convertToFormat(QImage::Format_RGBA64_Premultiplied);
    const auto original = source.convertToFormat(QImage::Format_RGBA64_Premultiplied);
    // Dithering reduces colors deliberately, but must not quantize 16-bit transparency.
    for (int y = 0; y < result.height(); ++y) {
        auto *row = reinterpret_cast<QRgba64 *>(result.scanLine(y));
        const auto *before = reinterpret_cast<const QRgba64 *>(original.constScanLine(y));
        for (int x = 0; x < result.width(); ++x) {
            const int oldAlpha = row[x].alpha(), newAlpha = before[x].alpha();
            row[x] = rgba64(oldAlpha ? (quint64(row[x].red()) * newAlpha / oldAlpha) : 0,
                            oldAlpha ? (quint64(row[x].green()) * newAlpha / oldAlpha) : 0,
                            oldAlpha ? (quint64(row[x].blue()) * newAlpha / oldAlpha) : 0, newAlpha);
        }
    }
    return result;
}
} // namespace
bool isStandaloneFilter(const QString &kind) {
    return QStringList{"Vignette", "Bloom / Glow", "Dither", "Tonal Contrast", "Lens Correction"}.contains(kind);
}
QImage standaloneFilter(const QImage &source, const QJsonObject &settings) {
    const QString kind = settings["kind"].toString();
    if (kind == "Dither") return dithered(source, settings);
    const bool wide = wideImage(source);
    QImage rgba = source.convertToFormat(wide ? QImage::Format_RGBA64_Premultiplied
                                              : QImage::Format_RGBA8888_Premultiplied);
    if (rgba.isNull()) throw Error("tooLarge");
    const int w = rgba.width(), h = rgba.height(), stride = rgba.bytesPerLine() / (wide ? 2 : 1);
    auto *pixels16 = reinterpret_cast<quint16 *>(rgba.bits());
    auto *pixels8 = rgba.bits();
    if (kind == "Vignette") {
        QColor c = chosenColor(settings, "vignetteColor", Qt::black);
        auto apply = [&](auto *pixels, auto fn) {
            fn(pixels, w, h, stride, 0, 0, w, h, 0,
               settings["vignetteAmount"].toDouble(35), settings["vignetteMidpoint"].toDouble(50),
               settings["vignetteRoundness"].toDouble(100), settings["vignetteFeather"].toDouble(60),
               settings["vignetteHighlights"].toDouble(25), c.redF(), c.greenF(), c.blueF());
        };
        if (wide) apply(pixels16, wide::adjust_colored_vignette);
        else apply(pixels8, adjust_colored_vignette);
    } else if (kind == "Bloom / Glow") {
        const double amount = std::clamp(settings["bloomAmount"].toDouble(40), 0., 100.);
        if (amount > 0) {
            QImage blurred = blurImage(rgba, std::clamp(settings["bloomRadius"].toDouble(24), 1., 150.));
            if (wide) {
                blurred = blurred.convertToFormat(QImage::Format_RGBA64_Premultiplied);
                wide::dither_glow(pixels16, reinterpret_cast<const quint16 *>(blurred.constBits()), w, h,
                                  stride, float(amount / 50.));
            } else {
                blurred = blurred.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
                dither_glow(pixels8, blurred.constBits(), w, h, stride, float(amount / 50.));
            }
        }
    } else if (kind == "Tonal Contrast") {
        QImage blurred = blurImage(rgba, std::clamp(settings["tonalRadius"].toDouble(16), 1., 150.));
        auto apply = [&](auto *pixels, auto *base, auto fn, int baseStride) {
            fn(pixels, base, w, h, stride, baseStride, settings["tonalAmount"].toDouble(50),
               settings["tonalShadows"].toDouble(40), settings["tonalMidtones"].toDouble(60),
               settings["tonalHighlights"].toDouble(30));
        };
        if (wide) {
            blurred = blurred.convertToFormat(QImage::Format_RGBA64_Premultiplied);
            apply(pixels16, reinterpret_cast<const quint16 *>(blurred.constBits()), wide::adjust_tonal_contrast,
                  blurred.bytesPerLine() / 2);
        } else {
            blurred = blurred.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
            apply(pixels8, blurred.constBits(), adjust_tonal_contrast, blurred.bytesPerLine());
        }
    } else if (kind == "Lens Correction") {
        QImage result(rgba.size(), rgba.format());
        if (result.isNull()) throw Error("tooLarge");
        const double distortion = std::clamp(settings["distortion"].toDouble(), -100., 100.) * .0035;
        if (wide)
            wide::lens_distort(pixels16, reinterpret_cast<quint16 *>(result.bits()), w, h, stride, distortion);
        else
            lens_distort(pixels8, result.bits(), w, h, stride, distortion);
        return result.convertToFormat(source.format());
    }
    return rgba.convertToFormat(source.format());
}
} // namespace comp
