#include "Importers.h"
#include "Pixels.h"
#include <QXmlStreamReader>
#include <QVector>
namespace comp {
QImage correctRawLens(const QImage &image, const QByteArray &profile, double focal) {
    if (profile.isEmpty())
        return image;
    if (profile.size() > 4 * 1024 * 1024)
        throw Error("tooLarge");
    QXmlStreamReader xml(profile);
    struct Calibration {
        double focal;
        QString model;
        double a, b, c;
    };
    QVector<Calibration> values;
    int lenses = 0;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == u"lens")
            ++lenses;
        if (xml.isStartElement() && xml.name() == u"distortion") {
            const auto attrs = xml.attributes();
            const auto model = attrs.value(u"model").toString();
            if (model != "poly3" && model != "poly5" && model != "ptlens")
                continue;
            Calibration entry{attrs.value(u"focal").toDouble(), model, 0, 0, 0};
            entry.a = attrs.value(model == "ptlens" ? u"a" : u"k1").toDouble();
            entry.b = attrs.value(model == "ptlens" ? u"b" : u"k2").toDouble();
            entry.c = attrs.value(u"c").toDouble();
            if (entry.focal > 0 && std::isfinite(entry.a) && std::isfinite(entry.b) && std::isfinite(entry.c))
                values.append(entry);
        }
    }
    if (xml.hasError() || values.isEmpty() || lenses != 1)
        throw Error("rawLensProfileError");
    auto calibration = *std::min_element(values.begin(), values.end(), [&](const auto &a, const auto &b) {
        return std::abs(a.focal - focal) < std::abs(b.focal - focal);
    });
    const auto source = image.convertToFormat(QImage::Format_RGBA64);
    QImage result(source.size(), source.format());
    if (result.isNull())
        throw Error("tooLarge");
    const double cx = source.width() / 2., cy = source.height() / 2., unit = std::min(cx, cy);
    for (int y = 0; y < source.height(); ++y) {
        auto *out = reinterpret_cast<QRgba64 *>(result.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            const double nx = (x + .5 - cx) / unit, ny = (y + .5 - cy) / unit, r = std::hypot(nx, ny);
            const double factor = calibration.model == "poly3" ? 1 - calibration.a + calibration.a * r * r
                                  : calibration.model == "poly5"
                                      ? 1 + calibration.a * r * r + calibration.b * r * r * r * r
                                      : calibration.a * r * r * r + calibration.b * r * r +
                                            calibration.c * r + 1 - calibration.a - calibration.b -
                                            calibration.c;
            const double sx = cx + nx * factor * unit - .5, sy = cy + ny * factor * unit - .5;
            if (sx < 0 || sy < 0 || sx > source.width() - 1 || sy > source.height() - 1) {
                out[x] = QRgba64::fromRgba64(0, 0, 0, 65535);
                continue;
            }
            const int ix = int(sx), iy = int(sy), jx = std::min(ix + 1, source.width() - 1),
                      jy = std::min(iy + 1, source.height() - 1);
            const double fx = sx - ix, fy = sy - iy;
            const auto a = pixel64(source, ix, iy), b = pixel64(source, jx, iy), c = pixel64(source, ix, jy),
                       d = pixel64(source, jx, jy);
            auto mix = [&](int aa, int bb, int cc, int dd) {
                return quint16(qRound((aa * (1 - fx) + bb * fx) * (1 - fy) + (cc * (1 - fx) + dd * fx) * fy));
            };
            out[x] = QRgba64::fromRgba64(mix(a.red(), b.red(), c.red(), d.red()),
                                         mix(a.green(), b.green(), c.green(), d.green()),
                                         mix(a.blue(), b.blue(), c.blue(), d.blue()), 65535);
        }
    }
    return result;
}
} // namespace comp
