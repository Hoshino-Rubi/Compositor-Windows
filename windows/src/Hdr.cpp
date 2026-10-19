#include "Hdr.h"
#include "Pixels.h"
#include "Filters.h"
#include "SpatialFilters.h"
#include "CameraRaw.h"
#include <QPainter>
#include <QRegularExpression>
#include <zlib.h>
#define TINYEXR_USE_MINIZ 0
#define TINYEXR_IMPLEMENTATION
#include "../third_party/tinyexr/tinyexr.h"
namespace comp {
bool hdrImage(const QImage &image) {
    return image.format() == QImage::Format_RGBA32FPx4 ||
           image.format() == QImage::Format_RGBA32FPx4_Premultiplied ||
           image.format() == QImage::Format_RGBA16FPx4 ||
           image.format() == QImage::Format_RGBA16FPx4_Premultiplied;
}
bool hasHdr(const State &state) {
    for (const auto &layer : state.layers)
        if (hdrImage(layer.image))
            return true;
    return false;
}
namespace {
void sizeCheck(qint64 w, qint64 h) {
    if (w < 1 || h < 1 || w > MaxSide || h > MaxSide ||
        w * h > documentCapacity(256, 16000000, MaxSourcePixels))
        throw Error("tooLarge");
}
void exrCheck(int code, const char *error) {
    if (code == TINYEXR_SUCCESS)
        return;
    const auto message = error ? QString::fromUtf8(error) : QString("EXR decode/encode failure");
    if (error)
        FreeEXRErrorMessage(error);
    throw Error("imageError", message);
}
} // namespace
QImage readExr(const QByteArray &bytes) {
    EXRVersion version{};
    exrCheck(ParseEXRVersionFromMemory(&version, reinterpret_cast<const unsigned char *>(bytes.constData()),
                                       bytes.size()),
             nullptr);
    if (version.multipart || version.non_image)
        throw Error("unsupportedFeature", "Multipart/deep EXR");
    EXRHeader header;
    InitEXRHeader(&header);
    const char *error = nullptr;
    const int status = ParseEXRHeaderFromMemory(
        &header, &version, reinterpret_cast<const unsigned char *>(bytes.constData()), bytes.size(), &error);
    if (status != TINYEXR_SUCCESS) {
        FreeEXRHeader(&header);
        exrCheck(status, error);
    }
    const qint64 w = qint64(header.data_window.max_x) - header.data_window.min_x + 1,
                 h = qint64(header.data_window.max_y) - header.data_window.min_y + 1;
    FreeEXRHeader(&header);
    sizeCheck(w, h);
    float *pixels = nullptr;
    int width = 0, height = 0;
    error = nullptr;
    const int loaded =
        LoadEXRFromMemory(&pixels, &width, &height,
                          reinterpret_cast<const unsigned char *>(bytes.constData()), bytes.size(), &error);
    if (loaded != TINYEXR_SUCCESS) {
        free(pixels);
        exrCheck(loaded, error);
    }
    std::unique_ptr<float, decltype(&free)> owner(pixels, free);
    if (width != w || height != h)
        throw Error("imageError");
    QImage image(width, height, QImage::Format_RGBA32FPx4);
    if (image.isNull())
        throw Error("tooLarge");
    for (int y = 0; y < height; ++y) {
        auto *out = reinterpret_cast<float *>(image.scanLine(y));
        for (int x = 0; x < width * 4; ++x) {
            const float v = pixels[(qint64(y) * width * 4) + x];
            out[x] = std::isfinite(v) ? ((x % 4 == 3) ? std::clamp(v, 0.f, 1.f) : v) : 0.f;
        }
    }
    image.setColorSpace(QColorSpace::SRgbLinear);
    image.convertToColorSpace(QColorSpace::SRgb);
    return image.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
}
QByteArray exrBytes(const QImage &source) {
    sizeCheck(source.width(), source.height());
    auto image = source.convertToFormat(QImage::Format_RGBA32FPx4);
    if (!image.colorSpace().isValid())
        image.setColorSpace(QColorSpace::SRgb);
    image.convertToColorSpace(QColorSpace::SRgbLinear);
    unsigned char *memory = nullptr;
    const char *error = nullptr;
    const int length = SaveEXRToMemory(reinterpret_cast<const float *>(image.constBits()), image.width(),
                                       image.height(), 4, 0, &memory, &error);
    if (length <= 0) {
        if (memory)
            free(memory);
        exrCheck(length, error);
        throw Error("fileError");
    }
    QByteArray bytes(reinterpret_cast<const char *>(memory), length);
    free(memory);
    return bytes;
}
QImage readRadiance(const QByteArray &bytes) {
    qsizetype pos = 0;
    auto line = [&]() {
        const auto end = bytes.indexOf('\n', pos);
        if (end < 0)
            throw Error("imageError");
        auto result = bytes.mid(pos, end - pos).trimmed();
        pos = end + 1;
        return result;
    };
    const auto magic = line();
    if (magic != "#?RADIANCE" && magic != "#?RGBE")
        throw Error("imageError");
    bool format = false;
    for (;;) {
        auto l = line();
        if (l.isEmpty())
            break;
        if (l == "FORMAT=32-bit_rle_rgbe")
            format = true;
    }
    if (!format)
        throw Error("imageError");
    const auto match =
        QRegularExpression("^([+-])Y ([0-9]+) ([+-])X ([0-9]+)$").match(QString::fromLatin1(line()));
    if (!match.hasMatch())
        throw Error("imageError");
    const int height = match.captured(2).toInt(), width = match.captured(4).toInt();
    sizeCheck(width, height);
    QImage result(width, height, QImage::Format_RGBA32FPx4);
    if (result.isNull())
        throw Error("tooLarge");
    QByteArray scan(width * 4, 0);
    auto byte = [&]() {
        if (pos >= bytes.size())
            throw Error("imageError");
        return quint8(bytes[pos++]);
    };
    for (int y = 0; y < height; ++y) {
        int a = byte(), b = byte(), c = byte(), d = byte();
        if (width >= 8 && width < 32768 && a == 2 && b == 2 && !(c & 128)) {
            if (((c << 8) | d) != width)
                throw Error("imageError");
            for (int channel = 0; channel < 4; ++channel) {
                int x = 0;
                while (x < width) {
                    const int count = byte();
                    if (!count)
                        throw Error("imageError");
                    if (count > 128) {
                        int n = count - 128, v = byte();
                        if (x + n > width)
                            throw Error("imageError");
                        while (n--)
                            scan[(x++) * 4 + channel] = char(v);
                    } else {
                        if (x + count > width)
                            throw Error("imageError");
                        for (int n = 0; n < count; ++n)
                            scan[(x++) * 4 + channel] = char(byte());
                    }
                }
            }
        } else {
            int x = 0, shift = 0;
            while (x < width) {
                if (a == 1 && b == 1 && c == 1) {
                    if (x == 0 || shift > 16 || d == 0)
                        throw Error("imageError");
                    const int count = d << shift;
                    if (count > width - x)
                        throw Error("imageError");
                    const auto previous = scan.mid((x - 1) * 4, 4);
                    for (int i = 0; i < count; ++i) {
                        memcpy(scan.data() + x * 4, previous.constData(), 4);
                        ++x;
                    }
                    shift += 8;
                } else {
                    scan[x * 4] = char(a);
                    scan[x * 4 + 1] = char(b);
                    scan[x * 4 + 2] = char(c);
                    scan[x * 4 + 3] = char(d);
                    ++x;
                    shift = 0;
                }
                if (x < width) {
                    a = byte();
                    b = byte();
                    c = byte();
                    d = byte();
                }
            }
        }
        const int row = match.captured(1) == "-" ? y : height - 1 - y;
        auto *out = reinterpret_cast<FloatPixel *>(result.scanLine(row));
        for (int x = 0; x < width; ++x) {
            const int e = quint8(scan[x * 4 + 3]);
            const float scale = e ? std::ldexp(1.f, e - 136) : 0;
            const int col = match.captured(3) == "+" ? x : width - 1 - x;
            out[col] = {quint8(scan[x * 4]) * scale, quint8(scan[x * 4 + 1]) * scale,
                        quint8(scan[x * 4 + 2]) * scale, 1};
        }
    }
    result.setColorSpace(QColorSpace::SRgbLinear);
    result.convertToColorSpace(QColorSpace::SRgb);
    return result.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
}
QImage hdrThrough(const QImage &source, const std::function<QImage(const QImage &)> &operation) {
    const auto input = source.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
    QImage normalized(input.size(), QImage::Format_RGBA64_Premultiplied);
    if (normalized.isNull())
        throw Error("tooLarge");
    QVector<float> scales(qsizetype(input.width()) * input.height());
    for (int y = 0; y < input.height(); ++y) {
        const auto *src = reinterpret_cast<const FloatPixel *>(input.constScanLine(y));
        auto *dst = reinterpret_cast<QRgba64 *>(normalized.scanLine(y));
        for (int x = 0; x < input.width(); ++x) {
            const auto p = src[x];
            const double a = std::clamp(double(p.a), 0., 1.);
            const float scale = a > 0 ? float(std::max({1., p.r / a, p.g / a, p.b / a})) : 1;
            scales[y * input.width() + x] = scale;
            dst[x] = rgba64(qRound(p.r / scale * 65535), qRound(p.g / scale * 65535),
                            qRound(p.b / scale * 65535), qRound(a * 65535));
        }
    }
    auto changed = operation(normalized).convertToFormat(QImage::Format_RGBA64_Premultiplied);
    if (changed.size() != input.size())
        throw Error("invalidSize");
    QImage out(input.size(), QImage::Format_RGBA32FPx4_Premultiplied);
    if (out.isNull())
        throw Error("tooLarge");
    out.setColorSpace(QColorSpace::SRgb);
    for (int y = 0; y < out.height(); ++y) {
        const auto *src = reinterpret_cast<const QRgba64 *>(changed.constScanLine(y));
        auto *dst = reinterpret_cast<FloatPixel *>(out.scanLine(y));
        for (int x = 0; x < out.width(); ++x) {
            const auto before = reinterpret_cast<const QRgba64 *>(normalized.constScanLine(y))[x];
            if (src[x] == before) {
                dst[x] = reinterpret_cast<const FloatPixel *>(input.constScanLine(y))[x];
                continue;
            }
            const float s = scales[y * out.width() + x] / 65535.f;
            dst[x] = {src[x].red() * s, src[x].green() * s, src[x].blue() * s, src[x].alpha() / 65535.f};
        }
    }
    return out;
}
QImage hdrBlur(const QImage &source, double radius) {
    if (radius <= 0)
        return source;
    QImage result = source.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
    const int r = std::clamp(qRound(radius * .58), 1, 250);
    for (int pass = 0; pass < 3; ++pass)
        for (int axis = 0; axis < 2; ++axis) {
            QImage out(result.size(), result.format());
            if (out.isNull())
                throw Error("tooLarge");
            const int n = axis ? result.height() : result.width(),
                      rows = axis ? result.width() : result.height();
            for (int row = 0; row < rows; ++row) {
                double sums[4]{};
                auto at = [&](int i) {
                    i = std::clamp(i, 0, n - 1);
                    return reinterpret_cast<const float *>(result.constScanLine(axis ? i : row)) +
                           (axis ? row : i) * 4;
                };
                auto add = [&](int i, int sign) {
                    const auto *p = at(i);
                    for (int c = 0; c < 4; ++c)
                        sums[c] += p[c] * sign;
                };
                for (int i = -r; i <= r; ++i)
                    add(i, 1);
                for (int i = 0; i < n; ++i) {
                    auto *p = reinterpret_cast<float *>(out.scanLine(axis ? i : row)) + (axis ? row : i) * 4;
                    for (int c = 0; c < 4; ++c)
                        p[c] = float(sums[c] / (2 * r + 1));
                    add(i - r, -1);
                    add(i + r + 1, 1);
                }
            }
            result = out;
        }
    result.setColorSpace(QColorSpace::SRgb);
    return result;
}
QImage hdrFiltered(const QImage &source, const QJsonObject &settings) {
    const auto kind = settings["kind"].toString();
    if (kind == "Motion Blur") {
        auto image = source.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
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
            auto *src = reinterpret_cast<const FloatPixel *>(rotated.constScanLine(y));
            auto *dst = reinterpret_cast<FloatPixel *>(average.scanLine(y));
            double sum[4] = {};
            auto add = [&](int x, int sign) {
                if (x < 0 || x >= rotated.width())
                    return;
                auto v = src[x];
                sum[0] += floatRed(v) * sign;
                sum[1] += floatGreen(v) * sign;
                sum[2] += floatBlue(v) * sign;
                sum[3] += floatAlpha(v) * sign;
            };
            for (int k = -radius; k <= radius; ++k)
                add(k, 1);
            for (int x = 0; x < rotated.width(); ++x) {
                dst[x] = floatRgba(sum[0] / count, sum[1] / count, sum[2] / count, sum[3] / count);
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
    if (kind == "Progressive Blur")
        return progressiveBlur(source, settings);
    if (kind == "Exposure") {
        auto image = source.convertToFormat(QImage::Format_RGBA32FPx4);
        auto s = settings["exposureSettings"].toObject();
        const double gain = std::pow(2., std::clamp(s["exposure"].toDouble(), -20., 20.)),
                     offset = s["offset"].toDouble(),
                     gamma = 1 / std::clamp(s["gamma"].toDouble(1), .01, 9.99);
        for (int y = 0; y < image.height(); ++y) {
            auto *p = reinterpret_cast<float *>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x)
                for (int c = 0; c < 3; ++c)
                    p[x * 4 + c] = float(std::pow(std::max(0., p[x * 4 + c] * gain + offset), gamma));
        }
        return image.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
    }
    if (kind == "Gaussian Blur") {
        auto blurred = hdrBlur(source, settings["blurRadius"].toDouble(5));
        if (settings["preserveTransparency"].toBool()) {
            const auto original = source.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
            for (int y = 0; y < blurred.height(); ++y) {
                auto *p = reinterpret_cast<FloatPixel *>(blurred.scanLine(y));
                const auto *a = reinterpret_cast<const FloatPixel *>(original.constScanLine(y));
                for (int x = 0; x < blurred.width(); ++x) {
                    const float ratio = p[x].a > 0 ? a[x].a / p[x].a : 0;
                    p[x] = {p[x].r * ratio, p[x].g * ratio, p[x].b * ratio, a[x].a};
                }
            }
        }
        return blurred;
    }
    return hdrThrough(source, [&](const QImage &normal) { return filtered(normal, settings); });
}
} // namespace comp
