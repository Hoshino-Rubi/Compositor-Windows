#pragma once
#include "Project.h"
namespace comp {
void warpDab(QImage &image, QPointF from, QPointF to, double radius, double hardness, double strength, bool smudge,
             const QPainterPath &selection = {});
QImage perspectiveWarp(const QImage &image, const QPolygonF &destination);
} // namespace comp
