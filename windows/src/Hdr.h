#pragma once
#include "Project.h"
#include <functional>
namespace comp {
struct FloatPixel {
    float r, g, b, a;
};
inline double floatRed(FloatPixel p) { return p.r; }
inline double floatGreen(FloatPixel p) { return p.g; }
inline double floatBlue(FloatPixel p) { return p.b; }
inline double floatAlpha(FloatPixel p) { return p.a; }
inline FloatPixel floatRgba(double r, double g, double b, double a) {
    return {float(r), float(g), float(b), float(a)};
}
inline double floatRound(double value) { return value; }
bool hdrImage(const QImage &image);
bool hasHdr(const State &state);
QImage readExr(const QByteArray &bytes);
QImage readRadiance(const QByteArray &bytes);
QByteArray exrBytes(const QImage &image);
QImage hdrThrough(const QImage &source, const std::function<QImage(const QImage &)> &operation);
QImage hdrFiltered(const QImage &source, const QJsonObject &settings);
QImage hdrBlur(const QImage &source, double radius);
QImage renderHdr(const State &state, QSize target = {});
QImage layerSurfaceHdr(const State &state, int index, QSize target);
QImage renderMaskHdr(const Layer &layer, QSize canvas, QSize target);
FloatPixel blendPixelHdr(FloatPixel backdrop, FloatPixel source, const QString &mode);
} // namespace comp
