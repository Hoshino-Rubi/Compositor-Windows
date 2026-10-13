#include "Document.h"
#include "Canvas.h"
#include "GpuHeal.h"
#include "GpuPaint.h"
#include "GpuPaintStroke.h"
#include "GpuPreview.h"
#include "Preferences.h"
#include "Pixels.h"
#include "WideKernels.h"

extern "C" {
#include "HealPixels.h"
}

#include <QApplication>
#include <QColorSpace>
#include <QElapsedTimer>
#include <QImageReader>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

using namespace comp;

namespace {

class ScopedEnvironment {
  public:
    ScopedEnvironment(const char *name, const QByteArray &value)
        : name_(name), old_(qgetenv(name)), had_(qEnvironmentVariableIsSet(name)) {
        qputenv(name_, value);
    }
    ~ScopedEnvironment() {
        if (had_)
            qputenv(name_, old_);
        else
            qunsetenv(name_);
    }

  private:
    QByteArray name_;
    QByteArray old_;
    bool had_ = false;
};

QImage::Format imageFormat(int depth) {
    return depth == 16 ? QImage::Format_RGBA64_Premultiplied : QImage::Format_ARGB32_Premultiplied;
}

QImage::Format coverageFormat(int depth) {
    return depth == 16 ? QImage::Format_Grayscale16 : QImage::Format_Grayscale8;
}

int maximumForDepth(int depth) {
    return depth == 16 ? 65535 : 255;
}

QImage paintFixture(QSize size, int depth) {
    QImage image(size, imageFormat(depth));
    if (image.isNull())
        return {};
    image.setColorSpace(QColorSpace::SRgb);
    image.setDotsPerMeterX(2835);
    image.setDotsPerMeterY(5669);
    const int maximum = maximumForDepth(depth);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            const int alpha = maximum / 5 + ((x * 997 + y * 313 + 37) % (maximum * 4 / 5 + 1));
            const int red = alpha == 0 ? 0 : (x * 313 + y * 17 + 23) % (alpha + 1);
            const int green = alpha == 0 ? 0 : (x * 71 + y * 541 + 41) % (alpha + 1);
            const int blue = alpha == 0 ? 0 : (x * 997 + y * 31 + 7) % (alpha + 1);
            if (depth == 16)
                reinterpret_cast<QRgba64 *>(image.scanLine(y))[x] = rgba64(red, green, blue, alpha);
            else
                reinterpret_cast<QRgb *>(image.scanLine(y))[x] = qRgba(red, green, blue, alpha);
        }
    }
    return image;
}

QImage coverageFixture(QSize size, int depth, bool soft) {
    QImage image(size, coverageFormat(depth));
    if (image.isNull())
        return {};
    const int maximum = maximumForDepth(depth);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            const double nx = (x + .5) / size.width() * 2. - 1.;
            const double ny = (y + .5) / size.height() * 2. - 1.;
            const double radius = std::sqrt(nx * nx + ny * ny);
            double value = soft ? std::clamp((.92 - radius) / .28, 0., 1.) : (radius <= .72 ? 1. : 0.);
            // A second edge keeps partial-alpha and integer quantisation exercised
            // even when a driver evaluates the radial fringe differently.
            if (soft && ((x * 7 + y * 11) % 19 == 0))
                value *= .67;
            const int sample = std::clamp(int(std::lround(value * maximum)), 0, maximum);
            if (depth == 16)
                reinterpret_cast<quint16 *>(image.scanLine(y))[x] = quint16(sample);
            else
                image.scanLine(y)[x] = uchar(sample);
        }
    }
    return image;
}

QImage selectionFixture(QSize size, int depth, QRect hole) {
    QImage image(size, coverageFormat(depth));
    if (image.isNull())
        return {};
    const int maximum = maximumForDepth(depth);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            int value = maximum;
            if (hole.contains(x, y))
                value = 0;
            else if (x >= hole.left() - 8 && x < hole.left() && y >= hole.top() && y <= hole.bottom())
                value = maximum * (x - hole.left() + 8) / 8;
            if (depth == 16)
                reinterpret_cast<quint16 *>(image.scanLine(y))[x] = quint16(value);
            else
                image.scanLine(y)[x] = uchar(value);
        }
    }
    return image;
}

double coverageValue(const QImage &image, int x, int y, int depth) {
    Q_UNUSED(depth);
    if (image.isNull() || x < 0 || y < 0 || x >= image.width() || y >= image.height())
        return 0.;
    if (image.format() == QImage::Format_Grayscale16)
        return reinterpret_cast<const quint16 *>(image.constScanLine(y))[x] / 65535.;
    return image.constScanLine(y)[x] / 255.;
}

QPoint sourcePoint(const QPointF &offset, int x, int y) {
    // The default paint route intentionally matches Qt's nearest sampler:
    // ties go to the lower pixel (ceil(p + .5) - 1), not bilinear filtering.
    const auto nearest = [](double p) { return int(std::ceil(p + .5) - 1.); };
    return QPoint(nearest(x + offset.x()), nearest(y + offset.y()));
}

void setPremultiplied(QImage &image, int x, int y, int depth, int r, int g, int b, int a) {
    if (depth == 16)
        reinterpret_cast<QRgba64 *>(image.scanLine(y))[x] = rgba64(r, g, b, a);
    else
        reinterpret_cast<QRgb *>(image.scanLine(y))[x] = qRgba(r, g, b, a);
}

void readPremultiplied(const QImage &image, int x, int y, int depth, int &r, int &g, int &b, int &a) {
    if (depth == 16) {
        const auto pixel = pixel64(image, x, y);
        r = pixel.red();
        g = pixel.green();
        b = pixel.blue();
        a = pixel.alpha();
    } else {
        const QRgb pixel = image.pixel(x, y);
        r = qRed(pixel);
        g = qGreen(pixel);
        b = qBlue(pixel);
        a = qAlpha(pixel);
    }
}

