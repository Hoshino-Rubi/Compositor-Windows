#pragma once
#include "Project.h"
namespace comp {
QJsonObject transformFilterLayer(QJsonObject settings, const QTransform &mapping, double scale);
QJsonObject defaultFilterLayerSettings(QSize canvas);
bool isFilterLayerKind(const QString &kind);
QJsonObject mappedFilterLayer(const QJsonObject &settings, QSize canvas, QSize target);
QImage rgbSplit(const QImage &source, const QJsonObject &settings);
bool isStandaloneFilter(const QString &kind);
QImage standaloneFilter(const QImage &source, const QJsonObject &settings);
QImage filtered(const QImage &source, const QJsonObject &settings);
QImage blurImage(const QImage &source, double radius);
QPainterPath alphaPath(const QImage &image);
QPainterPath wandPath(const QImage &image, QPoint point, int tolerance, bool contiguous);
bool supportedAdjustment(const QJsonObject &settings);
void validateAdjustment(const QJsonObject &settings);
} // namespace comp
