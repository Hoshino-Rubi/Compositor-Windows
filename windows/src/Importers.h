#pragma once
#include "Project.h"
namespace comp {
struct ImportResult {
    State state;
    QStringList notices;
    QVector<QPair<QString, State>> artboards;
};
QVector<QPair<QString, State>> photoshopArtboards(const State &state);
ImportResult readPhotoshop(const QString &path, qint64 pixelBudget = MaxSourcePixels);
QStringList imageFilesInFolder(const QString &directory);
bool isRawPath(const QString &path);
struct RawFrame {
    QImage linear;
    QString camera;
    QString lens;
    double focal = 0;
    QByteArray encoded;
    QString make, model, lensMake;
    double aperture = 0;
    int orientation = 0;
    QByteArray dngOpcodes;
    QRect dngDefaultCrop;
    bool embeddedGeometryCompatible = true;
};
RawFrame decodeRaw(const QString &path, double noiseReduction = 0, bool highlightRecovery = false);
RawFrame decodeRawBytes(const QByteArray &bytes, double noiseReduction, bool highlightRecovery);
QImage correctRawLens(const QImage &image, const QByteArray &profile, double focal);
QImage developRaw(const RawFrame &frame, double exposure = 0, double temperature = 6500, double tint = 0,
                  double boost = 1, int limit = 0, int bitDepth = 16);
QImage readHeif(const QString &path);
QImage readSvg(const QString &path);
} // namespace comp