// CPU reference for the low-level API.  QPainter performs the same
// premultiplied SourceOver/DestinationOut operation as the editor, while the
// dab raster is explicitly quantised to the document's native depth.
QImage cpuPaint(const QImage &target, const QImage &source, const QVector<GpuPaintDab> &dabs,
                const GpuPaintOptions &options) {
    const int depth = options.depth == 16 || (options.depth == 0 && wideImage(target)) ? 16 : 8;
    QImage result = target.convertToFormat(imageFormat(depth));
    const QImage original = result;
    const QImage sourceImage = source.isNull() ? QImage() : source.convertToFormat(imageFormat(depth));
    const QImage selection = options.selectionMask.isNull() ? QImage()
                                                              : options.selectionMask.convertToFormat(coverageFormat(depth));
    QColor color = options.color;
    if (!color.isValid())
        color = Qt::black;
    const int maximum = maximumForDepth(depth);
    const double flow = std::clamp(options.flow, 0., 1.);
    for (const auto &dab : dabs) {
        if (dab.coverage.isNull() || dab.targetRect.size() != dab.coverage.size())
            continue;
        QImage overlay(dab.coverage.size(), imageFormat(depth));
        overlay.fill(Qt::transparent);
        for (int y = 0; y < overlay.height(); ++y) {
            for (int x = 0; x < overlay.width(); ++x) {
                const int gx = dab.targetRect.left() + x;
                const int gy = dab.targetRect.top() + y;
                double factor = coverageValue(dab.coverage, x, y, depth) * flow;
                factor = std::clamp(factor, 0., 1.);
                int r = 0, g = 0, b = 0, a = 0;
                if (options.operation == GpuPaintOperation::Brush) {
                    a = int(std::lround(color.alphaF() * factor * maximum));
                    r = int(std::lround(color.redF() * a));
                    g = int(std::lround(color.greenF() * a));
                    b = int(std::lround(color.blueF() * a));
                } else if (options.operation == GpuPaintOperation::Clone ||
                           options.operation == GpuPaintOperation::Blur) {
                    const QPoint sample = sourcePoint(dab.sourceOffset, gx, gy);
                    if (!sourceImage.isNull() && sourceImage.rect().contains(sample))
                        readPremultiplied(sourceImage, sample.x(), sample.y(), depth, r, g, b, a);
                    r = int(std::lround(r * factor));
                    g = int(std::lround(g * factor));
                    b = int(std::lround(b * factor));
                    a = int(std::lround(a * factor));
                } else { // DestinationOut only uses the alpha of the overlay.
                    a = int(std::lround(factor * maximum));
                    r = g = b = a;
                }
                setPremultiplied(overlay, x, y, depth, r, g, b, a);
            }
        }
        QPainter painter(&result);
        painter.setCompositionMode(options.operation == GpuPaintOperation::Eraser
                                       ? QPainter::CompositionMode_DestinationOut
                                       : QPainter::CompositionMode_SourceOver);
        painter.drawImage(dab.targetRect.topLeft(), overlay);
        painter.end();
    }
    // GpuPaintOptions::selectionMask is a final per-output constraint.  The
    // Canvas path likewise paints the complete ordered batch and constrains
    // once, rather than multiplying every dab (which would compound feather
    // coverage on overlapping dabs).
    if (!selection.isNull()) {
        for (int y = 0; y < result.height(); ++y) {
            for (int x = 0; x < result.width(); ++x) {
                const double factor = std::clamp(coverageValue(selection, x, y, depth), 0., 1.);
                if (factor >= 1.)
                    continue;
                int orr = 0, org = 0, orb = 0, ora = 0, rr = 0, rg = 0, rb = 0, ra = 0;
                readPremultiplied(original, x, y, depth, orr, org, orb, ora);
                readPremultiplied(result, x, y, depth, rr, rg, rb, ra);
                const auto mix = [factor](int before, int after) {
                    return int(std::lround(before + (after - before) * factor));
                };
                setPremultiplied(result, x, y, depth, mix(orr, rr), mix(org, rg), mix(orb, rb), mix(ora, ra));
            }
        }
    }
    result = result.convertToFormat(target.format());
    result.setColorSpace(target.colorSpace());
    result.setDotsPerMeterX(target.dotsPerMeterX());
    result.setDotsPerMeterY(target.dotsPerMeterY());
    return result;
}

struct Difference {
    int maximum = 0;
    double mean = 0.;
    quint64 samples = 0;
    QPoint worstPosition{-1, -1};
    std::array<int, 4> actualAtWorst{};
    std::array<int, 4> expectedAtWorst{};
    int worstChannel = -1;
};

Difference difference(const QImage &actual, const QImage &expected, int depth) {
    Difference out;
    if (actual.isNull() || expected.isNull() || actual.size() != expected.size()) {
        out.maximum = maximumForDepth(depth);
        out.mean = double(out.maximum);
        return out;
    }
    for (int y = 0; y < actual.height(); ++y) {
        for (int x = 0; x < actual.width(); ++x) {
            int ar = 0, ag = 0, ab = 0, aa = 0, er = 0, eg = 0, eb = 0, ea = 0;
            readPremultiplied(actual, x, y, depth, ar, ag, ab, aa);
            readPremultiplied(expected, x, y, depth, er, eg, eb, ea);
            const int actualValues[] = {ar, ag, ab, aa};
            const int expectedValues[] = {er, eg, eb, ea};
            for (int channel = 0; channel < 4; ++channel) {
                const int delta = std::abs(actualValues[channel] - expectedValues[channel]);
                out.maximum = std::max(out.maximum, delta);
                out.mean += delta;
                ++out.samples;
                if (delta == out.maximum) {
                    out.worstPosition = QPoint(x, y);
                    out.actualAtWorst = {ar, ag, ab, aa};
                    out.expectedAtWorst = {er, eg, eb, ea};
                    out.worstChannel = channel;
                }
            }
        }
    }
    if (out.samples)
        out.mean /= double(out.samples);
    return out;
}

