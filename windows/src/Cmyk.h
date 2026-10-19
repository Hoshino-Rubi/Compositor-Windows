#pragma once
#include "Project.h"
namespace comp {
struct CmykProfileEntry { QString name, path; QByteArray data; bool removable = false; };
QVector<CmykProfileEntry> installedCmykProfiles();
void removeCmykProfile(const QString &path);
void validateCmykProfile(const QByteArray &profile);
QByteArray chosenCmykProfile();
void chooseCmykProfile(const QByteArray &profile);
QString cmykProfileName(const QByteArray &profile);
QImage cmykToRgb(const QImage &cmyk, const QByteArray &profile);
QImage rgbToCmyk(const QImage &image, const QByteArray &profile);
QImage cmykProof(const QImage &image, const QByteArray &profile, bool gamutWarning = false);
QByteArray encodeCmyk(const QImage &image, const QByteArray &profile, QByteArray format, double dpi,
                      int quality);
} // namespace comp
