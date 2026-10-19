#include "Cmyk.h"
#include "Pixels.h"
#include <lcms2.h>
#include <jpeglib.h>
#include <QSettings>
#include <QPainter>
#include <QDataStream>
#include <QBuffer>
#include <setjmp.h>
#include <memory>
namespace comp {
namespace {
struct Profile {
    cmsHPROFILE value = nullptr;
    explicit Profile(const QByteArray &data) {
        if (data.isEmpty() || data.size() > 4 * 1024 * 1024)
            throw Error("cmykProfileRequired");
        value = cmsOpenProfileFromMem(data.constData(), cmsUInt32Number(data.size()));
        if (!value || cmsGetColorSpace(value) != cmsSigCmykData) {
            if (value)
                cmsCloseProfile(value);
            value = nullptr;
            throw Error("cmykInvalidProfile");
        }
    }
    ~Profile() {
        if (value)
            cmsCloseProfile(value);
    }
};
struct Transform {
    cmsHTRANSFORM value = nullptr;
    ~Transform() {
        if (value)
            cmsDeleteTransform(value);
    }
};
QImage whiteRgb(const QImage &image) {
    QImage result(image.size(), QImage::Format_RGBX64);
    if (result.isNull())
        throw Error("tooLarge");
    result.fill(Qt::white);
    QPainter p(&result);
    p.drawImage(0, 0, image);
    p.end();
    return result;
}
struct JpegError {
    jpeg_error_mgr manager;
    jmp_buf jump;
};
void jpegFail(j_common_ptr common) { longjmp(reinterpret_cast<JpegError *>(common->err)->jump, 1); }
QByteArray cmykJpeg(const QImage &image, const QByteArray &profile, int quality, double dpi) {
    jpeg_compress_struct encoder{};
    JpegError error{};
    encoder.err = jpeg_std_error(&error.manager);
    error.manager.error_exit = jpegFail;
    unsigned char *memory = nullptr;
    unsigned long length = 0;
    QByteArray row(image.width() * 4, 0), marker;
    if (setjmp(error.jump)) {
        jpeg_destroy_compress(&encoder);
        free(memory);
        throw Error("fileError");
    }
    jpeg_create_compress(&encoder);
    jpeg_mem_dest(&encoder, &memory, &length);
    encoder.image_width = image.width();
    encoder.image_height = image.height();
    encoder.input_components = 4;
    encoder.in_color_space = JCS_CMYK;
    jpeg_set_defaults(&encoder);
    jpeg_set_quality(&encoder, std::clamp(quality, 1, 100), TRUE);
    encoder.density_unit = 1;
    encoder.X_density = encoder.Y_density = UINT16(std::clamp(qRound(dpi), 1, 65535));
    jpeg_start_compress(&encoder, TRUE);
    constexpr int chunk = 65519;
    const int count = (profile.size() + chunk - 1) / chunk;
    for (int i = 0; i < count; ++i) {
        marker = QByteArray("ICC_PROFILE\0", 12);
        marker.append(char(i + 1));
        marker.append(char(count));
        marker += profile.mid(i * chunk, chunk);
        jpeg_write_marker(&encoder, JPEG_APP0 + 2, reinterpret_cast<const JOCTET *>(marker.constData()),
                          unsigned(marker.size()));
    }
    // Adobe CMYK JPEG stores inverted ink channels.
    while (encoder.next_scanline < encoder.image_height) {
        const auto *src = image.constScanLine(int(encoder.next_scanline));
        for (int i = 0; i < row.size(); ++i)
            row[i] = char(255 - src[i]);
        auto *line = reinterpret_cast<JSAMPLE *>(row.data());
        jpeg_write_scanlines(&encoder, &line, 1);
    }
    jpeg_finish_compress(&encoder);
    QByteArray result(reinterpret_cast<char *>(memory), qsizetype(length));
    jpeg_destroy_compress(&encoder);
    free(memory);
    return result;
}
QByteArray cmykTiff(const QImage &image, const QByteArray &profile, double dpi) {
    QByteArray result;
    QDataStream out(&result, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);
    const quint16 entries = 13;
    const quint32 bits = 8 + 2 + entries * 12 + 4, res = bits + 8, icc = res + 16,
                  pixels = icc + quint32(profile.size());
    out.writeRawData("II", 2);
    out << quint16(42) << quint32(8) << entries;
    auto tag = [&](quint16 id, quint16 type, quint32 count, quint32 value) {
        out << id << type << count << value;
    };
    tag(256, 4, 1, image.width());
    tag(257, 4, 1, image.height());
    tag(258, 3, 4, bits);
    tag(259, 3, 1, 1);
    tag(262, 3, 1, 5);
    tag(273, 4, 1, pixels);
    tag(277, 3, 1, 4);
    tag(278, 4, 1, image.height());
    tag(279, 4, 1, image.width() * image.height() * 4);
    tag(282, 5, 1, res);
    tag(283, 5, 1, res + 8);
    tag(296, 3, 1, 2);
    tag(34675, 7, quint32(profile.size()), icc);
    out << quint32(0);
    for (int c = 0; c < 4; ++c)
        out << quint16(8);
    for (int i = 0; i < 2; ++i)
        out << quint32(qRound(dpi * 1000)) << quint32(1000);
    out.writeRawData(profile.constData(), profile.size());
    for (int y = 0; y < image.height(); ++y)
        out.writeRawData(reinterpret_cast<const char *>(image.constScanLine(y)), image.width() * 4);
    return result;
}
QByteArray cmykPdf(const QImage &image, const QByteArray &profile, double dpi) {
    QByteArray raw;
    raw.reserve(image.width() * image.height() * 4);
    for (int y = 0; y < image.height(); ++y)
        raw.append(reinterpret_cast<const char *>(image.constScanLine(y)), image.width() * 4);
    auto deflate = [](const QByteArray &bytes) { return qCompress(bytes, 6).mid(4); };
    auto stream = [](QByteArray attributes, QByteArray data) {
        return "<< " + attributes + " /Length " + QByteArray::number(data.size()) + " >>\nstream\n" + data +
               "\nendstream";
    };
    const auto w = QByteArray::number(image.width() * 72. / dpi, 'f', 6),
               h = QByteArray::number(image.height() * 72. / dpi, 'f', 6);
    QVector<QByteArray> objects{
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + w + " " + h +
            "] /Resources << /XObject << /Im0 5 0 R >> >> /Contents 4 0 R >>",
        stream({}, "q\n" + w + " 0 0 " + h + " 0 0 cm\n/Im0 Do\nQ"),
        stream("/Type /XObject /Subtype /Image /Width " + QByteArray::number(image.width()) + " /Height " +
                   QByteArray::number(image.height()) +
                   " /ColorSpace [/ICCBased 6 0 R] /BitsPerComponent 8 /Filter /FlateDecode",
               deflate(raw)),
        stream("/N 4 /Alternate /DeviceCMYK /Filter /FlateDecode", deflate(profile))};
    QByteArray pdf = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    QVector<qsizetype> offsets;
    for (int i = 0; i < objects.size(); ++i) {
        offsets.append(pdf.size());
        pdf += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const qsizetype xref = pdf.size();
    pdf += "xref\n0 7\n0000000000 65535 f \n";
    for (auto offset : offsets)
        pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    pdf += "trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    return pdf;
}
} // namespace
void validateCmykProfile(const QByteArray &data) { Profile profile(data); }
QByteArray chosenCmykProfile() { return QSettings().value("print/cmykProfile").toByteArray(); }
void chooseCmykProfile(const QByteArray &data) {
    validateCmykProfile(data);
    QSettings().setValue("print/cmykProfile", data);
}
QString cmykProfileName(const QByteArray &data) {
    Profile profile(data);
    char name[1024]{};
    cmsGetProfileInfoASCII(profile.value, cmsInfoDescription, "en", "US", name, sizeof(name));
    return QString::fromUtf8(name);
}
QImage cmykToRgb(const QImage &cmyk, const QByteArray &data) {
    Profile profile(data);
    if (cmyk.format() != QImage::Format_CMYK8888)
        throw Error("imageError");
    cmsHPROFILE rgb = cmsCreate_sRGBProfile();
    Transform transform;
    transform.value = cmsCreateTransform(profile.value, TYPE_CMYK_8, rgb, TYPE_RGBA_16,
                                         INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_BLACKPOINTCOMPENSATION);
    cmsCloseProfile(rgb);
    if (!transform.value)
        throw Error("cmykInvalidProfile");
    QImage out(cmyk.size(), QImage::Format_RGBA64);
    if (out.isNull())
        throw Error("tooLarge");
    out.fill(Qt::white);
    for (int y = 0; y < out.height(); ++y)
        cmsDoTransform(transform.value, cmyk.constScanLine(y), out.scanLine(y), out.width());
    out.setColorSpace(QColorSpace::SRgb);
    return out.convertToFormat(QImage::Format_RGBA64_Premultiplied);
}
QImage rgbToCmyk(const QImage &image, const QByteArray &data) {
    Profile profile(data);
    const auto rgb = whiteRgb(image);
    cmsHPROFILE srgb = cmsCreate_sRGBProfile();
    Transform transform;
    transform.value = cmsCreateTransform(srgb, TYPE_RGBA_16, profile.value, TYPE_CMYK_8,
                                         INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_BLACKPOINTCOMPENSATION);
    cmsCloseProfile(srgb);
    if (!transform.value)
        throw Error("cmykInvalidProfile");
    QImage out(rgb.size(), QImage::Format_CMYK8888);
    if (out.isNull())
        throw Error("tooLarge");
    for (int y = 0; y < out.height(); ++y)
        cmsDoTransform(transform.value, rgb.constScanLine(y), out.scanLine(y), out.width());
    return out;
}
QImage cmykProof(const QImage &image, const QByteArray &data, bool warning) {
    if (image.isNull())
        return {};
    Profile profile(data);
    const auto rgb = whiteRgb(image);
    QImage out(rgb.size(), QImage::Format_RGBX64);
    if (out.isNull())
        throw Error("tooLarge");
    cmsContext context = cmsCreateContext(nullptr, nullptr);
    if (!context)
        throw Error("tooLarge");
    cmsUInt16Number alarm[cmsMAXCHANNELS]{};
    alarm[0] = alarm[1] = alarm[2] = 32768;
    cmsSetAlarmCodesTHR(context, alarm);
    cmsHPROFILE srgb = cmsCreate_sRGBProfileTHR(context);
    Transform transform;
    transform.value = cmsCreateProofingTransformTHR(
        context, srgb, TYPE_RGBA_16, srgb, TYPE_RGBA_16, profile.value, INTENT_RELATIVE_COLORIMETRIC,
        INTENT_RELATIVE_COLORIMETRIC,
        cmsFLAGS_SOFTPROOFING | cmsFLAGS_BLACKPOINTCOMPENSATION | (warning ? cmsFLAGS_GAMUTCHECK : 0));
    if (transform.value) {
        out.fill(Qt::white);
        for (int y = 0; y < out.height(); ++y)
            cmsDoTransform(transform.value, rgb.constScanLine(y), out.scanLine(y), out.width());
    }
    cmsCloseProfile(srgb);
    if (transform.value) {
        cmsDeleteTransform(transform.value);
        transform.value = nullptr;
    } else {
        cmsDeleteContext(context);
        throw Error("cmykInvalidProfile");
    }
    cmsDeleteContext(context);
    out.setColorSpace(QColorSpace::SRgb);
    return out;
}
QByteArray encodeCmyk(const QImage &image, const QByteArray &profile, QByteArray format, double dpi,
                      int quality) {
    const auto inks = rgbToCmyk(image, profile);
    if (format == "tiff")
        return cmykTiff(inks, profile, dpi);
    if (format == "jpeg")
        return cmykJpeg(inks, profile, quality, dpi);
    if (format == "pdf")
        return cmykPdf(inks, profile, dpi);
    throw Error("unsupportedFeature", "CMYK export format");
}
} // namespace comp