qint64 median(QVector<qint64> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

bool metadataEqual(const QImage &left, const QImage &right) {
    return left.colorSpace() == right.colorSpace() && left.dotsPerMeterX() == right.dotsPerMeterX() &&
           left.dotsPerMeterY() == right.dotsPerMeterY();
}

bool samePixel(const QImage &left, const QImage &right, int x, int y, int depth) {
    int lr = 0, lg = 0, lb = 0, la = 0, rr = 0, rg = 0, rb = 0, ra = 0;
    readPremultiplied(left, x, y, depth, lr, lg, lb, la);
    readPremultiplied(right, x, y, depth, rr, rg, rb, ra);
    return lr == rr && lg == rg && lb == rb && la == ra;
}

bool outsideExact(const QImage &actual, const QImage &source, const QRect &roi, int depth) {
    if (actual.size() != source.size())
        return false;
    for (int y = 0; y < actual.height(); ++y)
        for (int x = 0; x < actual.width(); ++x)
            if (!roi.contains(x, y) && !samePixel(actual, source, x, y, depth))
                return false;
    return true;
}

QRect dabsRoi(const QVector<GpuPaintDab> &dabs, QSize bounds) {
    QRect roi;
    for (const auto &dab : dabs) {
        const QRect clipped = dab.targetRect.intersected(QRect(QPoint(), bounds));
        if (!clipped.isEmpty())
            roi = roi.isEmpty() ? clipped : roi.united(clipped);
    }
    return roi;
}

QImage cpuCanvasCloneStroke(const QImage &target, const QImage &source, QPointF from, QPointF to,
                            double brushSize, double flow, const QTransform &documentToPixel,
                            QPointF cloneOffset) {
    QImage result = target;
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    // Canvas installs placement^-1 first, then concatenates placement while
    // drawing the source layer. Supplying the same pair gives an actual
    // QPainter reference for the wrapper's document-to-pixel transform.
    painter.setTransform(documentToPixel);
    const QTransform matrix = documentToPixel.inverted();
    const double distance = QLineF(from, to).length();
    const int steps = std::max(1, int(distance / std::max(1., brushSize * .15)));
    for (int step = 0; step <= steps; ++step) {
        const QPointF point = from + (to - from) * (double(step) / steps);
        QPainterPath tip;
        tip.addEllipse(point, brushSize / 2., brushSize / 2.);
        painter.save();
        painter.setClipPath(tip, Qt::IntersectClip);
        painter.setOpacity(flow);
        painter.translate(-cloneOffset);
        painter.setTransform(matrix, true);
        painter.drawImage(0, 0, source);
        painter.restore();
    }
    painter.end();
    result.setColorSpace(target.colorSpace());
    result.setDotsPerMeterX(target.dotsPerMeterX());
    result.setDotsPerMeterY(target.dotsPerMeterY());
    return result;
}

QVector<GpuPaintDab> paintDabs(QSize size, int depth, bool soft, bool batch) {
    const QSize coverageSize(batch ? QSize(27, 23) : QSize(31, 27));
    const QImage coverage = coverageFixture(coverageSize, depth, soft);
    QVector<GpuPaintDab> dabs;
    const int count = batch ? 7 : 1;
    dabs.reserve(count);
    for (int i = 0; i < count; ++i) {
        const int x = batch ? 13 + i * 17 : size.width() / 2 - coverageSize.width() / 2;
        const int y = batch ? 15 + (i % 3) * 14 : size.height() / 2 - coverageSize.height() / 2;
        GpuPaintDab dab;
        dab.targetRect = QRect(x, y, coverageSize.width(), coverageSize.height());
        dab.coverage = coverage;
        dab.sourceOffset = QPointF(3, -2);
        dabs.append(std::move(dab));
    }
    return dabs;
}

bool gpuResultUsable(const QImage &result, const QString &label, QString *error = nullptr) {
    const auto metrics = gpuPreviewMetrics();
    if (!result.isNull() && metrics.processingDispatches > 0 && metrics.readbacks > 0)
        return true;
    const QString message = QString("%1 did not execute native GPU: result=%2 dispatches=%3 readbacks=%4 status=%5")
                                .arg(label)
                                .arg(!result.isNull())
                                .arg(metrics.processingDispatches)
                                .arg(metrics.readbacks)
                                .arg(gpuPreviewStatus());
    if (error)
        *error = message;
    return false;
}

void cpuHeal(QImage &image, const QImage &coverage, float opacity, int mode, uint32_t seed, int depth) {
    const QImage mask = coverage.convertToFormat(QImage::Format_Grayscale8);
    // spot_heal's C ABI deliberately takes a tightly packed width*height
    // coverage plane (unlike QImage's 32-bit-aligned scanlines).
    QByteArray packed(mask.width() * mask.height(), '\0');
    for (int y = 0; y < mask.height(); ++y)
        std::memcpy(packed.data() + y * mask.width(), mask.constScanLine(y), size_t(mask.width()));
    const auto *coverageBytes = reinterpret_cast<const uint8_t *>(packed.constData());
    if (depth == 16) {
        wide::spot_heal(reinterpret_cast<quint16 *>(image.bits()), coverageBytes, size_t(image.width()),
                        size_t(image.height()), size_t(image.bytesPerLine() / 2), opacity, mode, seed);
    } else {
        spot_heal(image.bits(), coverageBytes, size_t(image.width()), size_t(image.height()),
                  size_t(image.bytesPerLine()), opacity, mode, seed);
    }
}

QImage healFixture(QSize size, int depth) {
    // healGpuImage deliberately accepts the byte-order-stable RGBA formats;
    // the editor's ordinary 8-bit fixture uses Qt's ARGB32 convenience format.
    QImage image = paintFixture(size, depth).convertToFormat(
        depth == 16 ? QImage::Format_RGBA64_Premultiplied : QImage::Format_RGBA8888_Premultiplied);
    // A broad translucent band plus a fully transparent corner makes alpha and
    // premultiplied clamping observable in all three healing modes.
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            int r = 0, g = 0, b = 0, a = 0;
            readPremultiplied(image, x, y, depth, r, g, b, a);
            if ((x + y * 3) % 23 == 0)
                a = std::max(0, a / 3);
            setPremultiplied(image, x, y, depth, std::min(r, a), std::min(g, a), std::min(b, a), a);
        }
    }
    return image;
}

QImage healCoverage(QSize size, int depth, bool partial) {
    Q_UNUSED(depth);
    // The healing bridge intentionally accepts a compact 8-bit coverage mask
    // for both native image depths; wide::spot_heal uses the same mask.
    QImage coverage(size, QImage::Format_Grayscale8);
    coverage.fill(0);
    const int maximum = 255;
    const QRect patch(size.width() / 3, size.height() / 3, size.width() / 4, size.height() / 3);
    for (int y = patch.top(); y <= patch.bottom() && y < size.height(); ++y) {
        for (int x = patch.left(); x <= patch.right() && x < size.width(); ++x) {
            const int value = partial ? maximum * (1 + ((x * 13 + y * 7) % 5)) / 5 : maximum;
            coverage.scanLine(y)[x] = uchar(value);
        }
    }
    return coverage;
}

} // namespace

class GpuEditTests : public QObject {
    Q_OBJECT

