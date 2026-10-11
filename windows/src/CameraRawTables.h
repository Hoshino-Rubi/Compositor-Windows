#pragma once
#include "Project.h"
extern "C" {
#include "AdjustPixels.h"
}
namespace comp::rawtables {
inline constexpr int Size = 17, Grid = 33;
bool measuredCurve(const QJsonObject &curve);
QVector<CameraRawStage> stages(const QJsonObject &settings, const QImage &image);
QVector<CameraRawStage> stagesWithStatistics(const QJsonObject &settings, const double brightness[3]);
QVector<CameraRawStage> mixer(const QJsonObject &settings);
QVector<CameraRawStage> grading(const QJsonObject &settings);
QVector<float> compose(const QVector<CameraRawStage> &stages, bool identity = false);
const uint8_t *exposure();
QJsonObject neutralize(double red, double green, double blue);
} // namespace comp::rawtables
