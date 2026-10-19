#include "WideKernels.h"
#include "Pixels.h"
#include "CameraRawTables.h"
#include <QFile>
#include <QResource>
#include <QtEndian>
#include <cmath>
#include <bit>
static void initRawTables() {
    Q_INIT_RESOURCE(rawtables);
    Q_INIT_RESOURCE(rawshadows);
}
namespace comp::rawtables {
static constexpr int Entries = Size * Size * Size * 3, Count = 763;
static const QByteArray &tables() {
    static const QByteArray bytes = [] {
        initRawTables();
        QFile file(":/raw/CameraRawTables.qz");
        if (!file.open(QIODevice::ReadOnly))
            throw Error("fileError", "CameraRawTables.qz");
        const auto packed = file.readAll();
        if (packed.size() < 14 || packed.left(4) != "CRT3" ||
            qFromLittleEndian<quint16>(packed.constData() + 4) != Size ||
            qFromLittleEndian<quint16>(packed.constData() + 6) != Count)
            throw Error("invalidProject", "Camera Raw table header");
        const int step = qFromLittleEndian<quint16>(packed.constData() + 8);
        const auto planes = qUncompress(packed.mid(10));
        if (step <= 0 || planes.size() != Count * Entries)
            throw Error("invalidProject", "Camera Raw table payload");
        QByteArray out(Count * Entries, 0);
        constexpr int cells = Size * Size * Size;
        for (int t = 0; t < Count; ++t)
            for (int c = 0; c < 3; ++c) {
                quint8 running = 0;
                for (int cell = 0; cell < cells; ++cell) {
                    running = quint8(running + quint8(planes[(t * 3 + c) * cells + cell]));
                    const int axis = c == 0 ? cell / (Size * Size) : c == 1 ? cell / Size % Size : cell % Size;
                    const int delta = running < 128 ? running : int(running) - 256;
                    out[t * Entries + cell * 3 + c] =
                        char(std::clamp(qRound(axis * 255. / (Size - 1)) + delta * step, 0, 255));
                }
            }
        return out;
    }();
    return bytes;
}
static const uint8_t *table(int index) {
    Q_ASSERT(index >= 0 && index < Count);
    return reinterpret_cast<const uint8_t *>(tables().constData()) + index * Entries;
}
const uint8_t *exposure() {
    return table(81);
}
static double v(const QJsonObject &o, const char *key, double fallback = 0) {
    return o[key].toDouble(fallback);
}
static std::pair<int, double> between(double value, std::initializer_list<double> levels) {
    const QVector<double> a(levels);
    value = std::clamp(value, a.first(), a.last());
    int i = 0;
    while (i < a.size() - 2 && value > a[i + 1])
        ++i;
    return {i, (value - a[i]) / (a[i + 1] - a[i])};
}
static CameraRawStage blend(std::initializer_list<std::pair<const uint8_t *, double>> parts) {
    CameraRawStage s{};
    int i = 0;
    for (const auto &p : parts) {
        s.table[i] = p.first;
        s.weight[i++] = float(p.second);
    }
    return s;
}
static CameraRawStage stage(int start, double value, double low = -100, double step = 50, int count = 5) {
    const double position = std::clamp((value - low) / step, 0., double(count - 1));
    const int i = std::min(count - 2, int(position));
    const double f = position - i;
    return blend({{table(start + i), 1 - f}, {table(start + i + 1), f}});
}
static CameraRawStage whiteBalance(double temperature, double tint) {
    double row = std::clamp((temperature + 100) / 25, 0., 8.), col = std::clamp((tint + 100) / 25, 0., 8.);
    int r = std::min(7, int(row)), c = std::min(7, int(col));
    row -= r;
    col -= c;
    return blend({{table(r * 9 + c), (1 - row) * (1 - col)},
                  {table(r * 9 + c + 1), (1 - row) * col},
                  {table((r + 1) * 9 + c), row * (1 - col)},
                  {table((r + 1) * 9 + c + 1), row * col}});
}
static CameraRawStage adaptive(int start, double value, double brightness) {
    const auto [row, down] = between(brightness, {53, 80, 104, 130, 160, 190, 224});
    const auto [col, across] = between(value, {-100, -50, -25, 0, 25, 50, 100});
    const int base = start + row * 7 + col;
    return blend({{table(base), (1 - down) * (1 - across)},
                  {table(base + 1), (1 - down) * across},
                  {table(base + 7), down * (1 - across)},
                  {table(base + 8), down * across}});
}
bool measuredCurve(const QJsonObject &c) {
    return v(c, "shadowSplit", 25) == 25 && v(c, "darkSplit", 50) == 50 && v(c, "lightSplit", 75) == 75 &&
           v(c, "refineSaturation") == 0;
}
QVector<CameraRawStage> stages(const QJsonObject &s, const QImage &image) {
    const auto light=s["enableLight"].toBool(true)?s:QJsonObject();
    double brightness[]{.5, .5, .5};
    if (v(light, "contrast") || v(light, "highlights"))
        if (wideImage(image))
            wide::camera_raw_statistics(reinterpret_cast<const quint16 *>(image.constBits()), image.width(), image.height(), image.bytesPerLine() / 2, brightness);
        else
            camera_raw_statistics(image.constBits(), image.width(), image.height(), image.bytesPerLine(), brightness);
    return stagesWithStatistics(s, brightness);
}
QVector<CameraRawStage> stagesWithStatistics(const QJsonObject &s, const double brightness[3]) {
    QVector<CameraRawStage> out;
    const auto cal = s["enableCalibration"].toBool(true) ? s["calibration"].toObject() : QJsonObject();
    int index = 0;
    for (auto key : {"redHue", "redSaturation", "greenHue", "greenSaturation", "blueHue", "blueSaturation"}) {
        if (v(cal, key))
            out.append(stage(405 + index * 5, v(cal, key)));
        ++index;
    }
    const auto light = s["enableLight"].toBool(true) ? s : QJsonObject();
    const auto color = s["enableColor"].toBool(true) ? s : QJsonObject();
    if (v(light, "exposure"))
        out.append(stage(81, v(light, "exposure"), -5, .5, 21));
    if (v(color, "temperature") || v(color, "tint"))
        out.append(whiteBalance(v(color, "temperature"), v(color, "tint")));
    if (v(light, "contrast"))
        out.append(adaptive(138, v(light, "contrast"), brightness[0] * 255));
    if (v(light, "highlights"))
        out.append(adaptive(187, v(light, "highlights"), brightness[2] * 255));

    if (v(light, "whites"))
        out.append(stage(102, v(light, "whites"), -100, 25, 9));
    if (v(light, "blacks"))
        out.append(stage(111, v(light, "blacks"), -100, 25, 9));
    if (v(color, "saturation"))
        out.append(stage(120, v(color, "saturation"), -100, 25, 9));
    if (v(color, "vibrance"))
        out.append(stage(129, v(color, "vibrance"), -100, 25, 9));
    const auto curve = s["enableCurve"].toBool(true) ? s["curve"].toObject() : QJsonObject();
    if (measuredCurve(curve)) {
        index = 0;
        for (auto key : {"shadows", "darks", "lights", "highlights"}) {
            if (v(curve, key))
                out.append(stage(435 + index * 5, v(curve, key)));
            ++index;
        }
    }
    return out;
}
int beforeShadowsCount(const QJsonObject &s) {
    const auto cal = s["enableCalibration"].toBool(true) ? s["calibration"].toObject() : QJsonObject();
    const auto light = s["enableLight"].toBool(true) ? s : QJsonObject();
    const auto color = s["enableColor"].toBool(true) ? s : QJsonObject();
    int count = 0;
    for (const auto *key : {"redHue", "redSaturation", "greenHue", "greenSaturation", "blueHue", "blueSaturation"})
        count += v(cal, key) != 0;
    return count + (v(light, "exposure") != 0) + (v(color, "temperature") != 0 || v(color, "tint") != 0) +
           (v(light, "contrast") != 0) + (v(light, "highlights") != 0);
}
void applyShadows(QImage &image, double amount, double scale) {
    if (amount == 0 || image.isNull()) return;
    static const QByteArray bytes = [] {
        initRawTables();
        QFile file(":/raw/CameraRawShadows.bin");
        if (!file.open(QIODevice::ReadOnly)) throw Error("fileError", "CameraRawShadows.bin");
        return file.readAll();
    }();
    if (bytes.size() < 34 || bytes.left(4) != "CRS2") throw Error("invalidProject", "Camera Raw shadows");
    auto word = [&](int at) { return qFromLittleEndian<quint16>(bytes.constData() + at); };
    auto real = [&](int at) { return std::bit_cast<float>(qFromLittleEndian<quint32>(bytes.constData() + at)); };
    const int sets = word(4), amounts = word(6), columns = word(8), rows = word(10), terms = word(12);
    const qint64 per = qint64(columns) * rows * terms;
    if (sets != 2 || amounts < 1 || columns < 2 || rows < 1 || terms != 3 ||
        bytes.size() != 34 + amounts * 4 + sets * amounts * per * 4)
        throw Error("invalidProject", "Camera Raw shadows payload");
    QVector<QPair<double, int>> points{{0, -1}};
    for (int i = 0; i < amounts; ++i) points.append({real(34 + i * 4), i});
    std::sort(points.begin(), points.end());
    amount = std::clamp(amount, points.front().first, points.back().first);
    int index = 0;
    while (index < points.size() - 2 && amount > points[index + 1].first) ++index;
    const float mix = float((amount - points[index].first) / (points[index + 1].first - points[index].first));
    const int set = std::max(image.width(), image.height()) / (scale > 0 ? scale : 1) >= real(14) ? 1 : 0;
    auto coefficient = [&](int amountIndex, int element) {
        return amountIndex < 0 ? 0.f : real(int(34 + amounts * 4 + ((set * amounts + amountIndex) * per + element) * 4));
    };
    QVector<float> values(per);
    for (int i = 0; i < values.size(); ++i)
        values[i] = coefficient(points[index].second, i) * (1 - mix) + coefficient(points[index + 1].second, i) * mix;
    if (wideImage(image))
        wide::adjust_camera_raw_shadows(reinterpret_cast<quint16 *>(image.bits()), image.width(), image.height(),
                                       image.bytesPerLine() / 2, values.constData(), columns, real(18), real(22), rows, real(26), real(30));
    else
        adjust_camera_raw_shadows(image.bits(), image.width(), image.height(), image.bytesPerLine(), values.constData(),
                                  columns, real(18), real(22), rows, real(26), real(30));
}
QVector<CameraRawStage> mixer(const QJsonObject &s) {
    QVector<CameraRawStage> out;
    int kind = 0;
    for (auto key : {"luminance", "saturation", "hue"}) {
        const auto values = s[key].toArray();
        for (int color = 0; color < std::min(8, int(values.size())); ++color)
            if (values[color].toDouble())
                out.append(stage(285 + (kind * 8 + color) * 5, values[color].toDouble()));
        ++kind;
    }
    return out;
}
QVector<CameraRawStage> grading(const QJsonObject &s) {
    QVector<CameraRawStage> out;
    int index = 0;
    for (auto key : {"shadows", "midtones", "highlights", "global"}) {
        double amount = v(s[key].toObject(), "luminance");
        if (amount)
            out.append(stage(743 + index * 5, amount));
        ++index;
    }
    double balance = v(s, "balance") / 100;
    const double shares[]{std::max(0., balance < 0 ? 1 - 1.2 * balance : 1 - 1.4 * balance),
                          std::max(0., balance < 0 ? 1 + 1.4 * balance : 1 + 2 * balance), 1, 1};
    const QStringList keys{"shadows", "highlights", "midtones", "global"};
    const auto [blendLow, blendUp] = between(v(s, "blending", 50), {0, 50, 100});
    for (int wheel : {3, 2, 1, 0}) {
        auto color = s[keys[wheel]].toObject();
        double saturation = v(color, "saturation") * shares[wheel];
        if (saturation <= 0)
            continue;
        double position = std::fmod(v(color, "hue"), 360.) / 30;
        int first = int(position) % 12, second = (first + 1) % 12;
        double around = position - int(position);
        const auto [level, up] = between(saturation, {0, 25, 50, 100});
        CameraRawStage entry{};
        int slot = 0;
        const QVector<std::pair<int, double>> blendings =
            wheel < 2 ? QVector<std::pair<int, double>>{{blendLow, 1 - blendUp}, {blendLow + 1, blendUp}}
                      : QVector<std::pair<int, double>>{{1, 1}};
        for (const auto &[bi, bw] : blendings) {
            const int base = 455 + (bi == 0 ? 0 : bi == 1 ? 72 : 216) + wheel * 36;
            for (const auto &[hue, hw] : {std::pair{first, 1 - around}, std::pair{second, around}})
                for (const auto &[sat, sw] : {std::pair{level, 1 - up}, std::pair{level + 1, up}}) {
                    entry.table[slot] = sat == 0 ? nullptr : table(base + hue * 3 + sat - 1);
                    entry.weight[slot++] = float(bw * hw * sw);
                }
        }
        out.append(entry);
    }
    return out;
}
QVector<float> compose(const QVector<CameraRawStage> &stages, bool identity) {
    if (stages.isEmpty() && !identity)
        return {};
    QVector<float> out(Grid * Grid * Grid * 3);
    camera_raw_compose(out.data(), Grid, stages.constData(), int(stages.size()), Size);
    return out;
}
QJsonObject neutralize(double r, double g, double b) {
    if (std::min({r, g, b}) <= 1e-4)
        return {};
    auto cast = [&](double t, double tint) {
        auto stage = whiteBalance(t, tint);
        double color[]{r, g, b};
        camera_raw_stage_color(&stage, Size, color);
        return *std::max_element(color, color + 3) - *std::min_element(color, color + 3);
    };
    double temperature = 0, tint = 0, best = cast(0, 0);
    for (const auto &[step, reach] : {std::pair{10., 100.}, std::pair{2., 10.}, std::pair{.5, 2.}}) {
        double centerT = temperature, centerTint = tint;
        for (double t = centerT - reach; t <= centerT + reach; t += step)
            for (double i = centerTint - reach; i <= centerTint + reach; i += step) {
                if (std::abs(t) > 100 || std::abs(i) > 100)
                    continue;
                double found = cast(t, i);
                if (found < best) {
                    best = found;
                    temperature = t;
                    tint = i;
                }
            }
    }
    return {{"temperature", temperature}, {"tint", tint}};
}
} // namespace comp::rawtables