  private slots:
    void gpuPaintSingleAndBatchAgainstQPainter() {
        const QSize size(127, 101);
        const QColor ink = QColor::fromRgbF(.83F, .19F, .07F, .71F);
        for (const int depth : {8, 16}) {
            const QImage source = paintFixture(size, depth);
            const QImage selection = selectionFixture(size, depth, QRect(54, 39, 22, 21));
            for (const bool soft : {false, true}) {
                for (const bool batch : {false, true}) {
                    const auto dabs = paintDabs(size, depth, soft, batch);
                    const QRect roi = dabsRoi(dabs, size);
                    for (const auto operation : {GpuPaintOperation::Brush, GpuPaintOperation::Eraser,
                                                 GpuPaintOperation::Clone, GpuPaintOperation::Blur}) {
                        GpuPaintOptions options;
                        options.operation = operation;
                        options.color = ink;
                        options.flow = .73;
                        options.depth = depth;
                        options.selectionMask = selection;
                        const QImage operationSource = source;
                        const QImage expected = cpuPaint(source, operationSource, dabs, options);
                        resetGpuPreviewMetrics();
                        const QImage actual = paintGpuImage(source, operationSource, dabs, options);
                        QString failure;
                        if (!gpuResultUsable(actual, QString("paint depth=%1 soft=%2 batch=%3 op=%4")
                                                       .arg(depth)
                                                       .arg(soft)
                                                       .arg(batch)
                                                       .arg(int(operation)),
                                                   &failure)) {
                            if (qEnvironmentVariableIntValue("COMPOSITOR_REQUIRE_GPU"))
                                QFAIL(qPrintable(failure));
                            QSKIP(qPrintable(failure));
                        }
                        QCOMPARE(actual.size(), source.size());
                        QVERIFY2(metadataEqual(actual, source), "GPU paint changed image metadata");
                        const auto delta = difference(actual, expected, depth);
                        qInfo() << "PAINT_REFERENCE QPainter" << "depth" << depth << "soft" << soft << "batch" << batch
                                << "operation" << int(operation) << "maxNativeDelta" << delta.maximum << "meanNativeDelta"
                                << delta.mean << "dispatches" << gpuPreviewMetrics().processingDispatches << "readbacks"
                                << gpuPreviewMetrics().readbacks;
                        QVERIFY2(delta.maximum <= (depth == 8 ? 2 : 32),
                                 qPrintable(QString("paint parity depth=%1 soft=%2 batch=%3 operation=%4 max=%5")
                                                .arg(depth)
                                                .arg(soft)
                                                .arg(batch)
                                                .arg(int(operation))
                                                .arg(delta.maximum)));
                        QVERIFY2(outsideExact(actual, source, roi, depth), "GPU paint changed pixels outside the ROI");
                        const QRect hole(54, 39, 22, 21);
                        for (int y = hole.top(); y <= hole.bottom(); ++y)
                            for (int x = hole.left(); x <= hole.right(); ++x)
                                QVERIFY2(samePixel(actual, source, x, y, depth), "zero selection coverage was modified");
                    }
                }
            }
        }
    }

    void paintStrokeCoverageSelectionAndUndo() {
        // The wrapper intentionally refuses tiny strokes; this medium fixture
        // exercises its ordered coverage atlas and one-dispatch ROI path.
        for (const int depth : {8, 16}) {
            const QSize size(512, 256);
            const QImage before = paintFixture(size, depth);
            QImage actual = before;
            QPainterPath selection;
            selection.addRect(QRectF(0, 0, size.width(), size.height()));
            selection.addRect(QRectF(220, 78, 74, 62));
            selection.setFillRule(Qt::OddEvenFill);
            resetGpuPreviewMetrics();
            const bool accelerated = paintStrokeAccelerated(actual, QPointF(64, 128), QPointF(444, 128), 144, .36, .69,
                                                            QColor::fromRgbF(.68F, .31F, .12F, .81F), false, false,
                                                            QTransform(), selection);
            const auto metrics = gpuPreviewMetrics();
            if (!accelerated || metrics.processingDispatches == 0 || metrics.readbacks == 0) {
                const QString message = QString("paintStroke depth=%1 used fallback: accelerated=%2 dispatches=%3 readbacks=%4")
                                            .arg(depth)
                                            .arg(accelerated)
                                            .arg(metrics.processingDispatches)
                                            .arg(metrics.readbacks);
                if (qEnvironmentVariableIntValue("COMPOSITOR_REQUIRE_GPU"))
                    QFAIL(qPrintable(message));
                QSKIP(qPrintable(message));
            }
            QVERIFY2(actual.size() == before.size(), "paintStroke changed image dimensions");
            QVERIFY2(metadataEqual(actual, before), "paintStroke changed image metadata");
            QVERIFY2(actual != before, "paintStroke produced no pixel edits");
            const QRect roi(0, 55, size.width(), 146);
            QVERIFY2(outsideExact(actual, before, roi, depth), "paintStroke changed pixels outside its ROI");
            for (int y = 78; y < 140; ++y)
                for (int x = 220; x < 294; ++x)
                    QVERIFY2(samePixel(actual, before, x, y, depth), "path-selection hole was modified");

            State state;
            state.id = newId();
            state.size = size;
            state.bitDepth = depth;
            state.selectionMask = selectionFixture(size, depth, QRect(220, 78, 74, 62));
            state.selectionActive = true;
            Layer layer;
            layer.id = newId();
            layer.name = "GPU paint";
            layer.image = before;
            layer.placement.size = size;
            state.layers.append(layer);
            state.active = layer.id;
            Document document;
            document.reset(state, true);
            const State original = document.state();
            document.edit([&](State &edited) { edited.layers[0].image = actual; });
            QVERIFY(document.canUndo());
            document.undo();
            QCOMPARE(document.state().layers[0].image, original.layers[0].image);
            QCOMPARE(document.state().selectionMask, original.selectionMask);
            QCOMPARE(document.state().selectionActive, original.selectionActive);
            QVERIFY(!document.canUndo());
        }
    }

