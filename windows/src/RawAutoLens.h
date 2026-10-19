#pragma once
#include "Importers.h"
#include <memory>
namespace comp {
struct RawLensData;
struct RawLensCorrection {
    std::shared_ptr<const RawLensData> data;
    QString profile;
    QString status = "rawLensNoMatch";
    bool distortion = false;
    bool vignetting = false;
    bool embedded = false;
    bool available() const { return data && (distortion || vignetting); }
};
RawLensCorrection automaticRawLens(const RawFrame &frame, const QByteArray &database = {});
RawLensCorrection manualRawLens(const RawFrame &frame, const QByteArray &xml);
QImage applyRawLens(const RawFrame &frame, const RawLensCorrection &correction, int previewLimit = 0);
QImage orientRawImage(const QImage &image, int orientation, bool inverse = false);
} // namespace comp
