#pragma once
#include "Project.h"
namespace comp {
struct ImportResult {
    State state;
    QStringList notices;
};
ImportResult readPhotoshop(const QString &path, qint64 pixelBudget = MaxSourcePixels);
QStringList imageFilesInFolder(const QString &directory);
bool isRawPath(const QString &path);
struct RawFrame {
    QImage linear;
    QString camera;
};
RawFrame decodeRaw(const QString &path);
QImage developRaw(const RawFrame &frame, double exposure = 0, double temperature = 6500, double tint = 0,
                  double boost = 1, int limit = 0, int bitDepth = 16);
QImage readHeif(const QString &path);
QImage readSvg(const QString &path);
} // namespace comp