    void fractionalCloneWrapperMatchesCanvasPainter() {
        // Use a placement-like affine transform with both rotation and
        // non-uniform scale. The stroke endpoints are chosen in document
        // space by mapping target pixels backwards through that placement,
        // so the transformed ROI remains comfortably inside the layer.
        const QSize size(768, 512);
        for (const int depth : {8, 16}) {
            const QImage before = paintFixture(size, depth);
            const QImage source = paintFixture(size, depth).mirrored(true, false);
            QTransform documentToPixel;
            documentToPixel.translate(47.35, 31.7);
            documentToPixel.rotate(13.5);
            documentToPixel.scale(1.23, .77);
            const QTransform pixelToDocument = documentToPixel.inverted();
            const QPointF from = pixelToDocument.map(QPointF(138.25, 226.4));
            const QPointF to = pixelToDocument.map(QPointF(638.6, 282.8));
            const QPointF cloneOffset(4.35, -2.65);
            const double brushSize = 118.;
            const double flow = .63;
            const QImage expected = cpuCanvasCloneStroke(before, source, from, to, brushSize, flow,
                                                          documentToPixel, cloneOffset);

            QImage actual = before;
            resetGpuPreviewMetrics();
            const bool accelerated = paintStrokeAccelerated(actual, from, to, brushSize, .99, flow,
                                                            QColor::fromRgbF(.1F, .2F, .3F, 1.F), false, false,
                                                            documentToPixel, {}, source, cloneOffset, true);
            const auto metrics = gpuPreviewMetrics();
            if (!accelerated || metrics.processingDispatches == 0 || metrics.readbacks == 0) {
                const QString message = QString("fractional clone depth=%1 used fallback: accelerated=%2 dispatches=%3 readbacks=%4")
                                            .arg(depth)
                                            .arg(accelerated)
                                            .arg(metrics.processingDispatches)
                                            .arg(metrics.readbacks);
                if (qEnvironmentVariableIntValue("COMPOSITOR_REQUIRE_GPU"))
                    QFAIL(qPrintable(message));
                QSKIP(qPrintable(message));
            }
            QPainterPath stroke;
            stroke.moveTo(from);
            stroke.lineTo(to);
            QPainterPathStroker stroker;
            stroker.setWidth(brushSize);
            stroker.setCapStyle(Qt::RoundCap);
            const QRect roi = documentToPixel.map(stroker.createStroke(stroke)).boundingRect().toAlignedRect()
                                  .adjusted(-2, -2, 2, 2)
                                  .intersected(before.rect());
            const auto delta = difference(actual, expected, depth);
            const bool worstOutsideRoi = !roi.contains(delta.worstPosition);
            const bool worstAtRoiEdge = !worstOutsideRoi &&
                                        (delta.worstPosition.x() <= roi.left() + 1 ||
                                         delta.worstPosition.x() >= roi.right() - 1 ||
                                         delta.worstPosition.y() <= roi.top() + 1 ||
                                         delta.worstPosition.y() >= roi.bottom() - 1);
            qInfo() << "PAINT_REFERENCE Canvas QPainter clone" << "depth" << depth << "rotation" << 13.5
                    << "scale" << QPointF(1.23, .77) << "cloneOffset" << cloneOffset << "maxNativeDelta"
                    << delta.maximum << "meanNativeDelta" << delta.mean << "worstPosition" << delta.worstPosition
                    << "worstChannel" << delta.worstChannel << "actualAtWorst" << delta.actualAtWorst[0]
                    << delta.actualAtWorst[1] << delta.actualAtWorst[2] << delta.actualAtWorst[3]
                    << "expectedAtWorst" << delta.expectedAtWorst[0] << delta.expectedAtWorst[1]
                    << delta.expectedAtWorst[2] << delta.expectedAtWorst[3] << "roi" << roi
                    << "worstOutsideRoi" << worstOutsideRoi << "worstAtRoiEdge" << worstAtRoiEdge
                    << "dispatches" << metrics.processingDispatches << "readbacks" << metrics.readbacks;
            QVERIFY2(delta.maximum <= (depth == 8 ? 2 : 8),
                     qPrintable(QString("fractional clone parity depth=%1 max=%2 mean=%3")
                                    .arg(depth)
                                    .arg(delta.maximum)
                                    .arg(delta.mean)));
            QVERIFY2(delta.mean <= (depth == 8 ? .1 : 1.),
                     qPrintable(QString("fractional clone mean disagreement depth=%1 max=%2 mean=%3")
                                    .arg(depth)
                                    .arg(delta.maximum)
                                    .arg(delta.mean)));
            QVERIFY2(actual != before, "fractional clone produced no pixel edits");
            QVERIFY2(outsideExact(actual, before, roi, depth), "fractional clone changed pixels outside transformed ROI");
            QVERIFY(metadataEqual(actual, before));
        }
    }

    void paintMaskTargetAndInvalidFallback() {
        const QSize size(512, 256);
        const QImage before = paintFixture(size, 16);
        QImage target = before;
        resetGpuPreviewMetrics();
        const bool accelerated = paintStrokeAccelerated(target, QPointF(64, 128), QPointF(444, 128), 144, .98, .8,
                                                        QColor::fromRgbF(.2F, .7F, .3F, .73F), false, true, QTransform(),
                                                        {});
        const auto metrics = gpuPreviewMetrics();
        if (!accelerated || metrics.processingDispatches == 0 || metrics.readbacks == 0) {
            const QString message = QString("16-bit mask target used fallback: accelerated=%1 dispatches=%2 readbacks=%3")
                                        .arg(accelerated)
                                        .arg(metrics.processingDispatches)
                                        .arg(metrics.readbacks);
            if (qEnvironmentVariableIntValue("COMPOSITOR_REQUIRE_GPU"))
                QFAIL(qPrintable(message));
            QSKIP(qPrintable(message));
        }
        QVERIFY(wideImage(target));
        QVERIFY(target != before);

        // Invalid input and an explicit disable must return before any native
        // callback; a CPU fallback is not allowed to inflate GPU metrics.
        GpuPaintDab invalid;
        invalid.targetRect = QRect(1, 1, 4, 4);
        GpuPaintOptions options;
        options.depth = 8;
        resetGpuPreviewMetrics();
        QVERIFY(paintGpuImage(before.convertToFormat(imageFormat(8)), {}, {invalid}, options).isNull());
        QCOMPARE(gpuPreviewMetrics().processingDispatches, quint64(0));
        QCOMPARE(gpuPreviewMetrics().readbacks, quint64(0));
        {
            ScopedEnvironment disabled("COMPOSITOR_DISABLE_GPU", "1");
            resetGpuPreviewMetrics();
            QVERIFY(paintGpuImage(before, {}, {}, options).isNull());
            QCOMPARE(gpuPreviewMetrics().processingDispatches, quint64(0));
            QCOMPARE(gpuPreviewMetrics().readbacks, quint64(0));
        }
    }

