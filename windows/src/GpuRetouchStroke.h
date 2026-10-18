#pragma once
#include <QImage>
#include <QPointF>
#include <QVector>
#include <memory>
namespace comp {
struct GpuRetouchStroke;
QImage retouchGpuSegment(std::shared_ptr<GpuRetouchStroke> &stroke, const QImage &original, const QImage &current,
                         const QVector<QPointF> &points, float radius, float hardness, float strength, bool smudge,
                         const QImage &coverage);
} // namespace comp
