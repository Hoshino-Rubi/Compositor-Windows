#pragma once
#include "Project.h"
namespace comp {
QImage render16(const State &, QSize);
QImage renderMask16(const Layer &, QSize, QSize);
QImage layerSurface16(const State &, int, QSize);
QPainterPath alphaPath16(const QImage &);
QPainterPath wandPath16(const QImage &, QPoint, int, bool);
QImage filtered16(const QImage &, const QJsonObject &);
QImage blurImage16(const QImage &, double);
QImage applyEffects16(const QImage &, const QJsonObject &, double);
QImage cameraRaw16(const QImage &, const QJsonObject &, double = 1, int = 0);
QImage perspectiveWarp16(const QImage &, const QPolygonF &);
void warpDab16(QImage &, QPointF, QPointF, double, double, double, bool, const QPainterPath &);
}