    void gpuEditingPreferenceGatesCanvasAndPreservesUndo() {
        QSettings settings;
        settings.remove("rendering/gpuEditingExperimental");
        settings.setValue("preferences/gridSpacing", 731);
        settings.sync();
        QVERIFY(!Preferences::gpuEditingEnabled());
        QVERIFY(!Preferences::load().gpuEditingExperimental);

        settings.setValue("rendering/gpuEditingExperimental", true);
        settings.sync();
        QVERIFY(Preferences::gpuEditingEnabled());
        Preferences saved = Preferences::load();
        QVERIFY(saved.gpuEditingExperimental);
        saved.gpuEditingExperimental = false;
        saved.save();
        QVERIFY(!Preferences::gpuEditingEnabled());
        saved.gpuEditingExperimental = true;
        saved.save();
        QVERIFY(Preferences::gpuEditingEnabled());
        QCOMPARE(QSettings().value("preferences/gridSpacing").toInt(), 731);

        const QSize size(600, 256);
        State state;
        state.id = newId();
        state.size = size;
        state.bitDepth = 8;
        Layer layer;
        layer.id = newId();
        layer.name = "GPU preference gate";
        layer.image = paintFixture(size, 8);
        layer.placement.size = size;
        state.layers.append(layer);
        state.active = layer.id;
        Document document;
        document.reset(state, true);
        const QImage original = document.state().layers[0].image;

        Canvas canvas(&document);
        canvas.resize(696, 352);
        canvas.setTool(Tool::Brush);
        canvas.setBrushSize(256);
        canvas.setHardness(1.);
        canvas.setFlow(.63);
        canvas.setColor(QColor::fromRgbF(.1F, .2F, .3F, 1.F));
        canvas.show();
        canvas.fit();
        QCoreApplication::processEvents();
        const auto stroke = [&] {
            const QPoint from = canvas.viewPoint(QPointF(180, 128)).toPoint();
            const QPoint to = canvas.viewPoint(QPointF(412, 128)).toPoint();
            QTest::mousePress(&canvas, Qt::LeftButton, Qt::NoModifier, from);
            QTest::mouseMove(&canvas, to);
            QTest::mouseRelease(&canvas, Qt::LeftButton, Qt::NoModifier, to);
        };

        settings.setValue("rendering/gpuEditingExperimental", false);
        settings.sync();
        QVERIFY(!Preferences::gpuEditingEnabled());
        resetGpuPreviewMetrics();
        stroke();
        const auto cpuMetrics = gpuPreviewMetrics();
        QCOMPARE(cpuMetrics.processingDispatches, quint64(0));
        QCOMPARE(cpuMetrics.readbacks, quint64(0));
        QVERIFY(document.canUndo());
        QVERIFY(document.state().layers[0].image != original);
        document.undo();
        QCOMPARE(document.state().layers[0].image, original);
        QVERIFY(!document.canUndo());

        saved = Preferences::load();
        saved.gpuEditingExperimental = true;
        saved.save();
        QVERIFY(Preferences::gpuEditingEnabled());
        canvas.fit();
        QCoreApplication::processEvents();
        resetGpuPreviewMetrics();
        stroke();
        const auto gpuMetrics = gpuPreviewMetrics();
        if (gpuMetrics.processingDispatches == 0 || gpuMetrics.readbacks == 0) {
            const QString message = QString("Canvas GPU preference did not dispatch: dispatches=%1 readbacks=%2")
                                        .arg(gpuMetrics.processingDispatches)
                                        .arg(gpuMetrics.readbacks);
            if (qEnvironmentVariableIntValue("COMPOSITOR_REQUIRE_GPU"))
                QFAIL(qPrintable(message));
            QSKIP(qPrintable(message));
        }
        QVERIFY(document.canUndo());
        QVERIFY(document.state().layers[0].image != original);
        document.undo();
        QCOMPARE(document.state().layers[0].image, original);
        QVERIFY(!document.canUndo());
        QCOMPARE(QSettings().value("preferences/gridSpacing").toInt(), 731);
    }

    void gpuHealThreeModesAndAlphaMask() {
        const QSize size(113, 97);
        for (const int depth : {8, 16}) {
            const QImage source = healFixture(size, depth);
            const QImage coverage = healCoverage(size, depth, true);
            QImage zeroCoverage(size, QImage::Format_Grayscale8);
            zeroCoverage.fill(0);
            for (const int mode : {0, 1, 2}) {
                QImage expected = source;
                cpuHeal(expected, coverage, .67f, mode, 0x1234u, depth);
                resetGpuPreviewMetrics();
                const QImage actual = healGpuImage(source, coverage, .67f, mode, 0x1234u);
                QString failure;
                if (!gpuResultUsable(actual, QString("heal depth=%1 mode=%2").arg(depth).arg(mode), &failure)) {
                    if (qEnvironmentVariableIntValue("COMPOSITOR_REQUIRE_GPU"))
                        QFAIL(qPrintable(failure));
                    QSKIP(qPrintable(failure));
                }
                const auto delta = difference(actual, expected, depth);
                qInfo() << "HEAL_REFERENCE spot_heal/wide::spot_heal" << "depth" << depth << "mode" << mode
                        << "maxNativeDelta" << delta.maximum << "meanNativeDelta" << delta.mean << "dispatches"
                        << gpuPreviewMetrics().processingDispatches << "readbacks" << gpuPreviewMetrics().readbacks
                        << "algorithmOrder" << "GPU dispatch then alpha clamp vs CPU membrane then blend";
                // Keep this bound intentionally conservative: it catches a
                // wrong algorithm/order while allowing native integer rounding.
                QVERIFY2(delta.maximum <= (depth == 8 ? 2 : 8),
                         qPrintable(QString("heal large disagreement depth=%1 mode=%2 max=%3 mean=%4")
                                        .arg(depth)
                                        .arg(mode)
                                        .arg(delta.maximum)
                                        .arg(delta.mean)));
                QVERIFY2(delta.mean <= (depth == 8 ? .1 : 1.),
                         qPrintable(QString("heal mean disagreement depth=%1 mode=%2 max=%3 mean=%4")
                                        .arg(depth)
                                        .arg(mode)
                                        .arg(delta.maximum)
                                        .arg(delta.mean)));
                for (int y = 0; y < size.height(); ++y)
                    for (int x = 0; x < size.width(); ++x)
                        if (coverageValue(coverage, x, y, depth) == 0.)
                            QVERIFY2(samePixel(actual, source, x, y, depth), "healing changed pixels outside the mask");
                QCOMPARE(actual.size(), source.size());
                QVERIFY(metadataEqual(actual, source));
            }

            resetGpuPreviewMetrics();
            const QImage noOp = healGpuImage(source, zeroCoverage, .67f, 0, 0x1234u);
            const auto noOpMetrics = gpuPreviewMetrics();
            if (!noOp.isNull())
                QCOMPARE(noOp, source);
            QCOMPARE(noOpMetrics.processingDispatches, quint64(0));
            QCOMPARE(noOpMetrics.readbacks, quint64(0));
        }
    }

    void healFallbackDoesNotReportGpu() {
        const QImage source = healFixture({64, 48}, 8);
        const QImage coverage = healCoverage(source.size(), 8, false);
        {
            ScopedEnvironment disabled("COMPOSITOR_DISABLE_GPU", "1");
            resetGpuPreviewMetrics();
            QVERIFY(healGpuImage(source, coverage, 1.f, 0, 17).isNull());
            QCOMPARE(gpuPreviewMetrics().processingDispatches, quint64(0));
            QCOMPARE(gpuPreviewMetrics().readbacks, quint64(0));
        }
        resetGpuPreviewMetrics();
        QVERIFY(healGpuImage(source, QImage(QSize(1, 1), QImage::Format_Grayscale8), 1.f, 0, 17).isNull());
        QCOMPARE(gpuPreviewMetrics().processingDispatches, quint64(0));
        QCOMPARE(gpuPreviewMetrics().readbacks, quint64(0));
    }

