#include "Pixels.h"
#include "Importers.h"
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QPainter>
#include <QColorSpace>
#include <QSvgRenderer>
#include <libraw/libraw.h>
#include <libheif/heif.h>
#include <memory>
#include <cmath>
extern "C" {
#include "DitherPixels.h"
}
namespace comp {
static QByteArray fileBytes(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw Error("fileError");
    if (file.size() > 512ll * 1024 * 1024)
        throw Error("tooLarge");
    return file.readAll();
}
static void checkSize(QSize size) {
    if (!size.isValid() || size.width() > 30000 || size.height() > 30000 ||
        qint64(size.width()) * size.height() > MaxSourcePixels)
        throw Error("tooLarge");
}
bool isRawPath(const QString &path) {
    return QStringList{"3fr", "ari", "arw", "bay", "cap", "cr2", "cr3", "crw", "dcr", "dcs", "dng", "drf", "eip", "erf",
                       "fff", "gpr", "iiq", "k25", "kdc", "mdc", "mef", "mos", "mrw", "nef", "nrw", "obm", "orf", "pef",
                       "ptx", "pxn", "r3d", "raf", "raw", "rw2", "rwl", "rwz", "sr2", "srf", "srw", "x3f"}
        .contains(QFileInfo(path).suffix().toLower());
}
QStringList imageFilesInFolder(const QString &directory) {
    const QFileInfo folder(directory);
    if (!folder.isDir() || folder.suffix().compare("comp", Qt::CaseInsensitive) == 0) return {};
    const QStringList extensions{"png", "jpg", "jpeg", "webp", "bmp", "tif", "tiff", "svg", "svgz", "heic", "heif", "hif", "psd", "psb"};
    QStringList paths;
    const auto entries = QDir(directory).entryInfoList(QDir::Files | QDir::Readable | QDir::NoSymLinks, QDir::Name | QDir::IgnoreCase);
    for (const auto &entry : entries) {
        if (!extensions.contains(entry.suffix().toLower()) && !isRawPath(entry.filePath())) continue;
        if (paths.size() >= MaxLayers) throw Error("tooLarge");
        paths.append(entry.absoluteFilePath());
    }
    return paths;
}
RawFrame decodeRaw(const QString &path) {
    auto bytes = fileBytes(path);
    LibRaw raw;
    int code = raw.open_buffer(bytes.data(), size_t(bytes.size()));
    if (code != LIBRAW_SUCCESS)
        throw Error("rawError", QString::fromLatin1(libraw_strerror(code)));
    checkSize({raw.imgdata.sizes.width, raw.imgdata.sizes.height});
    raw.imgdata.params.use_camera_wb = 1;
    raw.imgdata.params.no_auto_bright = 1;
    raw.imgdata.params.output_bps = 16;
    raw.imgdata.params.output_color = 1;
    raw.imgdata.params.gamm[0] = 1;
    raw.imgdata.params.gamm[1] = 1;
    raw.imgdata.params.user_qual = 3;
    if ((code = raw.unpack()) != LIBRAW_SUCCESS || (code = raw.dcraw_process()) != LIBRAW_SUCCESS)
        throw Error("rawError", QString::fromLatin1(libraw_strerror(code)));
    std::unique_ptr<libraw_processed_image_t, decltype(&LibRaw::dcraw_clear_mem)> decoded(
        raw.dcraw_make_mem_image(&code), LibRaw::dcraw_clear_mem);
    if (!decoded || code != LIBRAW_SUCCESS || decoded->type != LIBRAW_IMAGE_BITMAP || decoded->bits != 16 ||
        decoded->colors < 3)
        throw Error("rawError", "Unsupported decoded pixel layout");
    checkSize({decoded->width, decoded->height});
    QImage image(decoded->width, decoded->height, QImage::Format_RGBA64);
    if (image.isNull())
        throw Error("tooLarge");
    const auto *src = reinterpret_cast<const quint16 *>(decoded->data);
    for (int y = 0; y < image.height(); ++y) {
        auto *dst = reinterpret_cast<QRgba64 *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const auto *p = src + (qint64(y) * image.width() + x) * decoded->colors;
            dst[x] = QRgba64::fromRgba64(p[0], p[1], p[2], 65535);
        }
    }
    return {image, QString::fromLatin1(raw.imgdata.idata.make) + " " + QString::fromLatin1(raw.imgdata.idata.model)};
}
QImage developRaw(const RawFrame &frame, double exposure, double temperature, double tint, double boost, int limit,
                  int bitDepth) {
    QImage source = frame.linear;
    if (limit > 0 && (source.width() > limit || source.height() > limit))
        source = source.scaled(limit, limit, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QImage wide(source.size(), QImage::Format_RGBA64);
    if (wide.isNull())
        throw Error("tooLarge");
    const double warmth = std::clamp(log(std::clamp(temperature, 2000., 50000.) / 6500.), -1.5, 1.5);
    const double gain = std::pow(2., std::clamp(exposure, -10., 10.));
    const double gains[]{std::exp(warmth * .45 + tint * .0015), std::exp(-tint * .003),
                         std::exp(-warmth * .45 + tint * .0015)};
    auto encode = [&](double v, int c) {
        v = std::clamp(v * gains[c] * gain, 0., 1.);
        double srgb = v <= .0031308 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - .055;
        const double contrast = std::clamp(boost, 0., 2.) - 1;
        srgb = std::clamp((srgb - .5) * (1 + contrast * .25) + .5, 0., 1.);
        return quint16(qRound(srgb * 65535));
    };
    for (int y = 0; y < source.height(); ++y) {
        const auto *src = reinterpret_cast<const QRgba64 *>(source.constScanLine(y));
        auto *dst = reinterpret_cast<QRgba64 *>(wide.scanLine(y));
        for (int x = 0; x < source.width(); ++x)
            dst[x] = QRgba64::fromRgba64(encode(src[x].red() / 65535., 0), encode(src[x].green() / 65535., 1),
                                         encode(src[x].blue() / 65535., 2), 65535);
    }
    if (bitDepth == 16) {
        auto native = wide.convertToFormat(QImage::Format_RGBA64_Premultiplied);
        native.setColorSpace(QColorSpace::SRgb);
        return native;
    }
    QImage result(source.size(), QImage::Format_RGBA8888_Premultiplied);
    if (result.isNull())
        throw Error("tooLarge");
    dither_quantize16(reinterpret_cast<const uint16_t *>(wide.constBits()), result.bits(), result.width(),
                      result.height(), result.bytesPerLine());
    result = result.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    result.setColorSpace(QColorSpace::SRgb);
    return result;
}
QImage readHeif(const QString &path) {
    const auto bytes = fileBytes(path);
    std::unique_ptr<heif_context, decltype(&heif_context_free)> ctx(heif_context_alloc(), heif_context_free);
    if (!ctx)
        throw Error("tooLarge");
    auto check = [](heif_error e) {
        if (e.code != heif_error_Ok)
            throw Error("imageError", QString::fromUtf8(e.message));
    };
    check(heif_context_read_from_memory_without_copy(ctx.get(), bytes.constData(), size_t(bytes.size()), nullptr));
    heif_image_handle *handle = nullptr;
    check(heif_context_get_primary_image_handle(ctx.get(), &handle));
    std::unique_ptr<heif_image_handle, decltype(&heif_image_handle_release)> owner(handle, heif_image_handle_release);
    checkSize({heif_image_handle_get_width(handle), heif_image_handle_get_height(handle)});
    heif_image *decoded = nullptr;
    const bool high = heif_image_handle_get_luma_bits_per_pixel(handle) > 8;
    check(heif_decode_image(handle, &decoded, heif_colorspace_RGB,
                            high ? heif_chroma_interleaved_RRGGBBAA_LE : heif_chroma_interleaved_RGBA, nullptr));
    std::unique_ptr<heif_image, decltype(&heif_image_release)> decodedOwner(decoded, heif_image_release);
    int stride = 0;
    const auto *pixels = heif_image_get_plane_readonly(decoded, heif_channel_interleaved, &stride);
    if (!pixels)
        throw Error("imageError");
    QImage image(pixels, heif_image_get_primary_width(decoded), heif_image_get_primary_height(decoded), stride,
                 high ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    if (high) {
        const int bits = heif_image_get_bits_per_pixel_range(decoded, heif_channel_interleaved);
        if (bits < 9 || bits > 16)
            throw Error("imageError");
        image = image.copy();
        const quint32 maximum = (1u << bits) - 1;
        for (int y = 0; y < image.height(); ++y) {
            auto *row = reinterpret_cast<quint16 *>(image.scanLine(y));
            for (int x = 0; x < image.width() * 4; ++x)
                row[x] = quint16((quint32(row[x]) * 65535u + maximum / 2) / maximum);
        }
    }
    auto result = image.convertToFormat(pixelFormat(high ? 16 : 8));
    const auto profileSize = heif_image_handle_get_raw_color_profile_size(handle);
    if (profileSize > 0 && profileSize < 4 * 1024 * 1024) {
        QByteArray profile(qsizetype(profileSize), 0);
        if (heif_image_handle_get_raw_color_profile(handle, profile.data()).code == heif_error_Ok) {
            auto colorSpace = QColorSpace::fromIccProfile(profile);
            if (colorSpace.isValid()) {
                result.setColorSpace(colorSpace);
                result.convertToColorSpace(QColorSpace::SRgb);
            }
        }
    }
    return result;
}
QImage readSvg(const QString &path) {
    auto bytes = fileBytes(path);
    if (bytes.size() > 32 * 1024 * 1024)
        throw Error("tooLarge");
    QSvgRenderer renderer(bytes);
    if (!renderer.isValid())
        throw Error("imageError");
    QSize size = renderer.defaultSize();
    checkSize(size);
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull())
        throw Error("tooLarge");
    image.fill(Qt::transparent);
    QPainter p(&image);
    renderer.render(&p);
    return image;
}
} // namespace comp
