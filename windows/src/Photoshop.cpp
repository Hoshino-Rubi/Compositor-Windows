#include "Pixels.h"
#include "Cmyk.h"
#include "Importers.h"
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QColorTransform>
#include <QRegularExpression>
#include <QtEndian>
#include <zlib.h>
#include <bit>
#include <optional>
#include <QFontDatabase>
#include <cmath>
namespace comp {
namespace {
struct Reader {
    QByteArray data;
    qsizetype pos = 0;
    void need(quint64 n) const {
        if (n > quint64(data.size()) || pos < 0 || quint64(pos) > quint64(data.size()) - n)
            throw Error("psdCorrupt");
    }
    QByteArray bytes(quint64 n) {
        need(n);
        auto result = data.mid(pos, qsizetype(n));
        pos += qsizetype(n);
        return result;
    }
    void skip(quint64 n) {
        need(n);
        pos += qsizetype(n);
    }
    quint8 u8() {
        need(1);
        return quint8(data[pos++]);
    }
    quint16 u16() {
        need(2);
        auto v = qFromBigEndian<quint16>(data.constData() + pos);
        pos += 2;
        return v;
    }
    qint16 i16() { return qint16(u16()); }
    quint32 u32() {
        need(4);
        auto v = qFromBigEndian<quint32>(data.constData() + pos);
        pos += 4;
        return v;
    }
    qint32 i32() { return qint32(u32()); }
    quint64 u64() {
        need(8);
        auto v = qFromBigEndian<quint64>(data.constData() + pos);
        pos += 8;
        return v;
    }
    double real() { return std::bit_cast<double>(u64()); }
    QString unicode() {
        quint32 n = u32();
        if (n > 1000000)
            throw Error("psdCorrupt");
        QString s;
        s.reserve(n);
        for (quint32 i = 0; i < n; ++i)
            s.append(QChar(u16()));
        s.remove(QChar(0));
        return s;
    }
    QString key() {
        auto n = u32();
        return QString::fromLatin1(bytes(n ? n : 4));
    }
    Reader section(quint64 n) { return {bytes(n)}; }
};
QJsonValue descriptorValue(Reader &r, QByteArray type, int depth);
QJsonObject descriptor(Reader &r, int depth = 0) {
    if (depth > 24)
        throw Error("psdCorrupt");
    r.unicode();
    r.key();
    auto count = r.u32();
    if (count > 100000)
        throw Error("psdCorrupt");
    QJsonObject o;
    for (quint32 i = 0; i < count; ++i) {
        auto key = r.key();
        auto type = r.bytes(4);
        o[key] = descriptorValue(r, type, depth + 1);
    }
    return o;
}
QJsonValue descriptorValue(Reader &r, QByteArray type, int depth) {
    if (depth > 24)
        throw Error("psdCorrupt");
    if (type == "Objc" || type == "GlbO")
        return descriptor(r, depth);
    if (type == "TEXT")
        return r.unicode();
    if (type == "doub")
        return r.real();
    if (type == "UntF") {
        r.skip(4);
        return r.real();
    }
    if (type == "long")
        return r.i32();
    if (type == "bool")
        return r.u8() != 0;
    if (type == "enum") {
        r.key();
        return r.key();
    }
    if (type == "type" || type == "GlbC") {
        r.unicode();
        return r.key();
    }
    if (type == "tdta" || type == "alis")
        return QString::fromLatin1(r.bytes(r.u32()).toBase64());
    if (type == "VlLs") {
        auto n = r.u32();
        if (n > 100000)
            throw Error("psdCorrupt");
        QJsonArray list;
        for (quint32 i = 0; i < n; ++i)
            list.append(descriptorValue(r, r.bytes(4), depth + 1));
        return list;
    }
    throw Error("psdConversion", "Unsupported descriptor");
}
void undoPrediction(QByteArray &data, int w, int h, int depth) {
    for (int y = 0; y < h; ++y) {
        char *row = data.data() + qint64(y) * w * (depth / 8);
        if (depth == 16) {
            quint16 previous = qFromBigEndian<quint16>(row);
            for (int x = 1; x < w; ++x) {
                previous = quint16(previous + qFromBigEndian<quint16>(row + x * 2));
                qToBigEndian(previous, row + x * 2);
            }
        } else {
            for (int x = 1; x < w; ++x)
                row[x] = char(quint8(row[x]) + quint8(row[x - 1]));
        }
    }
}
QByteArray plane(Reader &r, int w, int h, bool psb, int depth) {
    if (w <= 0 || h <= 0)
        return {};
    const qint64 pixels = qint64(w) * h, rowBytes = qint64(w) * (depth / 8), bytes = rowBytes * h;
    if (w > 30000 || h > 30000 || pixels > MaxSourcePixels)
        throw Error("tooLarge");
    const int compression = r.u16();
    if (compression == 0)
        return r.bytes(bytes);
    QByteArray result(bytes, 0);
    if (compression == 1) {
        QVector<quint32> counts;
        for (int y = 0; y < h; ++y)
            counts.append(psb ? r.u32() : r.u16());
        for (int y = 0; y < h; ++y) {
            auto row = r.section(counts[y]);
            int x = 0;
            while (x < rowBytes) {
                int n = qint8(row.u8());
                if (n >= 0) {
                    const int count = n + 1;
                    if (x + count > rowBytes)
                        throw Error("psdCorrupt");
                    const auto literal = row.bytes(count);
                    memcpy(result.data() + y * rowBytes + x, literal.constData(), count);
                    x += count;
                } else if (n != -128) {
                    const int count = 1 - n;
                    if (x + count > rowBytes)
                        throw Error("psdCorrupt");
                    memset(result.data() + y * rowBytes + x, row.u8(), count);
                    x += count;
                }
            }
        }
        return result;
    }
    if (compression == 2 || compression == 3) {
        uLongf length = uLongf(bytes);
        const auto payload = r.bytes(r.data.size() - r.pos);
        if (uncompress(reinterpret_cast<Bytef *>(result.data()), &length,
                       reinterpret_cast<const Bytef *>(payload.constData()), uLong(payload.size())) != Z_OK ||
            length != bytes)
            throw Error("psdCorrupt");
        if (compression == 3)
            undoPrediction(result, w, h, depth);
        return result;
    }
    throw Error("psdCompression");
}
QByteArray croppedPlane(Reader &r, int width, int height, bool psb, int depth, QRect crop) {
    if (crop == QRect(0, 0, width, height)) return plane(r, width, height, psb, depth);
    if (crop.isEmpty()) return {};
    const int sampleBytes = depth / 8;
    const qint64 rowBytes = qint64(width) * sampleBytes;
    const qint64 total = rowBytes * height;
    if (width > 30000 || height > 30000 || qint64(width) * height > qint64(MaxSide) * MaxSide)
        throw Error("tooLarge");
    QByteArray result(qsizetype(qint64(crop.width()) * crop.height() * sampleBytes), 0);
    const int compression = r.u16();
    auto copyRow = [&](int y, const char *row) {
        if (y >= crop.top() && y < crop.bottom() + 1)
            memcpy(result.data() + qint64(y - crop.top()) * crop.width() * sampleBytes,
                   row + qint64(crop.left()) * sampleBytes, qsizetype(crop.width() * sampleBytes));
    };
    if (compression == 0) {
        r.need(total);
        for (int y = crop.top(); y <= crop.bottom(); ++y)
            copyRow(y, r.data.constData() + r.pos + y * rowBytes);
        r.skip(total);
    } else if (compression == 1) {
        QVector<quint32> lengths;
        lengths.reserve(height);
        for (int y = 0; y < height; ++y) lengths.append(psb ? r.u32() : r.u16());
        for (int y = 0; y < height; ++y) {
            auto encoded = r.section(lengths[y]);
            if (y < crop.top() || y > crop.bottom()) continue;
            QByteArray row(qsizetype(rowBytes), 0);
            qsizetype position = 0;
            while (position < rowBytes) {
                const int count = qint8(encoded.u8());
                if (count >= 0) {
                    const int n = count + 1;
                    if (position + n > rowBytes) throw Error("psdCorrupt");
                    const auto literal = encoded.bytes(n);
                    memcpy(row.data() + position, literal.constData(), n);
                    position += n;
                } else if (count != -128) {
                    const int n = 1 - count;
                    if (position + n > rowBytes) throw Error("psdCorrupt");
                    memset(row.data() + position, encoded.u8(), n);
                    position += n;
                }
            }
            copyRow(y, row.constData());
        }
    } else if (compression == 2 || compression == 3) {
        z_stream stream{};
        stream.next_in = reinterpret_cast<Bytef *>(r.data.data() + r.pos);
        stream.avail_in = uInt(r.data.size() - r.pos);
        if (inflateInit(&stream) != Z_OK) throw Error("psdCorrupt");
        try {
            QByteArray row(qsizetype(rowBytes), 0);
            for (int y = 0; y < height; ++y) {
                stream.next_out = reinterpret_cast<Bytef *>(row.data());
                stream.avail_out = uInt(rowBytes);
                while (stream.avail_out) {
                    const int status = inflate(&stream, Z_NO_FLUSH);
                    if (status != Z_OK && status != Z_STREAM_END) throw Error("psdCorrupt");
                    if (status == Z_STREAM_END && stream.avail_out) throw Error("psdCorrupt");
                }
                if (y >= crop.top() && y <= crop.bottom()) {
                    if (compression == 3) undoPrediction(row, width, 1, depth);
                    copyRow(y, row.constData());
                }
            }
            if (qint64(stream.total_out) != total) throw Error("psdCorrupt");
        } catch (...) {
            inflateEnd(&stream);
            throw;
        }
        inflateEnd(&stream);
    } else throw Error("psdCompression");
    return result;
}
int sampleChannel(const QByteArray &data, qint64 pixel, int depth) {
    return depth == 16 ? qFromBigEndian<quint16>(data.constData() + pixel * 2) : quint8(data[pixel]);
}
struct Record {
    Layer layer;
    QRect bounds, maskBounds, originalBounds, originalMaskBounds;
    int section = 0, maskDefault = 255;
    bool clipping = false, hasMask = false;
    QVector<QPair<int, quint64>> channels;
    QHash<QByteArray, QByteArray> extra;
};
QRect bounds(Reader &r) {
    qint64 t = r.i32(), l = r.i32(), b = r.i32(), right = r.i32();
    if (l < -1000000 || t < -1000000 || right - l < 0 || b - t < 0 || right - l > 30000 || b - t > 30000)
        throw Error("tooLarge");
    return QRect(int(l), int(t), int(right - l), int(b - t));
}
QString blend(QByteArray key) {
    static const QHash<QByteArray, QString> modes{{"norm", "Normal"},
                                                  {"mul ", "Multiply"},
                                                  {"scrn", "Screen"},
                                                  {"over", "Overlay"},
                                                  {"sLit", "Soft Light"},
                                                  {"hLit", "Hard Light"},
                                                  {"dark", "Darken"},
                                                  {"lite", "Lighten"},
                                                  {"diff", "Difference"},
                                                  {"div ", "Color Dodge"},
                                                  {"idiv", "Color Burn"},
                                                  {"hue ", "Hue"},
                                                  {"sat ", "Saturation"},
                                                  {"colr", "Color"},
                                                  {"lum ", "Luminosity"},
                                                  {"lbrn", "Linear Burn"},
                                                  {"lddg", "Linear Dodge (Add)"},
                                                  {"vLit", "Vivid Light"},
                                                  {"lLit", "Linear Light"},
                                                  {"pLit", "Pin Light"},
                                                  {"hMix", "Hard Mix"},
                                                  {"smud", "Exclusion"},
                                                  {"fsub", "Subtract"},
                                                  {"fdiv", "Divide"},
                                                  {"pass", "Normal"}};
    return modes.value(key);
}
QJsonObject adjustment(const Record &record) {
    QJsonObject out;
    QJsonObject identity{{"black", 0}, {"white", 255}, {"gamma", 1}, {"outputBlack", 0}, {"outputWhite", 255}};
    QJsonArray ranges{identity, identity, identity, identity};
    QJsonArray linear{QJsonObject{{"x", 0}, {"y", 0}}, QJsonObject{{"x", 255}, {"y", 255}}};
    QJsonArray channels{linear, linear, linear, linear};
    if (record.extra.contains("nvrt"))
        out["kind"] = "Invert";
    else if (record.extra.contains("levl")) {
        Reader r{record.extra["levl"]};
        r.u16();
        for (int i = 0; i < 4; ++i) {
            auto black = r.u16(), white = r.u16(), lo = r.u16(), hi = r.u16(), gamma = r.u16();
            ranges[i] = QJsonObject{{"black", std::clamp(int(black), 0, 254)},
                                    {"white", std::clamp(int(white), std::clamp(int(black), 0, 254) + 1, 255)},
                                    {"gamma", std::clamp(gamma / 100., .1, 9.99)},
                                    {"outputBlack", int(lo)},
                                    {"outputWhite", int(hi)}};
        }
        out["kind"] = "Levels";
    } else if (record.extra.contains("hue2")) {
        Reader r{record.extra["hue2"]};
        r.u16();
        out["colorize"] = r.u8() != 0;
        r.u8();
        auto colorHue = r.i16(), colorSat = r.i16(), colorLight = r.i16();
        out["kind"] = "Hue/Saturation";
        out["hue"] = int(r.i16());
        out["saturation"] = int(r.i16());
        out["lightness"] = int(r.i16());
        if (out["colorize"].toBool()) {
            out["hue"] = int(colorHue);
            out["saturation"] = int(colorSat);
            out["lightness"] = int(colorLight);
        }
        QJsonArray values{
            "Master",
            QJsonObject{{"hue", out["hue"]}, {"saturation", out["saturation"]}, {"lightness", out["lightness"]}}},
            bands;
        for (const auto &key : QStringList{"Reds", "Yellows", "Greens", "Cyans", "Blues", "Magentas"}) {
            if (r.data.size() - r.pos < 14)
                break;
            QJsonObject band;
            for (auto field : {"falloffStart", "rangeStart", "rangeEnd", "falloffEnd"}) {
                int angle = r.i16();
                band[field] = (angle % 360 + 360) % 360;
            }
            QJsonObject value;
            for (auto field : {"hue", "saturation", "lightness"})
                value[field] = r.i16();
            bands.append(key);
            bands.append(band);
            values.append(key);
            values.append(value);
        }
        out["hsvSettings"] =
            QJsonObject{{"range", "Master"}, {"colorize", out["colorize"]}, {"adjustments", values}, {"bands", bands}};
    } else if (record.extra.contains("curv")) {
        Reader r{record.extra["curv"]};
        if (!r.data.isEmpty() && r.data[0] == 0)
            r.u8();
        r.u16();
        int n = r.u16();
        if (n > 32)
            throw Error("psdCorrupt");
        for (int i = 0; i < n; ++i) {
            int ch = i, count = r.u16();
            if (count > 32 || count < 2)
                throw Error("psdCorrupt");
            QJsonArray points;
            for (int k = 0; k < count; ++k) {
                int y = std::clamp(int(r.u16()), 0, 255), x = std::clamp(int(r.u16()), 0, 255);
                points.append(QJsonObject{{"x", x}, {"y", y}});
            }
            if (ch < 4) {
                QVector<QJsonObject> sorted;
                for (const auto &v : points)
                    sorted.append(v.toObject());
                std::sort(sorted.begin(), sorted.end(),
                          [](const auto &a, const auto &b) { return a["x"].toDouble() < b["x"].toDouble(); });
                points = {};
                for (const auto &v : sorted)
                    if (points.isEmpty() || points.last().toObject()["x"] != v["x"])
                        points.append(v);
                if (points.first().toObject()["x"].toInt() != 0)
                    points.prepend(QJsonObject{{"x", 0}, {"y", points.first().toObject()["y"]}});
                if (points.last().toObject()["x"].toInt() != 255)
                    points.append(QJsonObject{{"x", 255}, {"y", points.last().toObject()["y"]}});
                if (points.size() <= 32)
                    channels[ch] = points;
            }
        }
        out["kind"] = "Curves";
    } else
        return {};
    if (!out.contains("hue"))
        out["hue"] = 0;
    if (!out.contains("saturation"))
        out["saturation"] = 0;
    if (!out.contains("lightness"))
        out["lightness"] = 0;
    if (!out.contains("colorize"))
        out["colorize"] = false;
    out["levels"] = QJsonObject{{"channel", "RGB"}, {"ranges", ranges}};
    out["curves"] = QJsonObject{{"channel", "RGB"}, {"channels", channels}};
    return out;
}

// EngineData uses a small PostScript dictionary, not JSON. Decode UTF-16 strings and style runs.
struct EngineReader {
    QByteArray data;
    qsizetype pos = 0;
    int items = 0;
    void space() {
        while (pos < data.size() && (quint8(data[pos]) <= 32))
            ++pos;
    }
    QJsonValue read(int depth = 0) {
        if (depth > 24 || ++items > 100000)
            throw Error("psdCorrupt");
        space();
        if (pos >= data.size())
            return {};
        if (data.mid(pos, 2) == "<<") {
            pos += 2;
            QJsonObject o;
            while (true) {
                space();
                if (data.mid(pos, 2) == ">>") {
                    pos += 2;
                    return o;
                }
                if (pos >= data.size() || data[pos] != '/')
                    throw Error("psdCorrupt");
                ++pos;
                auto begin = pos;
                while (pos < data.size() && quint8(data[pos]) > 32 && data[pos] != '/' && data[pos] != '>' &&
                       data[pos] != '[')
                    ++pos;
                auto key = QString::fromLatin1(data.mid(begin, pos - begin));
                o[key] = read(depth + 1);
            }
        }
        if (data[pos] == '[') {
            ++pos;
            QJsonArray a;
            while (true) {
                space();
                if (pos >= data.size())
                    throw Error("psdCorrupt");
                if (data[pos] == ']') {
                    ++pos;
                    return a;
                }
                a.append(read(depth + 1));
            }
        }
        if (data[pos] == '(') {
            ++pos;
            int level = 1;
            QByteArray bytes;
            while (pos < data.size() && level) {
                char c = data[pos++];
                if (c == '\\') {
                    if (pos >= data.size())
                        break;
                    c = data[pos++];
                    if (c == 'n')
                        c = '\n';
                    else if (c == 'r')
                        c = '\r';
                    else if (c == 't')
                        c = '\t';
                    else if (c >= '0' && c <= '7') {
                        int n = c - '0';
                        for (int i = 0; i < 2 && pos < data.size() && data[pos] >= '0' && data[pos] <= '7'; ++i)
                            n = n * 8 + (data[pos++] - '0');
                        c = char(n);
                    }
                    bytes.append(c);
                } else if (c == '(') {
                    ++level;
                    bytes.append(c);
                } else if (c == ')') {
                    if (--level)
                        bytes.append(c);
                } else
                    bytes.append(c);
            }
            if (level)
                throw Error("psdCorrupt");
            if (bytes.startsWith(QByteArray::fromHex("feff"))) {
                QString text;
                for (int i = 2; i + 1 < bytes.size(); i += 2)
                    text.append(QChar(qFromBigEndian<quint16>(bytes.constData() + i)));
                return text;
            }
            return QString::fromLatin1(bytes);
        }
        auto begin = pos;
        while (pos < data.size() && quint8(data[pos]) > 32 && data[pos] != ']' && data[pos] != '>')
            ++pos;
        if (pos == begin)
            throw Error("psdCorrupt");
        auto token = data.mid(begin, pos - begin);
        if (token == "true")
            return true;
        if (token == "false")
            return false;
        bool ok = false;
        double n = token.toDouble(&ok);
        return ok ? QJsonValue(n) : QJsonValue(QString::fromLatin1(token));
    }
};
std::optional<double> scalar(const QByteArray &data, const QByteArray &key, const QByteArray &type) {
    auto at = data.indexOf(key + type);
    if (at < 0)
        return {};
    Reader r{data};
    r.pos = at + key.size() + 4;
    if (type == "UntF") {
        r.skip(4);
        auto n = r.real();
        return std::isfinite(n) ? std::optional<double>(n) : std::nullopt;
    }
    if (type == "doub") {
        auto n = r.real();
        return std::isfinite(n) ? std::optional<double>(n) : std::nullopt;
    }
    if (type == "long")
        return r.i32();
    if (type == "bool")
        return r.u8();
    return {};
}
QColor vectorColor(const QByteArray &data) {
    auto r = scalar(data, "Rd  ", "doub"), g = scalar(data, "Grn ", "doub"), b = scalar(data, "Bl  ", "doub");
    if (!r || !g || !b)
        return {};
    auto channel = [](double n) { return std::clamp(n > 1 ? n / 255 : n, 0., 1.); };
    return QColor::fromRgbF(channel(*r), channel(*g), channel(*b));
}
QPainterPath vectorPath(const QByteArray &data, QSize canvas, bool *sharp = nullptr) {
    if (sharp)
        *sharp = false;
    if (data.size() < 8)
        return {};
    Reader r{data};
    r.skip(8);
    QPainterPath path;
    int remaining = 0, anchors = 0, subpaths = 0;
    bool closed = false, first = true, rect = true;
    QPointF start, incomingFirst, previous;
    auto point = [&](Reader &body) {
        double y = body.i32() / 16777216., x = body.i32() / 16777216.;
        return QPointF(x * canvas.width(), y * canvas.height());
    };
    auto close = [&] {
        if (!first && closed) {
            path.cubicTo(previous, incomingFirst, start);
            path.closeSubpath();
        }
    };
    while (r.data.size() - r.pos >= 26) {
        int type = r.u16();
        auto body = r.section(24);
        if (type == 0 || type == 3) {
            close();
            remaining = body.u16();
            closed = type == 0;
            first = true;
            ++subpaths;
        } else if ((type == 1 || type == 2 || type == 4 || type == 5) && remaining > 0) {
            --remaining;
            auto in = point(body), anchor = point(body), out = point(body);
            ++anchors;
            if (QLineF(in, anchor).length() > .5 || QLineF(out, anchor).length() > .5)
                rect = false;
            if (first) {
                path.moveTo(anchor);
                start = anchor;
                incomingFirst = in;
                first = false;
            } else
                path.cubicTo(previous, in, anchor);
            previous = out;
        }
    }
    close();
    if (sharp)
        *sharp = rect && anchors == 4 && subpaths == 1 && closed;
    return path;
}
void vectorMetadata(Record &record, QSize canvas, QStringList &notices) {
    auto mask = record.extra.value("vmsk", record.extra.value("vsms"));
    auto fill = vectorColor(record.extra.value("SoCo"));
    auto strokeData = record.extra.value("vstk");
    auto stroke = vectorColor(strokeData);
    bool fillOn = scalar(strokeData, "fillEnabled", "bool").value_or(fill.isValid()) != 0;
    bool strokeOn = scalar(strokeData, "strokeEnabled", "bool").value_or(0) != 0 && stroke.isValid();
    double strokeWidth = scalar(strokeData, "strokeStyleLineWidth", "UntF").value_or(1);
    if (strokeWidth < 0 || strokeWidth > 30000)
        throw Error("tooLarge");
    bool sharp = false;
    auto path = vectorPath(mask, canvas, &sharp);
    QString kind;
    double radius = 0;
    QRectF box;
    auto origin = record.extra.value("vogk");
    int type = int(scalar(origin, "keyOriginType", "long").value_or(0));
    if (type == 1 || type == 2 || type == 5) {
        auto l = scalar(origin, "Left", "UntF"), t = scalar(origin, "Top ", "UntF"),
             right = scalar(origin, "Rght", "UntF"), b = scalar(origin, "Btom", "UntF");
        if (l && t && right && b) {
            box = QRectF(QPointF(*l, *t), QPointF(*right, *b));
            kind = type == 5 ? "Ellipse" : "Rectangle";
        }
        auto radii = QList<double>{};
        for (auto key : {"topLeft", "topRight", "bottomRight", "bottomLeft"}) {
            auto v = scalar(origin, key, "UntF");
            if (v)
                radii.append(*v);
        }
        if (radii.size() == 4) {
            auto [lo, hi] = std::minmax_element(radii.begin(), radii.end());
            if (*hi - *lo > .5)
                kind.clear();
            else
                radius = std::max(0., *hi);
        }
    } else if (sharp) {
        kind = "Rectangle";
        box = path.boundingRect();
    }
    if (fillOn && fill.isValid() && !strokeOn && !kind.isEmpty() && box.width() >= 1 && box.height() >= 1) {
        auto bounds = box.toAlignedRect();
        if (bounds.width() > 30000 || bounds.height() > 30000 ||
            qint64(bounds.width()) * bounds.height() > MaxSourcePixels)
            throw Error("tooLarge");
        QImage image(bounds.size(), QImage::Format_ARGB32_Premultiplied);
        if (image.isNull())
            throw Error("tooLarge");
        image.fill(Qt::transparent);
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        if (kind == "Ellipse")
            p.drawEllipse(QRectF(QPointF(), bounds.size()));
        else
            p.drawRoundedRect(QRectF(QPointF(), bounds.size()), radius, radius);
        p.end();
        record.layer.image = image;
        record.layer.placement.origin = bounds.topLeft();
        record.layer.placement.size = bounds.size();
        record.layer.metadata["shape"] = QJsonObject{{"kind", kind},
                                                     {"red", fill.redF()},
                                                     {"green", fill.greenF()},
                                                     {"blue", fill.blueF()},
                                                     {"cornerRadius", radius}};
    } else if (!path.isEmpty() && ((fillOn && fill.isValid()) || strokeOn)) {
        auto bounds =
            path.boundingRect().adjusted(-strokeWidth, -strokeWidth, strokeWidth, strokeWidth).toAlignedRect();
        if (bounds.width() > 30000 || bounds.height() > 30000 ||
            qint64(bounds.width()) * bounds.height() > MaxSourcePixels)
            throw Error("tooLarge");
        QImage image(bounds.size(), QImage::Format_ARGB32_Premultiplied);
        if (image.isNull())
            throw Error("tooLarge");
        image.fill(Qt::transparent);
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        p.translate(-bounds.topLeft());
        if (fillOn && fill.isValid())
            p.fillPath(path, fill);
        if (strokeOn)
            p.strokePath(path, QPen(stroke, strokeWidth));
        p.end();
        record.layer.image = image;
        record.layer.placement.origin = bounds.topLeft();
        record.layer.placement.size = bounds.size();
        notices.append(record.layer.name + ": psdRasterVector");
    } else if (!path.isEmpty() && !record.layer.image.isNull()) {
        QImage alpha(record.layer.image.size(), QImage::Format_ARGB32_Premultiplied);
        alpha.fill(Qt::transparent);
        QPainter maskPainter(&alpha);
        maskPainter.translate(-record.layer.placement.origin);
        maskPainter.fillPath(path, Qt::white);
        maskPainter.end();
        QPainter p(&record.layer.image);
        p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        p.drawImage(0, 0, alpha);
        notices.append(record.layer.name + ": psdRasterVector");
    }
}
void textMetadata(Record &record, QStringList &notices) {
    if (!record.extra.contains("TySh"))
        return;
    try {
        Reader r{record.extra["TySh"]};
        r.u16();
        for (int i = 0; i < 6; ++i)
            r.real();
        r.u16();
        r.u32();
        auto desc = descriptor(r);
        QString content = desc["Txt "].toString();
        content.replace('\r', '\n');
        while (content.endsWith('\n'))
            content.chop(1);
        if (content.isEmpty() || record.layer.image.isNull())
            return;
        auto engineBytes = QByteArray::fromBase64(desc["EngineData"].toString().toLatin1());
        auto engine = EngineReader{engineBytes}.read().toObject();
        auto dict = engine["EngineDict"].toObject();
        auto styleRun = dict["StyleRun"].toObject();
        auto runs = styleRun["RunArray"].toArray();
        auto lengths = styleRun["RunLengthArray"].toArray();
        auto fonts = engine["ResourceDict"].toObject()["FontSet"].toArray();
        auto data = runs.isEmpty() ? QJsonObject()
                                   : runs.first().toObject()["StyleSheet"].toObject()["StyleSheetData"].toObject();
        auto fontName = [&](QJsonObject d) {
            int index = d["Font"].toInt();
            return index >= 0 && index < fonts.size() ? fonts[index].toObject()["Name"].toString("Arial")
                                                      : QString("Arial");
        };
        auto fillColor = [](QJsonObject d) {
            auto v = d["FillColor"].toObject()["Values"].toArray();
            return v.size() >= 4
                       ? QColor::fromRgbF(std::clamp(v[1].toDouble(), 0., 1.), std::clamp(v[2].toDouble(), 0., 1.),
                                          std::clamp(v[3].toDouble(), 0., 1.))
                       : QColor(Qt::black);
        };
        double size = std::clamp(data["FontSize"].toDouble(12), 1., 2000.);
        auto fill = fillColor(data);
        auto paragraph = dict["ParagraphRun"].toObject()["RunArray"].toArray();
        int alignment = paragraph.isEmpty() ? 0
                                            : paragraph.first()
                                                  .toObject()["ParagraphSheet"]
                                                  .toObject()["Properties"]
                                                  .toObject()["Justification"]
                                                  .toInt();
        QJsonObject style{{"content", content},
                          {"fontName", fontName(data)},
                          {"fontSize", size},
                          {"red", fill.redF()},
                          {"green", fill.greenF()},
                          {"blue", fill.blueF()},
                          {"alignment", alignment == 1   ? "Right"
                                        : alignment == 2 ? "Center"
                                                         : "Left"},
                          {"tracking", data["Tracking"].toDouble() * size / 1000},
                          {"leading", data["AutoLeading"].toBool(true) ? 0 : data["Leading"].toDouble()}};
        QJsonArray colorRuns, fontRuns;
        int location = 0;
        for (int i = 0; i < runs.size() && i < lengths.size(); ++i) {
            int length = std::clamp(lengths[i].toInt(), 0, int(content.size()) - location);
            auto run = runs[i].toObject()["StyleSheet"].toObject()["StyleSheetData"].toObject();
            auto color = fillColor(run);
            if (length > 0) {
                fontRuns.append(QJsonObject{{"location", location}, {"length", length}, {"fontName", fontName(run)}});
                colorRuns.append(QJsonObject{{"location", location},
                                             {"length", length},
                                             {"red", color.redF()},
                                             {"green", color.greenF()},
                                             {"blue", color.blueF()}});
            }
            location += length;
        }
        if (!colorRuns.isEmpty())
            style["colorRuns"] = colorRuns;
        if (!fontRuns.isEmpty())
            style["fontRuns"] = fontRuns;
        record.layer.metadata["text"] = style;
        notices.append(record.layer.name + ": psdText");
    } catch (const Error &) {
        notices.append(record.layer.name + ": psdRasterText");
    }
}
} // namespace
ImportResult readPhotoshop(const QString &path, qint64 pixelBudget) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw Error("fileError");
    if (file.size() > 2ll * 1024 * 1024 * 1024)
        throw Error("tooLarge");
    Reader r{file.readAll()};
    if (r.bytes(4) != "8BPS")
        throw Error("psdCorrupt");
    int version = r.u16();
    if (version != 1 && version != 2)
        throw Error("psdVersion");
    bool psb = version == 2;
    r.skip(6);
    int compositeChannels = r.u16();
    int height = int(r.u32()), width = int(r.u32());
    int depth = r.u16(), mode = r.u16();
    if ((depth != 8 && depth != 16) || (mode != 3 && mode != 4) || (mode == 4 && depth != 8))
        throw Error("psdMode");
    State state;
    state.id = newId();
    state.size = {width, height};
    state.bitDepth = depth;
    validateState(state);
    r.skip(r.u32());
    QColorSpace sourceSpace(QColorSpace::SRgb);
    QByteArray cmykProfile = mode == 4 ? chosenCmykProfile() : QByteArray();
    auto resources = r.section(r.u32());
    while (resources.data.size() - resources.pos >= 12) {
        if (resources.bytes(4) != "8BIM")
            throw Error("psdCorrupt");
        auto id = resources.u16();
        auto nameLength = resources.u8();
        resources.skip(nameLength);
        if ((nameLength + 1) % 2)
            resources.skip(1);
        auto length = resources.u32();
        auto payload = resources.section(length);
        if (id == 1005 && length >= 4)
            state.resolution = std::clamp(payload.u32() / 65536., 1., 9600.);
        if (id == 1039) {
            if (mode == 4) { validateCmykProfile(payload.data); cmykProfile = payload.data; }
            else {
                const auto profile = QColorSpace::fromIccProfile(payload.data);
                if (!profile.isValid() || profile.colorModel() != QColorSpace::ColorModel::Rgb) throw Error("psdProfile");
                sourceSpace = profile;
            }
        }
        if (length % 2)
            resources.skip(1);
    }
    if (mode == 4) {
        validateCmykProfile(cmykProfile);
        state.metadata["windowsCMYKProfile"] = QString::fromLatin1(cmykProfile.toBase64());
    }
    auto fromInks = [&](QImage inks, const QByteArray &alpha) {
        QImage rgb = cmykToRgb(inks, cmykProfile);
        if (!alpha.isEmpty()) for (int y=0;y<rgb.height();++y) {
            auto *row = reinterpret_cast<QRgba64 *>(rgb.scanLine(y));
            for(int x=0;x<rgb.width();++x) {
                const quint16 a=quint8(alpha[y*rgb.width()+x])*257;
                const auto c=row[x]; row[x]=QRgba64::fromRgba64(c.red(),c.green(),c.blue(),a).premultiplied();
            }
        }
        return rgb.convertToFormat(pixelFormat(depth));
    };
    auto section = r.section(psb ? r.u64() : r.u32());
    QVector<Record> records;
    QStringList notices;
    if (section.data.size() >= (psb ? 8 : 4)) {
        auto layerInfo = section.section(psb ? section.u64() : section.u32());
        // Photoshop stores high-depth layer records in a global Lr16 block.
        if (depth == 16 && section.data.size() - section.pos >= 4) {
            section.skip(section.u32()); // Global layer mask.
            while (section.data.size() - section.pos >= 12) {
                const auto signature = section.bytes(4), key = section.bytes(4);
                if (signature != "8BIM" && signature != "8B64")
                    throw Error("psdCorrupt");
                const bool large = signature == "8B64" ||
                                   (psb && QList<QByteArray>{"LMsk", "Lr16", "Lr32", "Layr", "Mt16", "Mt32", "Mtrn",
                                                             "Alph", "FMsk", "lnk2", "FEid", "FXid", "PxSD"}
                                               .contains(key));
                const auto length = large ? section.u64() : section.u32();
                auto block = section.section(length);
                if (key == "Lr16")
                    layerInfo = std::move(block);
                const auto padding = (4 - length % 4) % 4;
                if (padding && section.data.size() - section.pos >= qsizetype(padding))
                    section.skip(padding);
            }
        }
        if (layerInfo.data.size() >= 2) {
            int count = abs(int(layerInfo.i16()));
            if (count > MaxLayers * 2)
                throw Error("tooLarge");
            for (int i = 0; i < count; ++i) {
                Record rec;
                rec.layer.id = newId();
                rec.bounds = bounds(layerInfo);
                rec.originalBounds = rec.bounds;
                auto n = layerInfo.u16();
                if (n > 56)
                    throw Error("psdCorrupt");
                for (int k = 0; k < n; ++k) {
                    int id = layerInfo.i16();
                    auto length = psb ? layerInfo.u64() : layerInfo.u32();
                    rec.channels.append({id, length});
                }
                if (layerInfo.bytes(4) != "8BIM")
                    throw Error("psdCorrupt");
                auto modeKey = layerInfo.bytes(4);
                rec.layer.blend = blend(modeKey);
                if (rec.layer.blend.isEmpty()) {
                    rec.layer.blend = "Normal";
                    notices.append("psdBlend");
                }
                rec.layer.opacity = layerInfo.u8() / 255.;
                rec.clipping = layerInfo.u8() != 0;
                rec.layer.visible = (layerInfo.u8() & 2) == 0;
                layerInfo.u8();
                auto extra = layerInfo.section(layerInfo.u32());
                auto mask = extra.section(extra.u32());
                if (mask.data.size() >= 18) {
                    rec.hasMask = true;
                    rec.maskBounds = bounds(mask);
                    rec.originalMaskBounds = rec.maskBounds;
                    rec.maskDefault = mask.u8();
                    int flags = mask.u8();
                    rec.layer.maskEnabled = !(flags & 2);
                    rec.layer.maskLinked = !(flags & 1);
                }
                extra.skip(extra.u32());
                int len = extra.u8();
                rec.layer.name = QString::fromLatin1(extra.bytes(len));
                extra.skip((4 - (len + 1) % 4) % 4);
                while (extra.data.size() - extra.pos >= 12) {
                    auto signature = extra.bytes(4);
                    if (signature != "8BIM" && signature != "8B64")
                        throw Error("psdCorrupt");
                    auto key = extra.bytes(4);
                    bool large = signature == "8B64" ||
                                 (psb && QList<QByteArray>{"LMsk", "Lr16", "Lr32", "Layr", "Mt16", "Mt32", "Mtrn",
                                                           "Alph", "FMsk", "lnk2", "FEid", "FXid", "PxSD"}
                                             .contains(key));
                    auto length = large ? extra.u64() : extra.u32();
                    auto payload = extra.bytes(length);
                    rec.extra[key] = payload;
                    if (length % 2)
                        extra.skip(1);
                    if (key == "luni") {
                        Reader name{payload};
                        rec.layer.name = name.unicode();
                    }
                    if ((key == "lsct" || key == "lsdk") && payload.size() >= 4) {
                        Reader folder{payload};
                        rec.section = int(folder.u32());
                    }
                    if (key == "iOpa" && !payload.isEmpty())
                        rec.layer.opacity *= quint8(payload[0]) / 255.;
                }
                if (rec.layer.name.trimmed().isEmpty())
                    rec.layer.name = "Layer " + QString::number(i + 1);
                records.append(rec);
            }
            auto fits = [&] {
                qint64 images = 0, masks = 0;
                for (const auto &rec : records) {
                    if (!rec.layer.group && rec.section == 0)
                        images += qint64(rec.bounds.width()) * rec.bounds.height();
                    if (rec.hasMask)
                        masks += qint64(rec.maskBounds.width()) * rec.maskBounds.height();
                }
                return images <= pixelBudget && masks <= pixelBudget;
            };
            if (!fits()) {
                const QRect canvas(QPoint(0, 0), state.size);
                for (auto &rec : records) {
                    rec.bounds = rec.bounds.intersected(canvas);
                    rec.maskBounds = rec.maskBounds.intersected(canvas);
                    if (rec.bounds != rec.originalBounds || rec.maskBounds != rec.originalMaskBounds)
                        notices.append(rec.layer.name + ": psdCropped");
                }
                if (!fits()) throw Error("tooLarge");
            }
            qint64 budget = 0, maskBudget = 0;
            for (auto &rec : records) {
                QHash<int, QByteArray> planes;
                if (!rec.bounds.isEmpty() && !rec.layer.group) {
                    budget += qint64(rec.bounds.width()) * rec.bounds.height();
                    if (budget > pixelBudget) throw Error("tooLarge");
                }
                for (auto channel : rec.channels) {
                    auto data = layerInfo.section(channel.second);
                    bool isMask = channel.first == -2;
                    const QRect original = isMask ? rec.originalMaskBounds : rec.originalBounds;
                    const QRect clipped = isMask ? rec.maskBounds : rec.bounds;
                    if (!(mode == 4 ? QList<int>{-2,-1,0,1,2,3} : QList<int>{-2,-1,0,1,2}).contains(channel.first) || original.isEmpty() || clipped.isEmpty())
                        continue;
                    if (isMask) {
                        maskBudget += qint64(clipped.width()) * clipped.height();
                        if (maskBudget > pixelBudget) throw Error("tooLarge");
                    }
                    planes[channel.first] = croppedPlane(data, original.width(), original.height(), psb, depth,
                                                          clipped.translated(-original.topLeft()));
                }
                if (rec.section != 1 && rec.section != 2 && rec.section != 3 && !rec.bounds.isEmpty()) {
                    QImage image(rec.bounds.size(), pixelFormat(depth));
                    if (image.isNull())
                        throw Error("tooLarge");
                    for (int y = 0; y < image.height(); ++y) {
                        for (int x = 0; x < image.width(); ++x) {
                            const qint64 p = qint64(y) * image.width() + x;
                            auto channel = [&](int id, int fallback) {
                                return planes.contains(id) ? sampleChannel(planes[id], p, depth) : fallback;
                            };
                            if (depth == 16)
                                reinterpret_cast<QRgba64 *>(image.scanLine(y))[x] =
                                    rgba64(channel(0, 0), channel(1, 0), channel(2, 0), channel(-1, 65535))
                                        .premultiplied();
                            else
                                reinterpret_cast<QRgb *>(image.scanLine(y))[x] =
                                    qPremultiply(qRgba(channel(0, 0), channel(1, 0), channel(2, 0), channel(-1, 255)));
                        }
                    }
                    if (mode == 4) {
                        QImage inks(image.size(),QImage::Format_CMYK8888);
                        if(inks.isNull())throw Error("tooLarge");
                        for(int y=0;y<inks.height();++y)for(int x=0;x<inks.width();++x)for(int c=0;c<4;++c)
                            inks.scanLine(y)[x*4+c]=quint8(255-(planes.contains(c)?quint8(planes[c][y*inks.width()+x]):255));
                        image=fromInks(inks,planes.value(-1));
                    }
                    rec.layer.image = image;
                    rec.layer.placement.origin = rec.bounds.topLeft();
                    rec.layer.placement.size = rec.bounds.size();
                }
                if (planes.contains(-2)) {
                    QSize grid = rec.layer.image.isNull() ? state.size : rec.layer.image.size();
                    QPoint origin = rec.layer.image.isNull() ? QPoint() : rec.bounds.topLeft();
                    rec.layer.mask = QImage(grid, maskFormat(depth));
                    if (rec.layer.mask.isNull())
                        throw Error("tooLarge");
                    rec.layer.mask.fill(QColor(rec.maskDefault, rec.maskDefault, rec.maskDefault));
                    QImage patch(rec.maskBounds.size(), maskFormat(depth));
                    if (patch.isNull())
                        throw Error("tooLarge");
                    for (int y = 0; y < patch.height(); ++y)
                        for (int x = 0; x < patch.width(); ++x) {
                            const int value = sampleChannel(planes[-2], qint64(y) * patch.width() + x, depth);
                            if (depth == 16)
                                reinterpret_cast<quint16 *>(patch.scanLine(y))[x] = quint16(value);
                            else
                                patch.scanLine(y)[x] = uchar(value);
                        }
                    QPainter p(&rec.layer.mask);
                    p.drawImage(rec.maskBounds.topLeft() - origin, patch);
                    rec.layer.maskPlacement = rec.layer.placement;
                    if (rec.layer.image.isNull()) {
                        rec.layer.maskPlacement = {};
                        rec.layer.maskPlacement.size = grid;
                    }
                }
                vectorMetadata(rec, state.size, notices);
                auto a = adjustment(rec);
                if (!a.isEmpty()) {
                    rec.layer.metadata["adjustment"] = a;
                    rec.layer.image = {};
                    rec.layer.placement.origin = {};
                    rec.layer.placement.size = state.size;
                }
                if (rec.layer.image.isNull() && rec.layer.placement.size.width() < 1)
                    rec.layer.placement.size = state.size;
                textMetadata(rec, notices);
                for (const auto &key : QList<QByteArray>{"SoLd", "SoLE", "lfx2", "lrFX", "lmfx", "brit", "blnc", "expA",
                                                         "grdm", "selc", "blwh"})
                    if (rec.extra.contains(key))
                        notices.append(rec.layer.name + ": psdRasterFallback");
            }
        }
    }
    if (records.isEmpty()) {
        int compression = r.u16();
        const qint64 pixels = qint64(width) * height, planeBytes = pixels * (depth / 8);
        if (compositeChannels < (mode == 4 ? 4 : 3) || compositeChannels > 56)
            throw Error("psdCorrupt");
        QVector<QByteArray> planes;
        if (compression == 0) {
            for (int c = 0; c < compositeChannels; ++c) {
                auto bytes = r.bytes(planeBytes);
                if (c < (mode == 4 ? 5 : 4))
                    planes.append(bytes);
            }
        } else if (compression == 1) {
            QVector<quint32> lengths;
            for (int i = 0; i < compositeChannels * height; ++i)
                lengths.append(psb ? r.u32() : r.u16());
            for (int c = 0; c < compositeChannels; ++c) {
                QByteArray payload;
                for (int y = 0; y < height; ++y) {
                    if (psb) {
                        quint32 n = qToBigEndian(lengths[c * height + y]);
                        payload.append(reinterpret_cast<char *>(&n), 4);
                    } else {
                        quint16 n = qToBigEndian(quint16(lengths[c * height + y]));
                        payload.append(reinterpret_cast<char *>(&n), 2);
                    }
                }
                for (int y = 0; y < height; ++y)
                    payload += r.bytes(lengths[c * height + y]);
                if (c < (mode == 4 ? 5 : 4)) {
                    Reader channel{QByteArray::fromHex("0001") + payload};
                    planes.append(plane(channel, width, height, psb, depth));
                }
            }
        } else if (compression == 2 || compression == 3) {
            qint64 total = planeBytes * compositeChannels;
            if (total > MaxSourcePixels * 8)
                throw Error("tooLarge");
            QByteArray decoded(total, 0);
            uLongf length = uLongf(total);
            auto payload = r.bytes(r.data.size() - r.pos);
            if (uncompress(reinterpret_cast<Bytef *>(decoded.data()), &length,
                           reinterpret_cast<const Bytef *>(payload.constData()), uLong(payload.size())) != Z_OK ||
                length != total)
                throw Error("psdCorrupt");
            for (int c = 0; c < std::min(compositeChannels, mode == 4 ? 5 : 4); ++c) {
                auto bytes = decoded.mid(c * planeBytes, planeBytes);
                if (compression == 3)
                    undoPrediction(bytes, width, height, depth);
                planes.append(bytes);
            }
        } else
            throw Error("psdCompression");
        Layer l;
        l.id = newId();
        l.name = QFileInfo(path).completeBaseName();
        l.image = QImage(state.size, pixelFormat(depth));
        if (l.image.isNull())
            throw Error("tooLarge");
        l.placement.size = state.size;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const qint64 p = qint64(y) * width + x;
                auto channel = [&](int c) {
                    return c < planes.size() ? sampleChannel(planes[c], p, depth) : (depth == 16 ? 65535 : 255);
                };
                if (depth == 16)
                    reinterpret_cast<QRgba64 *>(l.image.scanLine(y))[x] =
                        rgba64(channel(0), channel(1), channel(2), channel(3)).premultiplied();
                else
                    reinterpret_cast<QRgb *>(l.image.scanLine(y))[x] =
                        qPremultiply(qRgba(channel(0), channel(1), channel(2), channel(3)));
            }
        if (mode == 4) {
            QImage inks(l.image.size(),QImage::Format_CMYK8888);if(inks.isNull())throw Error("tooLarge");
            for(int y=0;y<height;++y)for(int x=0;x<width;++x)for(int c=0;c<4;++c)
                inks.scanLine(y)[x*4+c]=quint8(255-quint8(planes[c][y*width+x]));
            l.image=fromInks(inks,planes.size()>4?planes[4]:QByteArray());
        }
        state.layers.append(l);
    } else {
        // PSD files normally store the topmost record first; recognize bottom-first section markers as well.
        bool bottomFirst = false;
        for (const auto &rec : records)
            if (rec.section) {
                bottomFirst = rec.section == 3;
                break;
            }
        if (!bottomFirst)
            std::reverse(records.begin(), records.end());
        QStringList groups;
        for (auto &rec : records) {
            if (rec.section == 3) {
                groups.append(newId());
                continue;
            }
            if (rec.section == 1 || rec.section == 2) {
                rec.layer.group = true;
                rec.layer.image = {};
                rec.layer.blend = "Normal";
                rec.layer.placement.origin = {};
                rec.layer.placement.size = state.size;
                rec.layer.maskPlacement.size = state.size;
                rec.layer.id = groups.isEmpty() ? newId() : groups.takeLast();
            }
            rec.layer.parent = groups.isEmpty() ? QString() : groups.last();
            state.layers.append(rec.layer);
        }
        if (!groups.isEmpty())
            throw Error("psdCorrupt");
        QHash<QString, QString> bases;
        int index = 0;
        for (const auto &rec : records) {
            if (rec.section == 3)
                continue;
            auto &l = state.layers[index++];
            if (rec.clipping && !l.group) {
                l.maskSource = bases.value(l.parent);
                if (l.maskSource.isEmpty())
                    notices.append(l.name + ": psdClip");
            } else if (!l.group)
                bases[l.parent] = l.id;
        }
    }
    if (!state.layers.isEmpty())
        state.active = state.layers.last().id;
    if (sourceSpace != QColorSpace(QColorSpace::SRgb)) {
        const auto transform = sourceSpace.transformationToColorSpace(QColorSpace::SRgb);
        std::function<QJsonValue(QJsonValue)> mapColors = [&](QJsonValue value) -> QJsonValue {
            if (value.isArray()) {
                QJsonArray out;
                for (const auto &v : value.toArray())
                    out.append(mapColors(v));
                return out;
            }
            if (!value.isObject())
                return value;
            auto object = value.toObject();
            if (object["red"].isDouble() && object["green"].isDouble() && object["blue"].isDouble()) {
                const auto color = transform.map(
                    QColor::fromRgbF(object["red"].toDouble(), object["green"].toDouble(), object["blue"].toDouble()));
                object["red"] = color.redF();
                object["green"] = color.greenF();
                object["blue"] = color.blueF();
            }
            for (auto it = object.begin(); it != object.end(); ++it)
                if (it->isArray() || it->isObject())
                    it.value() = mapColors(it.value());
            return object;
        };
        for (auto &layer : state.layers) {
            if (!layer.image.isNull()) {
                layer.image.setColorSpace(sourceSpace);
                layer.image.convertToColorSpace(QColorSpace::SRgb, pixelFormat(depth));
            }
            for (const auto &key : QStringList{"text", "shape"})
                if (layer.metadata.contains(key))
                    layer.metadata[key] = mapColors(layer.metadata[key]);
        }
        notices.append("psdProfileConverted");
    }
    notices.removeDuplicates();
    validateState(state);
    return {state, notices};
}
} // namespace comp