    void gpuPaintBenchmarkWhenRequested() {
        if (!qEnvironmentVariableIntValue("COMPOSITOR_GPU_BENCHMARK"))
            QSKIP("set COMPOSITOR_GPU_BENCHMARK=1 to run the opt-in benchmark");
        const QSize size(384, 320);
        for (const int depth : {8, 16}) {
            const QImage source = paintFixture(size, depth);
            const auto dabs = paintDabs(size, depth, true, true);
            GpuPaintOptions paintOptions;
            paintOptions.operation = GpuPaintOperation::Brush;
            paintOptions.color = QColor::fromRgbF(.75F, .26F, .12F, .72F);
            paintOptions.flow = .68;
            paintOptions.depth = depth;

            // Build the reference before timing.  The timed CPU path is the
            // QPainter/coverage implementation itself, not construction of a
            // second expected image for every sample.
            const QImage expected = cpuPaint(source, {}, dabs, paintOptions);

            // Compile/upload the complete core route before collecting the
            // three samples; otherwise the first sample measures shader
            // creation rather than the edit.
            resetGpuPreviewMetrics();
            const QImage warmPaint = paintGpuImage(source, {}, dabs, paintOptions);
            const auto warmMetrics = gpuPreviewMetrics();
            QString failure;
            if (!gpuResultUsable(warmPaint, QString("benchmark paint depth=%1").arg(depth), &failure)) {
                if (qEnvironmentVariableIntValue("COMPOSITOR_REQUIRE_GPU"))
                    QFAIL(qPrintable(failure));
                QSKIP(qPrintable(failure));
            }
            const auto warmDelta = difference(warmPaint, expected, depth);
            QVERIFY2(warmDelta.maximum <= (depth == 8 ? 2 : 32),
                     qPrintable(QString("benchmark warm paint depth=%1 max=%2 mean=%3")
                                    .arg(depth)
                                    .arg(warmDelta.maximum)
                                    .arg(warmDelta.mean)));

            QVector<qint64> gpuTimes;
            QVector<qint64> cpuTimes;
            quint64 dispatches = 0;
            quint64 readbacks = 0;
            QImage lastGpuPaint;
            QElapsedTimer timer;
            for (int repeat = 0; repeat < 3; ++repeat) {
                resetGpuPreviewMetrics();
                timer.start();
                lastGpuPaint = paintGpuImage(source, {}, dabs, paintOptions);
                gpuTimes.append(timer.nsecsElapsed());
                const auto metrics = gpuPreviewMetrics();
                QVERIFY2(gpuResultUsable(lastGpuPaint, QString("benchmark paint sample depth=%1").arg(depth)),
                         qPrintable(QString("benchmark paint sample depth=%1 used CPU fallback")
                                        .arg(depth)));
                dispatches += metrics.processingDispatches;
                readbacks += metrics.readbacks;
                const auto delta = difference(lastGpuPaint, expected, depth);
                QVERIFY2(delta.maximum <= (depth == 8 ? 2 : 32),
                         qPrintable(QString("benchmark paint sample depth=%1 max=%2 mean=%3")
                                        .arg(depth)
                                        .arg(delta.maximum)
                                        .arg(delta.mean)));

                timer.restart();
                const QImage cpuResult = cpuPaint(source, {}, dabs, paintOptions);
                cpuTimes.append(timer.nsecsElapsed());
                QCOMPARE(cpuResult, expected);
            }
            qInfo() << "GPU_EDIT_BENCHMARK_PAINT_CORE" << "depth" << depth << "size" << size << "dabs"
                    << dabs.size() << "coverage" << "included" << "gpuMedianNs" << median(gpuTimes)
                    << "cpuQPainterMedianNs" << median(cpuTimes) << "warmGpuNs" << "excluded"
                    << "warmDispatches" << warmMetrics.processingDispatches << "processingDispatches" << dispatches
                    << "readbacks" << readbacks << "maxNativeDelta" << warmDelta.maximum << "meanNativeDelta"
                    << warmDelta.mean;
        }
    }

    void gpuPaintWrapperBenchmarkWhenRequested() {
        if (!qEnvironmentVariableIntValue("COMPOSITOR_GPU_BENCHMARK"))
            QSKIP("set COMPOSITOR_GPU_BENCHMARK=1 to run the opt-in benchmark");
        const QSize size(600, 256);
        constexpr double brushSize = 256.;
        constexpr int expectedDabs = 7;
        for (const int depth : {8, 16}) {
            const QImage target = paintFixture(size, depth);
            const QImage retouchSource = paintFixture(size, depth).mirrored(true, false);
            const QPointF from(180, 128);
            const QPointF to(412, 128); // 232 / (256 * .15) -> six steps, seven dabs.
            const QPointF cloneOffset(4.35, -2.65);
            const double flow = .63;
            const QImage expected = cpuCanvasCloneStroke(target, retouchSource, from, to, brushSize, flow,
                                                         QTransform(), cloneOffset);

            // Warm the complete Canvas wrapper, including coverage generation,
            // before the timed samples.  The reference image is deliberately
            // prepared above so its per-pixel work is not charged to timing.
            QImage warm = target;
            resetGpuPreviewMetrics();
            const bool warmAccelerated = paintStrokeAccelerated(
                warm, from, to, brushSize, .99, flow, QColor::fromRgbF(.1F, .2F, .3F, 1.F), false, false,
                QTransform(), {}, retouchSource, cloneOffset, true);
            const auto warmMetrics = gpuPreviewMetrics();
            if (!warmAccelerated || warmMetrics.processingDispatches == 0 || warmMetrics.readbacks == 0) {
                const QString message = QString("benchmark wrapper depth=%1 used fallback: accelerated=%2 dispatches=%3 readbacks=%4")
                                            .arg(depth)
                                            .arg(warmAccelerated)
                                            .arg(warmMetrics.processingDispatches)
                                            .arg(warmMetrics.readbacks);
                if (qEnvironmentVariableIntValue("COMPOSITOR_REQUIRE_GPU"))
                    QFAIL(qPrintable(message));
                QSKIP(qPrintable(message));
            }
            const auto warmDelta = difference(warm, expected, depth);
            QVERIFY2(warmDelta.maximum <= (depth == 8 ? 2 : 8),
                     qPrintable(QString("benchmark warm wrapper depth=%1 max=%2 mean=%3")
                                    .arg(depth)
                                    .arg(warmDelta.maximum)
                                    .arg(warmDelta.mean)));

            QVector<qint64> gpuTimes;
            QVector<qint64> cpuTimes;
            quint64 dispatches = 0;
            quint64 readbacks = 0;
            QElapsedTimer timer;
            for (int repeat = 0; repeat < 3; ++repeat) {
                QImage gpu = target;
                resetGpuPreviewMetrics();
                timer.start();
                const bool accelerated = paintStrokeAccelerated(
                    gpu, from, to, brushSize, .99, flow, QColor::fromRgbF(.1F, .2F, .3F, 1.F), false, false,
                    QTransform(), {}, retouchSource, cloneOffset, true);
                gpuTimes.append(timer.nsecsElapsed());
                const auto metrics = gpuPreviewMetrics();
                QVERIFY2(accelerated && metrics.processingDispatches > 0 && metrics.readbacks > 0,
                         qPrintable(QString("benchmark wrapper sample depth=%1 used CPU fallback")
                                        .arg(depth)));
                dispatches += metrics.processingDispatches;
                readbacks += metrics.readbacks;
                const auto delta = difference(gpu, expected, depth);
                QVERIFY2(delta.maximum <= (depth == 8 ? 2 : 8),
                         qPrintable(QString("benchmark wrapper sample depth=%1 max=%2 mean=%3")
                                        .arg(depth)
                                        .arg(delta.maximum)
                                        .arg(delta.mean)));

                QImage cpu = target.copy();
                timer.restart();
                cpu = cpuCanvasCloneStroke(cpu, retouchSource, from, to, brushSize, flow, QTransform(), cloneOffset);
                cpuTimes.append(timer.nsecsElapsed());
                QCOMPARE(cpu, expected);
            }
            qInfo() << "GPU_EDIT_BENCHMARK_PAINT_WRAPPER" << "depth" << depth << "size" << size
                    << "brushSize" << brushSize << "batchDabs" << expectedDabs << "gpuMedianNs" << median(gpuTimes)
                    << "cpuQPainterMedianNs" << median(cpuTimes) << "warmDispatches"
                    << warmMetrics.processingDispatches << "processingDispatches" << dispatches << "readbacks"
                    << readbacks << "maxNativeDelta" << warmDelta.maximum << "meanNativeDelta" << warmDelta.mean;
        }
    }

