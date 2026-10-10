#pragma once
#include "Project.h"
namespace comp {
void exportDocument(const State &state, const QString &path, const QByteArray &format, QSize size, int quality = 92);
}
