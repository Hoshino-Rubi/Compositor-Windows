#include "GpuProcessing.h"
#ifdef Q_OS_WIN
#include <QFontDatabase>
#include <QFontMetrics>
#include <QPainter>
#include <cmath>
namespace comp {
GpuSurfacePtr GpuProcessor::dither(const GpuSurfacePtr &input, const QJsonObject &s) {
    const int style = std::clamp(s["style"].toInt(), 0, 9);
    if (style <= 1)
        return {}; // Atkinson and Floyd-Steinberg use the reference CPU path.
    const int block = style == 9 ? 1 : std::clamp(qRound(s["pixelSize"].toDouble(2)), 1, 32);
    const QSize reducedSize{std::max(1, (input->size.width() + block - 1) / block),
                            std::max(1, (input->size.height() + block - 1) / block)};
    Params resize{};
    resize[4] = float(input->size.width());
    resize[5] = float(input->size.height());
    auto pixels = pass("ditherResize", {input}, resize, reducedSize);
    if (!pixels)
        return {};
    Params p{};
    p[4] = float(style);
    p[5] = float(std::clamp(qRound(s["levels"].toDouble(2)), 2, 8));
    p[6] = std::exp2(float(std::clamp(s["density"].toDouble() / 100., -1., 1.) * 1.5));
    const float contrast = float(std::clamp(s["contrast"].toDouble() / 100., -1., 1.));
    p[7] = contrast >= 0 ? 1 / (1 - .95f * contrast) : 1 + contrast;
    p[8] = s["colors"].toInt() == 2 ? 1.f : 0.f;
    p[9] = s["lightOnDark"].toBool(true) ? 1.f : 0.f;
    p[10] = float(std::clamp(s["diffusion"].toDouble(100) / 100., 0., 1.));
    p[11] = float(std::clamp(qRound(s["cellSize"].toDouble(8)), 4, 64));
    const float angle = float(s["angle"].toDouble(45) * 3.141592653589793 / 180);
    p[12] = std::cos(angle);
    p[13] = std::sin(angle);
    QColor dark = s["colors"].toInt() == 1 ? QColor(s["dark"].toString("#000000")) : QColor(Qt::black);
    QColor light = s["colors"].toInt() == 1 ? QColor(s["light"].toString("#ffffff")) : QColor(Qt::white);
    if (!dark.isValid())
        dark = Qt::black;
    if (!light.isValid())
        light = Qt::white;
    p[16] = dark.red() / 255.f;
    p[17] = dark.green() / 255.f;
    p[18] = dark.blue() / 255.f;
    p[20] = light.red() / 255.f;
    p[21] = light.green() / 255.f;
    p[22] = light.blue() / 255.f;
    p[23] = float(block);
    p[24] = s["pixelShape"].toInt() == 1 ? 1.f : 0.f;
    p[25] = float(reducedSize.width());
    p[26] = float(reducedSize.height());
    auto tone = pass("ditherTone", {pixels}, p);
    if (!tone)
        return {};
    auto selected = tone, atlas = tone;
    if (style == 9) {
        // Only the font atlas/coverage table is CPU-generated. Cell analysis and glyph placement run on the GPU.
        QString chars = s["characters"].toString(" .:-=+*#%@");
        if (chars.isEmpty())
            chars = " .:-=+*#%@";
        chars = chars.left(64);
        const int gh = std::clamp(qRound(s["textSize"].toDouble(14)), 6, 64);
        QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        font.setPixelSize(std::max(5, gh - 2));
        font.setBold(true);
        const int gw = std::max(1, QFontMetrics(font).horizontalAdvance('M'));
        std::vector<std::pair<QByteArray, float>> glyphs;
        for (QChar ch : chars) {
            QImage glyph(gw, gh, QImage::Format_Grayscale8);
            glyph.fill(0);
            QPainter painter(&glyph);
            painter.setFont(font);
            painter.setPen(Qt::white);
            painter.drawText(glyph.rect(), Qt::AlignCenter, QString(ch));
            painter.end();
            QByteArray bytes;
            int ink = 0;
            for (int y = 0; y < gh; ++y)
                for (int x = 0; x < gw; ++x) {
                    const auto v = glyph.constScanLine(y)[x];
                    bytes.append(char(v));
                    ink += v;
                }
            glyphs.emplace_back(bytes, float(ink) / float(255 * gw * gh));
        }
        std::sort(glyphs.begin(), glyphs.end(), [](const auto &a, const auto &b) { return a.second < b.second; });
        QVector<float> values(int(glyphs.size()) * gw * gh * 4);
        for (int g = 0; g < int(glyphs.size()); ++g) {
            p[128 + g] = glyphs[g].second;
            for (int i = 0; i < gw * gh; ++i)
                values[(g * gw * gh + i) * 4] = quint8(glyphs[g].first[i]) / 255.f;
        }
        p[14] = float(gw);
        p[15] = float(gh);
        p[19] = float(glyphs.size());
        atlas = upload(values, {gw, int(glyphs.size()) * gh});
        selected = pass("ditherGlyphSelect", {tone}, p,
                        {(reducedSize.width() + gw - 1) / gw, (reducedSize.height() + gh - 1) / gh});
    }
    auto marks = pass("ditherMarks", {pixels, tone, selected, atlas}, p);
    return pass("ditherFinish", {marks, input}, p, input->size);
}
GpuSurfacePtr GpuProcessor::scanlines(const GpuSurfacePtr &input, const QJsonObject &s) {
    auto number = [&](const char *key, double fallback, double lo, double hi) {
        return std::clamp(s[key].toDouble(fallback), lo, hi);
    };
    Params p{};
    p[4] = float(qRound(number("lineSpacing", 4, 2, 32)));
    p[5] = float(number("thickness", 70, 5, 100) / 100);
    p[6] = float(number("dots", 0, 0, 100) / 100);
    p[7] = float(number("wobble", 0, 0, 64));
    p[8] = float(number("displace", 0, -100, 100));
    p[9] = float(number("smoothness", 50, 0, 100) / 100);
    p[10] = float(number("threshold", 0, 0, 100) / 100);
    p[11] = float(qRound(number("split", 0, 0, 16)));
    p[12] = std::exp2(float(number("density", 0, -100, 100) / 100 * 1.5));
    const float contrast = float(number("contrast", 0, -100, 100) / 100);
    p[13] = contrast >= 0 ? 1 / (1 - .95f * contrast) : 1 + contrast;
    p[14] = float(number("blackLevel", 0, 0, 100) / 100);
    p[15] = s["colors"].toInt() == 1 ? 1.f : 0.f;
    QColor dark = s["colors"].toInt() == 2 ? QColor(s["dark"].toString("#000000")) : QColor(Qt::black);
    QColor light = s["colors"].toInt() == 2 ? QColor(s["light"].toString("#ffffff")) : QColor(Qt::white);
    p[16] = dark.red() / 255.f;
    p[17] = dark.green() / 255.f;
    p[18] = dark.blue() / 255.f;
    p[20] = light.red() / 255.f;
    p[21] = light.green() / 255.f;
    p[22] = light.blue() / 255.f;
    const int lines = (input->size.height() + int(p[4]) - 1) / int(p[4]);
    p[25] = float(input->size.height());
    p[26] = float(lines);
    auto scan = pass("scanSample", {input}, p, {input->size.width(), lines});
    auto lift = scan;
    const int radius = qRound(p[9] * p[4] * 2);
    if (radius > 0 && p[8] != 0) {
        p[28] = float(radius);
        for (int i = 0; i < 3; ++i)
            lift = pass("scanSmooth", {lift}, p);
        p[29] = 1;
        for (int i = 0; i < qRound(p[9] * 2); ++i)
            lift = pass("scanSmooth", {lift}, p);
    }
    auto drawing = pass("scanDraw", {scan, lift}, p, input->size);
    auto out = pass("scanFinish", {drawing, input}, p);
    if (p[11] > 0)
        out = pass("scanSplit", {out}, p);
    const double glow = number("glow", 0, 0, 100);
    if (glow > 0) {
        auto lightPlane = blur(out, p[4] * 3 + 3);
        Params glowParams{};
        glowParams[3] = 2;
        glowParams[4] = float(glow / 100 * 2.5);
        out = pass("standaloneFilter", {out, lightPlane}, glowParams);
    }
    return out;
}
} // namespace comp
#endif
