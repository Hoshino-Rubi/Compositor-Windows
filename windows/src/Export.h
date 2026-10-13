#pragma once
#include "Project.h"
namespace comp {
// Export keeps the reference renderer as the default.  AcceleratedGpu is an
// explicit opt-in because Direct3D sampling and effect edges can differ from
// the integer CPU reference (notably for strokes).
enum class ExportRenderer { ReferenceCpu, AcceleratedGpu };

QImage renderExportImage(const State &state, QSize size,
                         ExportRenderer renderer = ExportRenderer::ReferenceCpu);

void exportDocument(const State &state, const QString &path, const QByteArray &format, QSize size,
                    int quality = 92,
                    ExportRenderer renderer = ExportRenderer::ReferenceCpu);
}
