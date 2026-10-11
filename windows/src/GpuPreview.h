#pragma once
#include "Project.h"
namespace comp {
// A null image requests the reference CPU renderer (unsupported document or unavailable device).
QImage renderGpuPreview(const State &state, QSize target);
QString gpuPreviewStatus();
} // namespace comp
