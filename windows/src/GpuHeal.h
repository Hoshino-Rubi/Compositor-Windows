#pragma once

#include <QImage>
#include <cstdint>

namespace comp {

// Runs the Direct3D 11 healing pipeline on a complete image.  A null image means
// that the hardware route was unavailable, unsupported, over budget, or failed
// before the result was validated; source is never modified.
QImage healGpuImage(const QImage &source, const QImage &coverage, float opacity, int mode,
                    uint32_t seed = 0);

} // namespace comp
