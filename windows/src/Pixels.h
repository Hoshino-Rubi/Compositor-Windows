#pragma once
#include "Project.h"
#include "Hdr.h"
#include <QColorSpace>
#include <algorithm>
#include <cmath>
namespace comp {
inline bool wideImage(const QImage &image) {
    return image.pixelFormat().redSize() > 8 || image.format() == QImage::Format_Grayscale16;
}
inline QImage::Format pixelFormat(int depth) {
    return depth == 16 ? QImage::Format_RGBA64_Premultiplied : QImage::Format_ARGB32_Premultiplied;
}
inline QImage::Format pixelFormat(const QImage &image) {
    return hdrImage(image) ? QImage::Format_RGBA32FPx4_Premultiplied : pixelFormat(wideImage(image) ? 16 : 8);
}
inline QImage::Format maskFormat(int depth) {
    return depth == 16 ? QImage::Format_Grayscale16 : QImage::Format_Grayscale8;
}
inline QRgba64 rgba64(int r, int g, int b, int a) {
    return QRgba64::fromRgba64(quint16(std::clamp(r, 0, 65535)), quint16(std::clamp(g, 0, 65535)),
                               quint16(std::clamp(b, 0, 65535)), quint16(std::clamp(a, 0, 65535)));
}
inline int wideRed(QRgba64 p) {
    return p.red();
}
inline int wideGreen(QRgba64 p) {
    return p.green();
}
inline int wideBlue(QRgba64 p) {
    return p.blue();
}
inline int wideAlpha(QRgba64 p) {
    return p.alpha();
}
inline QRgba64 pixel64(const QImage &image, int x, int y) {
    if (image.format() == QImage::Format_RGBA64_Premultiplied || image.format() == QImage::Format_RGBA64)
        return reinterpret_cast<const QRgba64 *>(image.constScanLine(y))[x];
    return image.pixelColor(x, y).rgba64();
}
inline QColor grayColor16(QColor color) {
    const auto p = color.rgba64();
    const quint16 gray = quint16((quint32(p.red()) * 11 + quint32(p.green()) * 16 + quint32(p.blue()) * 5) / 32);
    return QColor::fromRgba64(gray, gray, gray, p.alpha());
}
inline QColor coverageColor(double alpha) {
    return QColor::fromRgbF(0, 0, 0, std::clamp(alpha, 0., 1.));
}
inline QImage maskImage(const QImage &input, int depth) {
    if (depth == 8 || input.format() == QImage::Format_Grayscale16)
        return input.convertToFormat(maskFormat(depth));
    const auto rgba = input.convertToFormat(QImage::Format_RGBA64);
    QImage out(rgba.size(), QImage::Format_Grayscale16);
    if (!input.isNull() && out.isNull())
        throw Error("tooLarge");
    for (int y = 0; y < rgba.height(); ++y) {
        const auto *src = reinterpret_cast<const QRgba64 *>(rgba.constScanLine(y));
        auto *dst = reinterpret_cast<quint16 *>(out.scanLine(y));
        for (int x = 0; x < rgba.width(); ++x)
            dst[x] =
                quint16((quint32(src[x].red()) * 11 + quint32(src[x].green()) * 16 + quint32(src[x].blue()) * 5) / 32);
    }
    return out;
}
inline QImage alphaFromGray(const QImage &input, int depth) {
    const auto gray = maskImage(input, depth);
    QImage alpha(gray.size(), pixelFormat(depth));
    if (alpha.isNull())
        throw Error("tooLarge");
    for (int y = 0; y < gray.height(); ++y) {
        if (depth == 16) {
            auto *dst = reinterpret_cast<QRgba64 *>(alpha.scanLine(y));
            auto *src = reinterpret_cast<const quint16 *>(gray.constScanLine(y));
            for (int x = 0; x < gray.width(); ++x)
                dst[x] = rgba64(src[x], src[x], src[x], src[x]);
        } else {
            auto *dst = reinterpret_cast<QRgb *>(alpha.scanLine(y));
            auto *src = gray.constScanLine(y);
            for (int x = 0; x < gray.width(); ++x)
                dst[x] = qRgba(src[x], src[x], src[x], src[x]);
        }
    }
    return alpha;
}
inline QImage grayFromAlpha(const QImage &input, int depth) {
    const auto alpha = input.convertToFormat(pixelFormat(depth));
    QImage gray(alpha.size(), maskFormat(depth));
    if (gray.isNull())
        throw Error("tooLarge");
    for (int y = 0; y < gray.height(); ++y) {
        if (depth == 16) {
            auto *src = reinterpret_cast<const QRgba64 *>(alpha.constScanLine(y));
            auto *dst = reinterpret_cast<quint16 *>(gray.scanLine(y));
            for (int x = 0; x < gray.width(); ++x)
                dst[x] = src[x].alpha();
        } else {
            auto *src = reinterpret_cast<const QRgb *>(alpha.constScanLine(y));
            auto *dst = gray.scanLine(y);
            for (int x = 0; x < gray.width(); ++x)
                dst[x] = qAlpha(src[x]);
        }
    }
    return gray;
}
inline void normalizePrecision(State &state) {
    for (const auto &l : state.layers)
        if (wideImage(l.image) || wideImage(l.mask))
            state.bitDepth = 16;
    if (state.bitDepth != 16)
        return;
    for (auto &l : state.layers) {
        if (!l.image.isNull() && !hdrImage(l.image))
            l.image = l.image.convertToFormat(pixelFormat(l.metadata.value("windowsLayerBitDepth").toInt(16)));
        if (!l.mask.isNull())
            l.mask = maskImage(l.mask, 16);
    }
    if (!state.selectionMask.isNull())
        state.selectionMask = maskImage(state.selectionMask, 16);
}
} // namespace comp
