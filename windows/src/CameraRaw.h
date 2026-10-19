#pragma once
#include "Project.h"
namespace comp {
QJsonObject resetCameraRawSection(QJsonObject settings, const QString &section);
QJsonObject normalizedCameraRaw(QJsonObject settings);
QImage cameraRaw(const QImage &image, const QJsonObject &settings, double scale = 1, int clipping = 0);
QJsonObject autoWhiteBalance(const QImage &image);
double curveValue(double x, const QJsonArray &points);
} // namespace comp
