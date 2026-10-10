#pragma once
#include "Project.h"
namespace comp {
QImage filtered(const QImage &source, const QJsonObject &settings);
QImage blurImage(const QImage &source, double radius);
QPainterPath alphaPath(const QImage &image);
QPainterPath wandPath(const QImage &image, QPoint point, int tolerance, bool contiguous);
bool supportedAdjustment(const QJsonObject &settings);
void validateAdjustment(const QJsonObject &settings);
} // namespace comp