    void gpuHealBenchmarkWhenRequested() {
        if (!qEnvironmentVariableIntValue("COMPOSITOR_GPU_BENCHMARK"))
            QSKIP("set COMPOSITOR_GPU_BENCHMARK=1 to run the opt-in benchmark");
        const QSize size(384, 320);
        for (const int depth : {8, 16}) {
            const QImage source = paintFixture(size, depth);
            const QImage healingSource = source.convertToFormat(
                depth == 16 ? QImage::Format_RGBA64_Premultiplied : QImage::Format_RGBA8888_Premultiplied);
            const QImage healMask = healCoverage(size, depth, true);

            // Keep image conversion and the expected CPU image outside timing;
            // both GPU and CPU are measured on the same canonical RGBA source.
            QImage expected = healingSource.copy();
            cpuHeal(expected, healMask, .61f, 0, 93, depth);

            resetGpuPreviewMetrics();
            const QImage warmHeal = healGpuImage(healingSource, healMask, .61f, 0, 93);
            const auto warmMetrics = gpuPreviewMetrics();
            if (warmHeal.isNull() || warmMetrics.processingDispatches == 0 || warmMetrics.readbacks == 0) {
                const QString message = QString("benchmark heal depth=%1 used fallback: dispatches=%2 readbacks=%3")
                                            .arg(depth)
                                            .arg(warmMetrics.processingDispatches)
                                            .arg(warmMetrics.readbacks);
                if (qEnvironmentVariableIntValue("COMPOSITOR_REQUIRE_GPU"))
                    QFAIL(qPrintable(message));
                QSKIP(qPrintable(message));
            }
            const auto warmDelta = difference(warmHeal, expected, depth);
            QVERIFY2(warmDelta.maximum <= (depth == 8 ? 2 : 8),
                     qPrintable(QString("benchmark warm heal depth=%1 max=%2 mean=%3")
                                    .arg(depth)
                                    .arg(warmDelta.maximum)
                                    .arg(warmDelta.mean)));

            QVector<qint64> gpuTimes;
            QVector<qint64> cpuTimes;
            quint64 dispatches = 0;
            quint64 readbacks = 0;
            QElapsedTimer timer;
            for (int repeat = 0; repeat < 3; ++repeat) {
                resetGpuPreviewMetrics();
                timer.start();
                const QImage gpu = healGpuImage(healingSource, healMask, .61f, 0, 93);
                gpuTimes.append(timer.nsecsElapsed());
                const auto metrics = gpuPreviewMetrics();
                QVERIFY2(!gpu.isNull() && metrics.processingDispatches > 0 && metrics.readbacks > 0,
                         qPrintable(QString("benchmark heal sample depth=%1 used CPU fallback")
                                        .arg(depth)));
                dispatches += metrics.processingDispatches;
                readbacks += metrics.readbacks;
                const auto delta = difference(gpu, expected, depth);
                QVERIFY2(delta.maximum <= (depth == 8 ? 2 : 8),
                         qPrintable(QString("benchmark heal sample depth=%1 max=%2 mean=%3")
                                        .arg(depth)
                                        .arg(delta.maximum)
                                        .arg(delta.mean)));

                QImage cpu = healingSource.copy();
                timer.restart();
                cpuHeal(cpu, healMask, .61f, 0, 93, depth);
                cpuTimes.append(timer.nsecsElapsed());
                QCOMPARE(cpu, expected);
            }
            qInfo() << "GPU_EDIT_BENCHMARK_HEAL" << "depth" << depth << "size" << size << "gpuMedianNs"
                    << median(gpuTimes) << "cpuMedianNs" << median(cpuTimes) << "warmDispatches"
                    << warmMetrics.processingDispatches << "processingDispatches" << dispatches << "readbacks"
                    << readbacks << "maxNativeDelta" << warmDelta.maximum << "meanNativeDelta" << warmDelta.mean;
        }
    }
};

int main(int argc, char **argv) {
    Q_INIT_RESOURCE(translations);
    Q_INIT_RESOURCE(icons);
    Q_INIT_RESOURCE(controls);
    QApplication app(argc, argv);
    QImageReader::setAllocationLimit(1024);
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QCoreApplication::setOrganizationName("CompositorGpuEditTests");
    QCoreApplication::setApplicationName("GpuEditTests");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QTemporaryDir settings;
    if (!settings.isValid())
        return 1;
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    GpuEditTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "GpuEditTests.moc"
