#pragma once

#include <QImage>
#include <QPainterPath>
#include <QPointF>
#include <QVector>

namespace comp {
// Runs an ordered set of retouch dabs on a cropped image.  The transfer/GPU
// implementation returns a null image when the hardware path cannot be used;
// callers must keep the source unchanged until a non-null result is returned.
// `points` are in the source image's coordinate system.  When present,
// `selectionMask` is a binary Grayscale8 image with the same size as source.
QImage warpGpuImage(const QImage &source, const QVector<QPointF> &points, double radius, double hardness,
                    double strength, bool smudge, const QImage &selectionMask = {});

// Applies one pointer-move segment (which may contain many dabs), using the GPU
// for sufficiently large regions and the reference dab implementation
// otherwise.  The input image is changed only after the GPU result has passed
// all size/format checks.
void warpStrokeAccelerated(QImage &image, const QVector<QPointF> &points, double radius, double hardness,
                           double strength, bool smudge, const QPainterPath &selection = {});
} // namespace comp
