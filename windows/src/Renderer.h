#pragma once
#include "Project.h"
namespace comp {
QImage render(const State &state, QSize target = {});
QImage layerSurface(const State &state, int layer, QSize target);
QImage renderMask(const Layer &layer, QSize canvas, QSize target);
QRgb blendPixel(QRgb backdrop, QRgb source, const QString &mode);
} // namespace comp
