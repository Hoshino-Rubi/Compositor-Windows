#include "Pixels.h"
#include "Project.h"
#include "Importers.h"
#include "Filters.h"
#include "Effects.h"
#include <QBuffer>
#include <QColorSpace>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <QtEndian>
#include <cmath>
#include <functional>
#include <algorithm>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <Windows.h>
#endif

namespace comp {
qint64 documentCapacity(qint64 memoryDivisor, qint64 minimum, qint64 maximum) {
    static const quint64 bytes = [] {
#ifdef Q_OS_WIN
        MEMORYSTATUSEX status{};
        status.dwLength = sizeof(status);
        if (GlobalMemoryStatusEx(&status)) return quint64(status.ullTotalPhys);
#endif
        return quint64(8) * 1024 * 1024 * 1024;
    }();
    return std::clamp(qint64(bytes / quint64(memoryDivisor)), minimum, maximum);
}
Error::Error(QString key, QString detail)
    : std::runtime_error(key.toStdString()), key(std::move(key)), detail(std::move(detail)) {}
QString newId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper();
}
static bool validId(const QString &id) {
    return id.size() == 36 && !QUuid(id).isNull();
}
static bool finite(double v, double min, double max) {
    return std::isfinite(v) && v >= min && v <= max;
}
static void require(bool condition, const QString &key = "invalidProject") {
    if (!condition)
        throw Error(key);
}
static QString canonicalId(const QJsonValue &v) {
    require(v.isString() && validId(v.toString()));
    return v.toString().toUpper();
}
QTransform Placement::matrix(QSize source) const {
    QTransform t;
    t.translate(origin.x() + size.width() / 2, origin.y() + size.height() / 2);
    t.rotate(rotation);
    t.scale((flipX ? -1 : 1) * size.width() / qMax(1, source.width()),
            (flipY ? -1 : 1) * size.height() / qMax(1, source.height()));
    t.translate(-source.width() / 2.0, -source.height() / 2.0);
    return t;
}
QJsonObject Placement::json() const {
    return {{"origin", QJsonArray{origin.x(), origin.y()}},
            {"size", QJsonArray{size.width(), size.height()}},
            {"rotation", rotation},
            {"flipX", flipX},
            {"flipY", flipY},
            {"sampling", sampling}};
}
Placement Placement::fromJson(const QJsonObject &object) {
    const auto o = object["origin"].toArray(), s = object["size"].toArray();
    require(o.size() == 2 && s.size() == 2 && o[0].isDouble() && o[1].isDouble() && s[0].isDouble() && s[1].isDouble());
    require(object["rotation"].isDouble() && object["flipX"].isBool() && object["flipY"].isBool());
    Placement p;
    p.origin = {o[0].toDouble(), o[1].toDouble()};
    p.size = {s[0].toDouble(), s[1].toDouble()};
    p.rotation = object["rotation"].toDouble();
    p.flipX = object["flipX"].toBool();
    p.flipY = object["flipY"].toBool();
    p.sampling = object["sampling"].toString();
    require(finite(p.origin.x(), -1e6, 1e6) && finite(p.origin.y(), -1e6, 1e6) && finite(p.size.width(), 1, 300000) &&
            finite(p.size.height(), 1, 300000) && finite(p.rotation, -1e9, 1e9) &&
            QStringList{"Nearest", "Smooth", "High quality"}.contains(p.sampling));
    return p;
}
int State::index(const QString &key) const {
    for (int i = 0; i < layers.size(); ++i)
        if (layers[i].id == key)
            return i;
    return -1;
}
QVector<int> State::descendants(const QString &key) const {
    QVector<int> result;
    std::function<void(const QString &)> walk = [&](const QString &parent) {
        for (int i = 0; i < layers.size(); ++i)
            if (layers[i].parent == parent) {
                result.append(i);
                walk(layers[i].id);
            }
    };
    walk(key);
    return result;
}
QVector<int> State::drawOrder() const {
    QVector<int> result;
    std::function<void(const QString &)> walk = [&](const QString &parent) {
        for (int i = 0; i < layers.size(); ++i)
            if (layers[i].parent == parent) {
                result.append(i);
                if (layers[i].group)
                    walk(layers[i].id);
            }
    };
    walk({});
    return result;
}
qint64 State::sourcePixels() const {
    qint64 n = 0;
    for (const auto &l : layers)
        n += qint64(l.image.width()) * l.image.height();
    return n;
}
QStringList blendModes() {
    return {"Normal",     "Darken",     "Multiply",    "Color Burn",         "Linear Burn",
            "Lighten",    "Screen",     "Color Dodge", "Linear Dodge (Add)", "Overlay",
            "Soft Light", "Hard Light", "Vivid Light", "Linear Light",       "Pin Light",
            "Hard Mix",   "Difference", "Exclusion",   "Subtract",           "Divide",
            "Hue",        "Saturation", "Color",       "Luminosity"};
}
void validateState(const State &s) {
    require(validId(s.id));
    require(s.bitDepth == 8 || s.bitDepth == 16);
    require(s.size.width() > 0 && s.size.height() > 0 && s.size.width() <= MaxSide && s.size.height() <= MaxSide &&
                qint64(s.size.width()) * s.size.height() <= MaxCanvasPixels && s.layers.size() <= MaxLayers,
            "tooLarge");
    require(finite(s.resolution, 1, 9600));
    QSet<QString> ids;
    qint64 pixels = 0, maskPixels = 0;
    for (const auto &l : s.layers) {
        require(validId(l.id) && !ids.contains(l.id) && !l.name.trimmed().isEmpty() && l.name.toUtf8().size() <= 16384);
        ids.insert(l.id);
        Placement::fromJson(l.placement.json());
        require(finite(l.opacity, 0, 1) && blendModes().contains(l.blend));
        require(!l.group || (l.image.isNull() && l.blend == "Normal"));
        require(!l.metadata.contains("text") || (!l.image.isNull() && !l.group));
        if (!l.mask.isNull())
            Placement::fromJson(l.maskPlacement.json());
        if (!l.metadata.value("adjustment").isNull() && !l.metadata.value("adjustment").isUndefined()) {
            require(!l.group);
            validateAdjustment(l.metadata["adjustment"].toObject());
        }
        if (l.metadata["shape"].toObject()["smoothing"].toBool())
            throw Error("unsupportedFeature", "Smooth rectangle corners");
        const auto effects = l.metadata["effects"].toObject();
        validateEffects(effects);

        for (const auto &image : {l.image, l.mask})
            require(image.width() <= 30000 && image.height() <= 30000, "tooLarge");
        pixels += qint64(l.image.width()) * l.image.height();
        maskPixels += qint64(l.mask.width()) * l.mask.height();
    }
    require(pixels <= MaxSourcePixels && maskPixels <= MaxSourcePixels, "tooLarge");
    require(s.active.isEmpty() || ids.contains(s.active));
    for (const auto &l : s.layers) {
        QSet<QString> seen{l.id};
        QString parent = l.parent;
        while (!parent.isEmpty()) {
            const int i = s.index(parent);
            require(i >= 0 && s.layers[i].group && !seen.contains(parent) && seen.size() <= 64);
            seen.insert(parent);
            parent = s.layers[i].parent;
        }
        seen = {l.id};
        QString source = l.maskSource;
        require(!l.group || source.isEmpty());
        while (!source.isEmpty()) {
            const int i = s.index(source);
            require(i >= 0 && !s.layers[i].group && !seen.contains(source) && seen.size() <= 256);
            seen.insert(source);
            source = s.layers[i].maskSource;
        }
    }
    require(s.guides.size() <= 1000);
    QSet<QString> guideIds;
    for (const auto &v : s.guides) {
        require(v.isObject());
        const auto g = v.toObject();
        const QString id = canonicalId(g["id"]);
        require(!guideIds.contains(id) && (g["axis"] == "horizontal" || g["axis"] == "vertical") &&
                g["position"].isDouble() && finite(g["position"].toDouble(), -1e6, 1e6));
        guideIds.insert(id);
    }
}
static QByteArray safeRead(const QString &root, const QString &relative, qint64 limit) {
    const QFileInfo f(QDir(root).filePath(relative));
    const QString canonical = f.canonicalFilePath();
    require(f.exists() && f.isFile() && !f.isSymLink() &&
                canonical.startsWith(QDir(root).canonicalPath() + "/", Qt::CaseInsensitive),
            "missingAsset");
    require(f.size() <= limit, "tooLarge");
    QFile file(f.absoluteFilePath());
    require(file.open(QIODevice::ReadOnly), "fileError");
    return file.readAll();
}
static QImage decodePng(const QByteArray &bytes, qint64 &budget, bool mask) {
    require(bytes.size() >= 33 && bytes.left(8) == QByteArray::fromHex("89504e470d0a1a0a") &&
                bytes.mid(12, 4) == "IHDR" && (quint8(bytes[24]) <= 8 || quint8(bytes[24]) == 16),
            "missingAsset");
    if (mask)
        require(quint8(bytes[25]) == 0, "missingAsset");
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "png");
    const QSize size = reader.size();
    require(size.isValid() && size.width() <= 30000 && size.height() <= 30000, "missingAsset");
    budget += qint64(size.width()) * size.height();
    require(budget <= MaxSourcePixels, "tooLarge");
    QImage image = reader.read();
    require(!image.isNull(), "missingAsset");
    if (!mask && image.colorSpace().isValid() && image.colorSpace() != QColorSpace::SRgb)
        image.convertToColorSpace(QColorSpace::SRgb);
    return mask ? maskImage(image, wideImage(image) ? 16 : 8) : image.convertToFormat(pixelFormat(image));
}
LoadedProject readProject(const QString &directory) {
    const auto bytes = safeRead(directory, "manifest.json", 4 * 1024 * 1024);
    QJsonParseError error;
    const auto json = QJsonDocument::fromJson(bytes, &error);
    require(error.error == QJsonParseError::NoError && json.isObject());
    const auto m = json.object();
    require(m["format"] == "com.compositor.project" && m["colorSpace"] == "sRGB");
    const double v = m["version"].toDouble(-1);
    require(v == std::floor(v));
    if (v < 1 || v > ProjectManifest::readable)
        throw Error("unsupportedVersion", QString::number(v));
    require(m["layers"].isArray() && m["width"].isDouble() && m["height"].isDouble());
    State s;
    s.id = canonicalId(m["documentID"]);
    s.size = {m["width"].toInt(), m["height"].toInt()};
    require(m["width"].toDouble() == s.size.width() && m["height"].toDouble() == s.size.height());
    s.resolution = m["resolution"].toDouble(72);
    s.bitDepth = v >= 12 ? m["bitDepth"].toInt(8) : 8;
    require(s.bitDepth == 8 || s.bitDepth == 16);
    require(v >= 12 || !m.contains("bitDepth") || m["bitDepth"] == 8);
    s.metadata = m;
    if (!m["activeLayerID"].isNull() && !m["activeLayerID"].isUndefined())
        s.active = canonicalId(m["activeLayerID"]);
    if (m.contains("guides")) {
        require(m["guides"].isArray());
        s.guides = m["guides"].toArray();
    }
    if (v < 8)
        require(s.guides.isEmpty());
    require(s.size.width() > 0 && s.size.height() > 0 && s.size.width() <= MaxSide && s.size.height() <= MaxSide &&
                qint64(s.size.width()) * s.size.height() <= MaxCanvasPixels &&
                m["layers"].toArray().size() <= MaxLayers,
            "tooLarge");
    qint64 pixels = 0, maskPixels = 0;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(bytes);
    for (const auto &entry : m["layers"].toArray()) {
        require(entry.isObject());
        const auto j = entry.toObject();
        Layer l;
        l.metadata = j;
        if (j["shape"].toObject()["smoothing"].toBool())
            throw Error("unsupportedFeature", "Smooth rectangle corners");
        l.id = canonicalId(j["id"]);
        l.name = j["name"].toString();
        require(j["isVisible"].isBool());
        l.visible = j["isVisible"].toBool();
        l.group = j["isGroup"].toBool(false);
        l.placement = Placement::fromJson(j["transform"].toObject());
        if (j.contains("opacity"))
            require(j["opacity"].isDouble());
        l.opacity = j["opacity"].toDouble(1);
        l.blend = j["blendMode"].toString("Normal");
        if (!j["parentID"].isNull() && !j["parentID"].isUndefined())
            l.parent = canonicalId(j["parentID"]);
        if (!j["maskSourceID"].isNull() && !j["maskSourceID"].isUndefined())
            l.maskSource = canonicalId(j["maskSourceID"]);
        if (!j["adjustment"].isNull() && !j["adjustment"].isUndefined() &&
            !supportedAdjustment(j["adjustment"].toObject()))
            throw Error("unsupportedFeature", "adjustments");
        for (const auto &field : QStringList{"isGroup", "maskEnabled", "maskLinked"})
            if (j.contains(field))
                require(j[field].isBool());
        if (j.contains("effects"))
            validateEffects(j["effects"].toObject());
        if (j.contains("imageFile") && !j["imageFile"].isNull()) {
            if (j["imageFile"].toString().endsWith(".exr", Qt::CaseInsensitive))
                throw Error("unsupportedFeature", "OpenEXR/HDR layers");
            require(j["imageFile"].toString() == l.id + ".png");
            l.originalPng =
                safeRead(directory, "images/" + l.id + ".png", (v >= 12 ? 1024ll : 512ll) * 1024 * 1024);
            hash.addData(l.originalPng);
            l.image = decodePng(l.originalPng, pixels, false);
        }
        l.maskPlacement = l.placement;
        if (j.contains("maskFile") && !j["maskFile"].isNull()) {
            require(j["maskFile"].toString() == l.id + ".mask.png" && v >= (l.group ? 6 : 4));
            l.originalMaskPng =
                safeRead(directory, "images/" + l.id + ".mask.png", (v >= 12 ? 1024ll : 512ll) * 1024 * 1024);
            hash.addData(l.originalMaskPng);
            l.mask = decodePng(l.originalMaskPng, maskPixels, true);
            l.maskEnabled = j["maskEnabled"].toBool(true);
            l.maskLinked = j["maskLinked"].toBool(true);
            if (j.contains("maskPlacement"))
                l.maskPlacement = Placement::fromJson(j["maskPlacement"].toObject());
        } else
            require(!j.contains("maskEnabled") && !j.contains("maskPlacement") && !j.contains("maskLinked"));
        if (v < 2)
            require(!l.group && l.parent.isEmpty());
        if (v < 3)
            require(l.opacity == 1 && l.blend == "Normal");
        if (v < 5)
            require(l.maskSource.isEmpty());
        if (v < 8 && l.group)
            require(l.opacity == 1);
        s.layers.append(l);
    }
    for (const auto &l : s.layers) {
        const bool highDepth = wideImage(l.image) || wideImage(l.mask);
        require(v >= 12 || !highDepth);
        if (highDepth) s.bitDepth = 16;
    }
    normalizePrecision(s);
    validateState(s);
    return {s, hash.result()};
}
QByteArray projectDigest(const QString &directory) {
    return readProject(directory).digest;
}
QByteArray pngBytes(const QImage &image, double resolution) {
    require(!image.isNull(), "missingAsset");
    QImage copy = image;
    copy.setDotsPerMeterX(qRound(resolution / 0.0254));
    copy.setDotsPerMeterY(qRound(resolution / 0.0254));
    QByteArray data;
    QBuffer buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, "png");
    require(writer.write(copy), "fileError");
    return data;
}
QImage readImage(const QString &path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == "svg" || suffix == "svgz")
        return readSvg(path);
    if (suffix == "heic" || suffix == "heif" || suffix == "hif")
        return readHeif(path);
    if (isRawPath(path))
        return developRaw(decodeRaw(path));
    QFileInfo info(path);
    require(info.size() <= 1024ll * 1024 * 1024, "tooLarge");
    QImageReader reader(path);
    QBuffer tinyWebp;
    // Qt 6.8's WebP handler peeks sizeof(WebPBitstreamFeatures) (40 bytes),
    // although valid lossless images can be shorter. A legal ignored RIFF
    // chunk supplies that lookahead without changing the image bitstream.
    if (info.size() >= 20 && info.size() < 40 && reader.format() == "webp") {
        QFile file(path);
        require(file.open(QIODevice::ReadOnly), "fileError");
        auto bytes = file.readAll();
        if (bytes.startsWith("RIFF") && bytes.mid(8, 4) == "WEBP" &&
            qFromLittleEndian<quint32>(bytes.constData() + 4) == quint32(bytes.size() - 8)) {
            bytes.append("JUNK", 4);
            const quint32 length = qToLittleEndian(quint32(16));
            bytes.append(reinterpret_cast<const char *>(&length), 4);
            bytes.append(QByteArray(16, 0));
            qToLittleEndian(quint32(bytes.size() - 8), bytes.data() + 4);
            tinyWebp.setData(bytes);
            tinyWebp.open(QIODevice::ReadOnly);
            reader.setDevice(&tinyWebp);
            reader.setFormat("webp");
        }
    }
    reader.setAutoTransform(true);
    if (!QList<QByteArray>{"png", "jpeg", "bmp", "webp", "tiff"}.contains(reader.format().toLower()))
        throw Error("unsupportedImport");
    const QSize size = reader.size();
    require(size.isValid(), "imageError");
    require(size.width() <= 30000 && size.height() <= 30000 && qint64(size.width()) * size.height() <= MaxSourcePixels,
            "tooLarge");
    QImage image = reader.read();
    require(!image.isNull(), "imageError");
    require(image.pixelFormat().redSize() <= 16 &&
                image.pixelFormat().typeInterpretation() != QPixelFormat::FloatingPoint,
            "unsupportedImportDepth");
    if (image.colorSpace().isValid() && image.colorSpace() != QColorSpace::SRgb)
        image.convertToColorSpace(QColorSpace::SRgb);
    image.setColorSpace(QColorSpace::SRgb);
    return image.convertToFormat(pixelFormat(image));
}
QByteArray writeProject(const QString &directory, const State &s, const QByteArray &expectedDigest) {
    validateState(s);
    const QFileInfo target(directory);
    require(target.suffix().compare("comp", Qt::CaseInsensitive) == 0 && !target.isSymLink(), "invalidProject");
    const bool existing = target.exists();
    if (existing) {
        require(target.isDir());
        const auto current = projectDigest(directory);
        if (!expectedDigest.isEmpty() && current != expectedDigest)
            throw Error("externalChange");
    } else if (!expectedDigest.isEmpty())
        throw Error("externalChange");
    const QString stage = target.absoluteFilePath() + ".staging-" + newId();
    const QString backup = target.absoluteFilePath() + ".backup-" + newId();
    require(QDir().mkpath(stage + "/images"), "fileError");
    QJsonObject manifest = s.metadata;
    manifest["format"] = "com.compositor.project";
    manifest["version"] = s.bitDepth == 16 ? ProjectManifest::current : ProjectManifest::eightBit;
    if (s.bitDepth == 16)
        manifest["bitDepth"] = 16;
    else
        manifest.remove("bitDepth");
    manifest["colorSpace"] = "sRGB";
    manifest["documentID"] = s.id;
    manifest["width"] = s.size.width();
    manifest["height"] = s.size.height();
    manifest["resolution"] = s.resolution;
    manifest["activeLayerID"] = s.active.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(s.active);
    if (s.guides.isEmpty())
        manifest.remove("guides");
    else
        manifest["guides"] = s.guides;
    QJsonArray records;
    QVector<QByteArray> encoded;
    bool moved = false, installed = false;
    auto write = [&](const QString &name, const QByteArray &bytes) {
        QSaveFile file(stage + "/" + name);
        require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit(), "fileError");
    };
    try {
        for (const auto &l : s.layers) {
            auto j = l.metadata;
            for (const auto &field : QStringList{"imageFile", "maskFile", "maskEnabled", "maskPlacement", "maskLinked",
                                                 "maskSourceID", "parentID"})
                j.remove(field);
            j["id"] = l.id;
            j["name"] = l.name;
            j["isVisible"] = l.visible;
            j["isGroup"] = l.group;
            j["opacity"] = l.opacity;
            j["blendMode"] = l.blend;
            j["transform"] = l.placement.json();
            if (!l.parent.isEmpty())
                j["parentID"] = l.parent;
            if (!l.maskSource.isEmpty())
                j["maskSourceID"] = l.maskSource;
            if (!l.image.isNull()) {
                const auto bytes = l.originalPng.isEmpty() ? pngBytes(l.image, s.resolution) : l.originalPng;
                j["imageFile"] = l.id + ".png";
                write("images/" + l.id + ".png", bytes);
                encoded.append(bytes);
            }
            if (!l.mask.isNull()) {
                const auto bytes = l.originalMaskPng.isEmpty() ? pngBytes(maskImage(l.mask, s.bitDepth), s.resolution)
                                                               : l.originalMaskPng;
                j["maskFile"] = l.id + ".mask.png";
                j["maskEnabled"] = l.maskEnabled;
                j["maskLinked"] = l.maskLinked;
                if (!l.maskLinked)
                    j["maskPlacement"] = l.maskPlacement.json();
                write("images/" + l.id + ".mask.png", bytes);
                encoded.append(bytes);
            }
            records.append(j);
        }
        manifest["layers"] = records;
        const auto json = QJsonDocument(manifest).toJson();
        require(json.size() <= 4 * 1024 * 1024, "tooLarge");
        write("manifest.json", json);
        if (existing) {
            if (!expectedDigest.isEmpty() && projectDigest(directory) != expectedDigest)
                throw Error("externalChange");
            require(QDir().rename(target.absoluteFilePath(), backup), "fileError");
            moved = true;
        }
        require(QDir().rename(stage, target.absoluteFilePath()), "fileError");
        installed = true;
        if (moved)
            QDir(backup).removeRecursively();
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(json);
        for (const auto &bytes : encoded)
            hash.addData(bytes);
        return hash.result();
    } catch (...) {
        if (moved && !installed)
            QDir().rename(backup, target.absoluteFilePath());
        QDir(stage).removeRecursively();
        throw;
    }
}
} // namespace comp
