#pragma once
#include "Project.h"
namespace comp {
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
