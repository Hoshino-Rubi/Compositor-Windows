#include "RawAutoLens.h"
#include "Pixels.h"
#include "PixelParallel.h"
#include <lensfun/lensfun.h>
#include <QFile>
#include <QResource>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QtEndian>
#include <array>
#include <bit>
#include <vector>
#include <cmath>
#include <cstring>
#include <algorithm>
static void initLensDatabase() { Q_INIT_RESOURCE(lensdatabase); }
namespace comp {
struct DngLensOperation {
    int kind = 0;
    std::array<double, 6> k{};
    double cx = .5, cy = .5;
};
struct RawLensData {
    std::shared_ptr<lfDatabase> database;
    const lfLens *lens = nullptr;
    double crop = 1, focal = 0, aperture = 0;
    int flags = 0;
    std::vector<DngLensOperation> operations;
};
namespace {
QString name(const char *value) { return value ? QString::fromUtf8(value) : QString{}; }
QString normalized(QString value) {
    value = value.toCaseFolded();
    value.replace(QRegularExpression("[^\\p{L}\\p{N}.]+"), "");
    return value;
}
QString maker(QString value) {
    value = normalized(value);
    for (const auto &brand : {"canon", "nikon", "sony", "fujifilm", "panasonic", "olympus", "pentax", "sigma", "tamron"})
        if (value.startsWith(QLatin1String(brand))) return QLatin1String(brand);
    return value;
}
QString modelName(QString value, const QString &brand) {
    value = normalized(value);
    if (!brand.isEmpty() && value.startsWith(brand)) value.remove(0, brand.size());
    return value;
}
bool matchesModel(const char *models, const QString &value, const QString &brand) {
    if (!models) return false;
    const auto wanted = modelName(value,brand);
    if (modelName(name(models),brand) == wanted) return true;
    auto p = models + strlen(models) + 1;
    while (*p) {
        p += strlen(p) + 1;
        if (modelName(name(p),brand) == wanted) return true;
        p += strlen(p) + 1;
    }
    return false;
}
std::shared_ptr<lfDatabase> loadDatabase(const QByteArray &bytes) {
    if (bytes.isEmpty() || bytes.size() > 16 * 1024 * 1024) throw Error("rawLensProfileError");
    QXmlStreamReader xml(bytes);
    const QStringList numeric{"focal", "aperture", "distance", "a", "b", "c", "k1", "k2", "k3"};
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement() || (xml.name() != u"distortion" && xml.name() != u"vignetting")) continue;
        for (const auto &attribute : xml.attributes()) if (numeric.contains(attribute.name().toString())) {
            bool valid = false;
            const double value = attribute.value().toDouble(&valid);
            if (!valid || !std::isfinite(value)) throw Error("rawLensProfileError");
        }
    }
    if (xml.hasError()) throw Error("rawLensProfileError");
    auto db = std::make_shared<lfDatabase>();
    if (db->Load("Compositor lens database", bytes.constData(), size_t(bytes.size())) != LF_NO_ERROR)
        throw Error("rawLensProfileError");
    return db;
}
std::shared_ptr<lfDatabase> bundledDatabase() {
    static const auto db = [] {
        initLensDatabase();
        QFile file(":/lenses/database.xml");
        if (!file.open(QIODevice::ReadOnly)) throw Error("rawLensDatabaseError");
        return loadDatabase(file.readAll());
    }();
    return db;
}
const lfCamera *cameraFor(const lfDatabase &db, const RawFrame &frame) {
    const auto brand = maker(frame.make), model = modelName(frame.model, brand);
    if (brand.isEmpty() || model.isEmpty()) return nullptr;
    const lfCamera *found = nullptr;
    const auto cameras = db.GetCameras();
    for (int i = 0; cameras && cameras[i]; ++i) {
        const auto camera = cameras[i];
        if (maker(name(camera->Maker)) != brand || !matchesModel(camera->Model, frame.model, brand)) continue;
        if (found && (std::abs(found->CropFactor - camera->CropFactor) > .01 || name(found->Mount) != name(camera->Mount))) return nullptr;
        found = camera;
    }
    return found;
}
RawLensCorrection fromLens(std::shared_ptr<lfDatabase> db, const lfLens *lens, const RawFrame &frame, double crop) {
    RawLensCorrection result;
    if (!lens || !std::isfinite(frame.focal) || frame.focal <= 0 || crop <= 0 || !std::isfinite(crop)) {
        result.status = "rawLensMissingMetadata";
        return result;
    }
    auto data = std::make_shared<RawLensData>();
    data->database = std::move(db); data->lens = lens; data->crop = crop; data->focal = frame.focal;
    data->aperture = std::isfinite(frame.aperture) && frame.aperture > 0 ? frame.aperture : 0;
    const int requested = LF_MODIFY_DISTORTION | (data->aperture > 0 ? LF_MODIFY_VIGNETTING : 0);
    lfModifier modifier(lens, float(crop), 640, 480);
    data->flags = modifier.Initialize(lens, LF_PF_U16, float(frame.focal), float(data->aperture),
                                     1000, 1, lens->Type, requested, false);
    result.distortion = data->flags & LF_MODIFY_DISTORTION;
    result.vignetting = data->flags & LF_MODIFY_VIGNETTING;
    if (!result.distortion && !result.vignetting) { result.status = "rawLensNoCalibration"; return result; }
    result.profile = name(lens->Model); result.status = "rawLensMatched"; result.data = data;
    return result;
}
// DNG 1.6 chapter 7: OpcodeList3 is big-endian regardless of TIFF byte order.
// The decoded working image is already RGB, so per-camera-channel TCA is not applied here.
RawLensCorrection embeddedLens(const RawFrame &frame) {
    RawLensCorrection result; result.status = "rawLensEmbeddedUnsupported";
    if (!frame.embeddedGeometryCompatible || frame.dngOpcodes.size() < 4 || frame.dngOpcodes.size() > 4 * 1024 * 1024) return result;
    const auto &bytes = frame.dngOpcodes;
    qsizetype pos = 0;
    auto u32 = [&]() { const auto value = qFromBigEndian<quint32>(bytes.constData() + pos); pos += 4; return value; };
    auto f64 = [&]() { const auto value = std::bit_cast<double>(qFromBigEndian<quint64>(bytes.constData() + pos)); pos += 8; return value; };
    const auto count = u32();
    if (count > 128) return result;
    auto data = std::make_shared<RawLensData>();
    bool skipped = false;
    for (quint32 n = 0; n < count; ++n) {
        if (pos + 16 > bytes.size()) return result;
        const auto id = u32(), version = u32(), flags = u32(), size = u32();
        if (size > quint32(bytes.size() - pos)) return result;
        const qsizetype end = pos + size;
        if (id != 1 && id != 2 && id != 3) {
            if (!(flags & 1)) return result;
            skipped = true; pos = end; continue;
        }
        if (version > 0x01060000) return result;
        DngLensOperation op; op.kind = int(id);
        if (id == 3) {
            if (size != 56) return result;
            for (int i = 0; i < 5; ++i) op.k[i] = f64();
        } else {
            if (size < 4) return result;
            const auto planes = u32();
            const int coefficients = id == 1 ? 6 : 4;
            if ((planes != 1 && planes != 3) || size != 4 + planes * coefficients * 8 + 16) return result;
            for (quint32 plane = 0; plane < planes; ++plane)
                for (int i = 0; i < coefficients; ++i) {
                    const double k = f64();
                    if (plane == 0) op.k[i] = k;
                    else if (k != op.k[i]) return result;
                }
        }
        op.cx = f64(); op.cy = f64();
        if (!std::isfinite(op.cx) || !std::isfinite(op.cy) || op.cx < 0 || op.cx > 1 || op.cy < 0 || op.cy > 1) return result;
        for (double k : op.k) if (!std::isfinite(k) || std::abs(k) > 100) return result;
        if (pos != end) return result;
        data->operations.push_back(op);
    }
    if (pos != bytes.size() || data->operations.empty()) return result;
    result.data = data; result.embedded = true; result.profile = "DNG"; result.status = skipped ? "rawLensEmbeddedPartial" : "rawLensEmbedded";
    for (const auto &op : data->operations) {
        result.distortion |= op.kind == 1 || op.kind == 2;
        result.vignetting |= op.kind == 3;
    }
    return result;
}
QRgba64 sample(const QImage &source, double x, double y) {
    if (!std::isfinite(x) || !std::isfinite(y) || x < -.001 || y < -.001 || x > source.width()-1+.001 || y > source.height()-1+.001)
        return QRgba64::fromRgba64(0, 0, 0, 65535);
    x = std::clamp(x, 0., double(source.width()-1)); y = std::clamp(y, 0., double(source.height()-1));
    const int ix = int(x), iy = int(y), jx = std::min(ix+1, source.width()-1), jy = std::min(iy+1, source.height()-1);
    const auto a = pixel64(source, ix, iy), b = pixel64(source, jx, iy), c = pixel64(source, ix, jy), d = pixel64(source, jx, jy);
    const double fx = x - ix, fy = y - iy;
    auto mix = [&](int aa, int bb, int cc, int dd) { return quint16(qRound((aa*(1-fx)+bb*fx)*(1-fy)+(cc*(1-fx)+dd*fx)*fy)); };
    return QRgba64::fromRgba64(mix(a.red(),b.red(),c.red(),d.red()), mix(a.green(),b.green(),c.green(),d.green()),
                              mix(a.blue(),b.blue(),c.blue(),d.blue()), mix(a.alpha(),b.alpha(),c.alpha(),d.alpha()));
}
QImage applyEmbedded(QImage source, const RawLensData &data) {
    for (const auto &op : data.operations) {
        QImage output(source.size(), QImage::Format_RGBA64);
        if (output.isNull()) throw Error("tooLarge");
        auto *bits = output.bits(); const auto stride = output.bytesPerLine();
        const double cx = op.cx*(source.width()-1), cy = op.cy*(source.height()-1);
        const double unit = std::max(1., std::hypot(std::max(cx, source.width()-1-cx), std::max(cy, source.height()-1-cy)));
        pixel_parallel(size_t(source.height()), [&](size_t row) {
            auto *out = reinterpret_cast<QRgba64 *>(bits + row * stride);
            const double dy = (double(row)-cy)/unit;
            for (int x = 0; x < source.width(); ++x) {
                const double dx = (x-cx)/unit, r2 = dx*dx+dy*dy;
                if (op.kind == 3) {
                    const double gain = 1+r2*(op.k[0]+r2*(op.k[1]+r2*(op.k[2]+r2*(op.k[3]+r2*op.k[4]))));
                    const auto p = pixel64(source,x,int(row));
                    auto scale = [&](int v) { return quint16(qRound(std::clamp(v*gain,0.,65535.))); };
                    out[x] = QRgba64::fromRgba64(scale(p.red()),scale(p.green()),scale(p.blue()),p.alpha());
                } else {
                    double factor = op.k[0]+r2*(op.k[1]+r2*(op.k[2]+r2*op.k[3]));
                    if (op.kind == 2) {
                        const double r = std::sqrt(r2), theta = std::atan(r), t2 = theta*theta;
                        factor = r > 1e-12 ? theta/r*(op.k[0]+t2*(op.k[1]+t2*(op.k[2]+t2*op.k[3]))) : op.k[0];
                    }
                    out[x] = sample(source, cx+unit*(dx*factor+2*op.k[4]*dx*dy+op.k[5]*(r2+2*dx*dx)),
                                            cy+unit*(dy*factor+2*op.k[5]*dx*dy+op.k[4]*(r2+2*dy*dy)));
                }
            }
        });
        source = output;
    }
    return source;
}
} // namespace
QImage orientRawImage(const QImage &image, int orientation, bool inverse) {
    const int flip = orientation & 7;
    if (!flip) return image;
    const auto source = image.convertToFormat(QImage::Format_RGBA64);
    QSize size = source.size(); if (flip & 4) size.transpose();
    QImage output(size, source.format()); if (output.isNull()) throw Error("tooLarge");
    const int width = inverse ? output.width() : source.width(), height = inverse ? output.height() : source.height();
    for (int y = 0; y < (inverse ? source.height() : output.height()); ++y)
        for (int x = 0; x < (inverse ? source.width() : output.width()); ++x) {
            int sx = x, sy = y; if (flip & 4) std::swap(sx,sy);
            if (flip & 1) sx = width-1-sx;
            if (flip & 2) sy = height-1-sy;
            if (inverse) reinterpret_cast<QRgba64 *>(output.scanLine(sy))[sx] = pixel64(source,x,y);
            else reinterpret_cast<QRgba64 *>(output.scanLine(y))[x] = pixel64(source,sx,sy);
        }
    return output;
}
RawLensCorrection automaticRawLens(const RawFrame &frame, const QByteArray &database) {
    RawLensCorrection missing;
    if (!frame.dngOpcodes.isEmpty()) {
        auto embedded = embeddedLens(frame);
        if (embedded.available()) return embedded;
        missing = embedded;
    }
    try {
        const auto db = database.isEmpty() ? bundledDatabase() : loadDatabase(database);
        const auto camera = cameraFor(*db,frame);
        if (!camera || frame.focal <= 0) { if (frame.dngOpcodes.isEmpty()) missing.status = "rawLensMissingMetadata"; return missing; }
        static QMutex searchMutex;
        QMutexLocker lock(&searchMutex);
        const auto list = db->FindLenses(camera, nullptr, nullptr, 0);
        const lfLens *best = nullptr; bool ambiguous = false; double cropDifference = 1e9;
        const auto lensBrand = maker(frame.lensMake);
        for (int i = 0; list && list[i]; ++i) {
            const auto candidate = list[i]; const auto brand = maker(name(candidate->Maker));
            if (!lensBrand.isEmpty() && lensBrand != brand) continue;
            if (!frame.lens.isEmpty()) {
                if (!matchesModel(candidate->Model, frame.lens, brand)) continue;
            } else {
                // Missing EXIF lens names are safe only for camera-specific fixed-lens mounts.
                if (!normalized(name(camera->Mount)).contains(modelName(frame.model,maker(frame.make)))) continue;
            }
            if (candidate->MinFocal > frame.focal*1.02 || candidate->MaxFocal < frame.focal*.98) continue;
            const double difference = std::abs(camera->CropFactor-candidate->CropFactor);
            if (difference + .001 < cropDifference) { best=candidate; ambiguous=false; cropDifference=difference; }
            else if (std::abs(difference-cropDifference) <= .001) ambiguous=true;
        }
        lf_free(list);
        if (ambiguous) { missing.status="rawLensAmbiguous"; return missing; }
        if (!best) return missing;
        return fromLens(db,best,frame,camera->CropFactor);
    } catch (const Error &) { missing.status="rawLensDatabaseError"; return missing; }
}
RawLensCorrection manualRawLens(const RawFrame &frame, const QByteArray &xml) {
    if (xml.size() > 4*1024*1024) throw Error("tooLarge");
    const auto db = loadDatabase(xml); const auto list = db->GetLenses();
    if (!list || !list[0] || list[1]) throw Error("rawLensProfileError");
    const auto camera = cameraFor(*bundledDatabase(),frame);
    auto result = fromLens(db,list[0],frame,camera ? camera->CropFactor : list[0]->CropFactor);
    if (!result.available()) throw Error("rawLensProfileError");
    result.status = "rawLensManual";
    return result;
}
QImage applyRawLens(const RawFrame &frame, const RawLensCorrection &correction, int previewLimit) {
    auto image = frame.linear;
    if (previewLimit > 0 && (image.width()>previewLimit || image.height()>previewLimit))
        image = image.scaled(previewLimit,previewLimit,Qt::KeepAspectRatio,Qt::SmoothTransformation);
    if (!correction.available() || image.isNull()) return image;
    image = orientRawImage(image,frame.orientation,true).convertToFormat(QImage::Format_RGBA64);
    const auto &data = *correction.data;
    if (correction.embedded) {
        image = applyEmbedded(image,data);
        if (!frame.dngDefaultCrop.isEmpty()) {
            QSize original = frame.linear.size(); if (frame.orientation & 4) original.transpose();
            const auto crop = frame.dngDefaultCrop;
            const double sx = double(image.width())/original.width(), sy = double(image.height())/original.height();
            const int left = qRound(crop.x()*sx), top = qRound(crop.y()*sy);
            const QRect scaled(left,top,std::max(1,qRound((crop.x()+crop.width())*sx)-left),
                               std::max(1,qRound((crop.y()+crop.height())*sy)-top));
            image = image.copy(scaled.intersected(image.rect()));
        }
    }
    else {
        lfModifier modifier(data.lens,float(data.crop),image.width(),image.height());
        const int flags = modifier.Initialize(data.lens,LF_PF_U16,float(data.focal),float(data.aperture),1000,0,
                                             data.lens->Type,data.flags|LF_MODIFY_SCALE,false);
        if (flags & LF_MODIFY_VIGNETTING) {
            auto *bits = image.bits(); const auto stride = image.bytesPerLine();
            pixel_parallel(size_t(image.height()),[&](size_t y) {
                modifier.ApplyColorModification(bits+y*stride,0,float(y),image.width(),1,LF_CR_4(RED,GREEN,BLUE,UNKNOWN),int(stride));
            });
        }
        if (flags & (LF_MODIFY_DISTORTION|LF_MODIFY_SCALE)) {
            QImage result(image.size(),image.format()); if (result.isNull()) throw Error("tooLarge");
            auto *bits = result.bits(); const auto stride = result.bytesPerLine();
            pixel_parallel(size_t(image.height()),[&](size_t y) {
                // MSVC's default allocator aligns allocations to at least 16 bytes on x64.
                std::vector<float> coordinates(size_t(image.width())*2);
                if (!modifier.ApplyGeometryDistortion(0,float(y),image.width(),1,coordinates.data())) {
                    memcpy(bits+y*stride,image.constScanLine(int(y)),size_t(stride)); return;
                }
                auto *out = reinterpret_cast<QRgba64 *>(bits+y*stride);
                for (int x=0;x<image.width();++x) out[x]=sample(image,coordinates[2*x],coordinates[2*x+1]);
            });
            image=result;
        }
    }
    return orientRawImage(image,frame.orientation);
}
} // namespace comp
