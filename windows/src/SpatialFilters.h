#pragma once
#include "Project.h"
#include <QPainter>
namespace comp {
QPointF linePixelPoint(QPointF point, double width);
void strokeShapeLine(QPainter &p, QPointF start, QPointF end, double width, const QString &style, const QString &cap, QColor color);
QPainterPath roundedShapePath(QRectF rect, double radius, bool smoothing);
QImage blurPreservingTransparency(const QImage &source, double radius);
QImage motionBlurClamped(const QImage &source, const QJsonObject &settings);
QImage progressiveBlur(const QImage &source, const QJsonObject &settings);
void expandLayerPixels(Layer &layer, QRect bounds);
void prepareBlurBounds(Layer &layer, QSize canvas, const QJsonObject &settings);
} // namespace comp
