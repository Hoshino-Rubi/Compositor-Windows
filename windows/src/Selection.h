#pragma once
#include "Project.h"
namespace comp {
QImage subjectMask(const QImage &image, QPoint seed = QPoint(-1, -1));
QImage selectionCoverage(const State &state, QSize target, const Placement &placement);
void constrainSelection(const State &state, const Placement &placement, const QImage &before, QImage &after);
} // namespace comp
