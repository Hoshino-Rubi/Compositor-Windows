#include "LayerClipboard.h"
#include "Pixels.h"
#include <QBuffer>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
namespace comp {
namespace {
QSet<QString> selectedSet(const State &state, const QStringList &selected) {
    QSet<QString> chosen;
    for (const auto &id : selected) {
        if (state.index(id) < 0) continue;
        chosen.insert(id);
        for (int i : state.descendants(id)) chosen.insert(state.layers[i].id);
    }
    return chosen;
}
QByteArray decodeBytes(const QJsonValue &value) {
    if (!value.isString()) throw Error("invalidProject");
    const auto encoded = value.toString().toLatin1();
    if (encoded.size() > 700 * 1024 * 1024) throw Error("tooLarge");
    const auto bytes = QByteArray::fromBase64(encoded, QByteArray::AbortOnBase64DecodingErrors);
    if (bytes.isEmpty() || bytes.toBase64() != encoded) throw Error("invalidProject");
    return bytes;
}
QImage decodeImage(const QJsonObject &record, const char *field, QByteArray &original, qint64 &budget) {
    if (!record.contains(field)) return {};
    original = decodeBytes(record.value(field));
    if (QString::fromLatin1(field)=="image" && record["imageEncoding"]=="exr") {
        auto image=readExr(original);budget+=qint64(image.width())*image.height();if(budget>MaxSourcePixels)throw Error("tooLarge");return image;
    }
    QBuffer buffer(&original);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "png");
    const QSize size = reader.size();
    if (!size.isValid() || size.width() > 30000 || size.height() > 30000 ||
        (budget += qint64(size.width()) * size.height()) > MaxSourcePixels)
        throw Error("tooLarge");
    auto image = reader.read();
    if (image.isNull()) throw Error("imageError");
    return image.convertToFormat(pixelFormat(image));
}
QJsonObject header(const QByteArray &data) {
    if (data.isEmpty() || data.size() > 950 * 1024 * 1024) throw Error("tooLarge");
    const auto json = QJsonDocument::fromJson(data);
    if (!json.isObject() || json.object().value("format") != "com.compositor.layers" ||
        (json.object().value("version") != 1 && json.object().value("version") != 2)) throw Error("invalidProject");
    return json.object();
}
} // namespace
QByteArray encodeLayers(const State &source, const QStringList &selected) {
    const auto chosen = selectedSet(source, selected);
    if (chosen.isEmpty()) return {};
    QJsonArray entries;
    for (const auto &layer : source.layers) {
        if (!chosen.contains(layer.id)) continue;
        QJsonObject record{{"id", layer.id}, {"name", layer.name}, {"parent", layer.parent},
                           {"visible", layer.visible}, {"group", layer.group}, {"opacity", layer.opacity},
                           {"blend", layer.blend}, {"placement", layer.placement.json()},
                           {"maskEnabled", layer.maskEnabled}, {"maskLinked", layer.maskLinked},
                           {"maskPlacement", layer.maskPlacement.json()}, {"maskSource", layer.maskSource},
                           {"metadata", layer.metadata}};
        if (!layer.image.isNull()) {
            const bool hdr=hdrImage(layer.image);
            if(hdr)record["imageEncoding"]="exr";
            record["image"]=QString::fromLatin1((layer.originalPng.isEmpty()?(hdr?exrBytes(layer.image):pngBytes(layer.image,source.resolution)):layer.originalPng).toBase64());
        }
        if (!layer.mask.isNull())
            record["mask"] = QString::fromLatin1(layer.originalMaskPng.isEmpty() ? pngBytes(maskImage(layer.mask, source.bitDepth), source.resolution).toBase64()
                                                                               : layer.originalMaskPng.toBase64());
        entries.append(record);
    }
    return QJsonDocument(QJsonObject{{"format", "com.compositor.layers"}, {"version", hasHdr(source)?2:1},
                                     {"document", source.id}, {"bitDepth", source.bitDepth},
                                     {"layers", entries}}).toJson(QJsonDocument::Compact);
}
QString layerClipboardDocument(const QByteArray &data) {
    try { return header(data).value("document").toString(); }
    catch (const Error &) { return {}; }
}
bool pasteLayers(Document &target, const QByteArray &data, const QString &parent, const QString &before, bool offset) {
    if (!target.hasDocument() || data.isEmpty()) return false;
    const auto object = header(data);
    const auto entries = object.value("layers").toArray();
    if (entries.isEmpty() || entries.size() > MaxLayers ||
        (object.value("bitDepth") != 8 && object.value("bitDepth") != 16))
        throw Error("invalidProject");
    QVector<Layer> copies;
    QHash<QString, QString> ids;
    qint64 pixels = 0, masks = 0;
    for (const auto &entry : entries) {
        if (!entry.isObject()) throw Error("invalidProject");
        const auto record = entry.toObject();
        Layer layer;
        const QString oldId = record.value("id").toString();
        if (oldId.isEmpty() || ids.contains(oldId)) throw Error("invalidProject");
        ids.insert(oldId, newId());
        layer.id = ids.value(oldId);
        layer.name = record.value("name").toString();
        layer.parent = record.value("parent").toString();
        layer.maskSource = record.value("maskSource").toString();
        layer.visible = record.value("visible").toBool(true);
        layer.group = record.value("group").toBool();
        layer.opacity = record.value("opacity").toDouble(1);
        layer.blend = record.value("blend").toString("Normal");
        layer.placement = Placement::fromJson(record.value("placement").toObject());
        layer.maskPlacement = Placement::fromJson(record.value("maskPlacement").toObject());
        layer.maskEnabled = record.value("maskEnabled").toBool(true);
        layer.maskLinked = record.value("maskLinked").toBool(true);
        layer.metadata = record.value("metadata").toObject();
        layer.image = decodeImage(record, "image", layer.originalPng, pixels);
        layer.mask = decodeImage(record, "mask", layer.originalMaskPng, masks);
        if (!layer.mask.isNull()) layer.mask = maskImage(layer.mask, object.value("bitDepth").toInt());
        copies.append(layer);
    }
    const auto &state = target.state();
    if ((!parent.isEmpty() && (state.index(parent) < 0 || !state.layers[state.index(parent)].group)) ||
        (!before.isEmpty() && (state.index(before) < 0 || state.layers[state.index(before)].parent != parent)))
        throw Error("invalidProject");
    target.edit([&](State &s) {
        if (object.value("bitDepth") == 16) s.bitDepth = 16;
        const int insertion = before.isEmpty() ? s.layers.size() : s.index(before);
        for (auto &layer : copies) {
            const bool root = !ids.contains(layer.parent);
            layer.parent = root ? parent : ids.value(layer.parent);
            layer.maskSource = ids.value(layer.maskSource);
            if (offset) {
                layer.placement.origin += QPointF(12, 12);
                if (!layer.maskLinked) layer.maskPlacement.origin += QPointF(12, 12);
            }
        }
        for (int i = 0; i < copies.size(); ++i) s.layers.insert(insertion + i, copies[i]);
        s.active = copies.last().id;
    });
    return true;
}
bool moveLayers(Document &document, const QStringList &selected, const QString &parent, const QString &before) {
    const auto &state = document.state();
    const auto chosen = selectedSet(state, selected);
    if (chosen.isEmpty() || chosen.contains(parent) || chosen.contains(before)) return false;
    if ((!parent.isEmpty() && (state.index(parent) < 0 || !state.layers[state.index(parent)].group)) ||
        (!before.isEmpty() && (state.index(before) < 0 || state.layers[state.index(before)].parent != parent)))
        return false;
    document.edit([&](State &s) {
        QVector<Layer> moved;
        for (auto layer : s.layers) {
            if (!chosen.contains(layer.id)) continue;
            if (!chosen.contains(layer.parent)) layer.parent = parent;
            moved.append(layer);
        }
        s.layers.removeIf([&](const Layer &layer) { return chosen.contains(layer.id); });
        const int at = before.isEmpty() ? s.layers.size() : s.index(before);
        for (int i = 0; i < moved.size(); ++i) s.layers.insert(at + i, moved[i]);
    });
    return true;
}
bool cutLayers(Document &source, const QStringList &selected) {
    if (!source.hasDocument()) return false;
    const auto chosen = selectedSet(source.state(), selected);
    if (chosen.isEmpty()) return false;
    source.edit([&](State &s) {
        s.layers.removeIf([&](const Layer &layer) { return chosen.contains(layer.id); });
        for (auto &layer : s.layers)
            if (chosen.contains(layer.maskSource)) layer.maskSource.clear();
        s.active = s.layers.isEmpty() ? QString() : s.layers.last().id;
    });
    return true;
}
} // namespace comp
