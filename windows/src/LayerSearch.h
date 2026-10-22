#pragma once
#include "Project.h"
#include <QSet>
namespace comp {
QSet<QString> matchingLayers(const State &state, const QString &name, const QString &type, const QStringList &selected);
}
