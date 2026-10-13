#pragma once

#include <QImage>
#include <QPolygonF>

namespace comp {

// Strict GPU probe.  It never falls back to the CPU renderer: an unavailable
// device, unsupported size/transform, failed shader/resource operation, or
// non-Windows build returns a null image.  A successful result has the same
// bounding-box dimensions and native 8/16-bit format as perspectiveWarp().
QImage perspectiveGpuImage(const QImage &source, const QPolygonF &destination);

// Accelerated public operation.  It preserves perspectiveWarp()'s exception
// keys (`invalidTransform` and `tooLarge`) by replaying that reference path
// whenever the strict GPU probe declines the request.  Color-space and DPI
// metadata are copied to either result.
QImage perspectiveWarpAccelerated(const QImage &source, const QPolygonF &destination);

} // namespace comp
