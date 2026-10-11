#pragma once
#include "Project.h"
#include <QTextDocument>
namespace comp {
void loadText(QTextDocument &document, const QJsonObject &style);
QJsonObject textStyle(const QTextDocument &document, QJsonObject base);
QImage renderText(const QJsonObject &style, int bitDepth = 8);
} // namespace comp
