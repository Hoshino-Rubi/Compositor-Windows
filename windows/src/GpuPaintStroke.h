#pragma once

#include <QColor>
#include <QImage>
#include <QPainterPath>
#include <QPointF>
#include <QTransform>

namespace comp {

// Try to render one Canvas brush segment through the GPU paint kernel.  The
// caller supplies document-space points and a document-to-layer-pixel
// transform (normally the inverse Placement matrix).  A false result means
// that the request was not suitable for the accelerated path or that the GPU
// request failed; in either case target is left untouched and the reference
// Canvas implementation can replay the segment.
//
// `documentSelection` is the hard/path selection.  Feathered selections are
// intentionally not folded into the per-dab coverage; Canvas constrains the
// result against its selection mask once the complete segment has finished.
// For clone/blur, retouchSource contains the source layer.  cloneOffset is in
// document coordinates; it is transformed to a constant source-pixel offset
// for the affine documentToPixel mapping.
bool paintStrokeAccelerated(QImage &target, QPointF from, QPointF to, double brushSize, double hardness,
                            double flow, QColor color, bool erase, bool maskTarget,
                            const QTransform &documentToPixel, const QPainterPath &documentSelection,
                            const QImage &retouchSource = {}, QPointF cloneOffset = {},
                            bool cloneOrBlur = false);

} // namespace comp
