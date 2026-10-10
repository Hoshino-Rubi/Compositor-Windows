#pragma once
#include "Project.h"
namespace comp {
void validateEffects(const QJsonObject &effects);
QImage applyEffects(const QImage &source, const QJsonObject &effects, double scale);
} // namespace comp
