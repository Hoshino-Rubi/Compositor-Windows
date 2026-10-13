#include "Canvas.h"
#include "Document.h"
#include "GpuPreview.h"
#include "GpuWarp.h"
#include "Pixels.h"
#include "Warp.h"
#include <QApplication>
#include <QColorSpace>
#include <QElapsedTimer>
#include <QImageReader>
#include <QPainter>
#include <QSettings>
#include <QTest>
#include <algorithm>
#include <cmath>

using namespace comp;

namespace {

class ScopedEnvironment {
  public:
    ScopedEnvironment(const char *name, const QByteArray &value) : name_(name), old_(qgetenv(name)), had_(qEnvironmentVariableIsSet(name)) {
        qputenv(name_, value);
    }
    ~ScopedEnvironment() {
        if (had_)
            qputenv(name_, old_);
        else
            qunsetenv(name_);
    }

  private:
    QByteArray name_, old_;
    bool had_;
};

QImage warpFixture(QSize size, int depth) {
    QImage image(size, pixelFormat(depth));
    if (image.isNull())
        return {};
    image.setColorSpace(QColorSpace::SRgb);
    image.setDotsPerMeterX(2835);
    image.setDotsPerMeterY(5669);
    const int maximum = depth == 16 ? 65535 : 255;
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            int alpha = (x * 971 + y * 307 + 107) % (maximum + 1);
            if ((x + 3 * y) % 17 == 0)
                alpha = 0;
            else if ((x + y) % 11 == 0)
                alpha = maximum / 2;
            const int red = alpha ? (x * 313 + y * 17) % (alpha + 1) : 0;
            const int green = alpha ? (x * 71 + y * 541) % (alpha + 1) : 0;
            const int blue = alpha ? (x * 997 + y * 31) % (alpha + 1) : 0;
            if (depth == 16)
                reinterpret_cast<QRgba64 *>(image.scanLine(y))[x] = rgba64(red, green, blue, alpha);
            else
                reinterpret_cast<QRgb *>(image.scanLine(y))[x] = qRgba(red, green, blue, alpha);
        }
    }
    return image;
}

State warpState(const QImage &image) {
    State state;
    state.id = newId();
    state.size = image.size();
    state.bitDepth = wideImage(image) ? 16 : 8;
    Layer layer;
    layer.id = newId();
    layer.name = "Warp fixture";
    layer.image = image;
    layer.placement.size = image.size();
    state.layers.append(layer);
    state.active = layer.id;
    return state;
}

QPainterPath pathFromMaskHole(QSize size, QRect hole) {
    QPainterPath path;
    path.setFillRule(Qt::OddEvenFill);
    path.addRect(QRectF(QPointF(0, 0), QSizeF(size)));
    path.addRect(QRectF(hole));
    return path;
}

QImage selectionMaskWithHole(QSize size, QRect hole) {
    QImage mask(size, QImage::Format_Grayscale8);
    mask.fill(255);
    QPainter painter(&mask);
    painter.fillRect(hole, Qt::black);
    return mask;
}

QImage cpuWarp(const QImage &source, const QVector<QPointF> &points, double radius, double hardness, double strength,
              bool smudge, const QPainterPath &selection = {}) {
    QImage result = source;
    for (int i = 1; i < points.size(); ++i)
        warpDab(result, points[i - 1], points[i], radius, hardness, strength, smudge, selection);
    return result;
}

struct NativePixel {
    int red = 0, green = 0, blue = 0, alpha = 0;
};

NativePixel nativePixel(const QImage &image, int x, int y, int depth) {
    if (depth == 16) {
        const auto pixel = pixel64(image, x, y);
        return {pixel.red(), pixel.green(), pixel.blue(), pixel.alpha()};
    }
    const QRgb pixel = image.pixel(x, y);
    return {qRed(pixel), qGreen(pixel), qBlue(pixel), qAlpha(pixel)};
}

struct Difference {
    int maximum = 0;
    quint64 overOne = 0;
    quint64 samples = 0;
};

Difference difference(const QImage &actual, const QImage &expected, int depth) {
    Difference result;
    if (actual.size() != expected.size()) {
        result.maximum = depth == 16 ? 65535 : 255;
        return result;
    }
    for (int y = 0; y < actual.height(); ++y) {
        for (int x = 0; x < actual.width(); ++x) {
            const auto a = nativePixel(actual, x, y, depth), b = nativePixel(expected, x, y, depth);
            for (const int delta : {std::abs(a.red - b.red), std::abs(a.green - b.green), std::abs(a.blue - b.blue),
                                    std::abs(a.alpha - b.alpha)}) {
                result.maximum = std::max(result.maximum, delta);
                result.overOne += delta > 1;
                ++result.samples;
            }
        }
    }
    return result;
}

QRect dabArea(QPointF to, double radius, QSize size) {
    return QRectF(to - QPointF(radius, radius), QSizeF(radius * 2, radius * 2)).toAlignedRect().intersected(QRect(QPoint(), size));
}

QRect affectedArea(const QVector<QPointF> &points, double radius, QSize size) {
    QRect result;
    for (int i = 1; i < points.size(); ++i)
        result = result.isEmpty() ? dabArea(points[i], radius, size) : result.united(dabArea(points[i], radius, size));
    return result;
}

QRect sourceRoi(const QVector<QPointF> &points, double radius, double strength, bool smudge, QSize size) {
    const QRect bounds(QPoint(), size);
    QRect result;
    for (int i = 1; i < points.size(); ++i) {
        const QPointF from = points[i - 1], to = points[i];
        if (from == to)
            continue;
        const QRect area = dabArea(to, radius, size);
        if (area.isEmpty())
            continue;
        const QPointF movement = (to - from) * (smudge ? 1. : strength);
        const int margin = qCeil(std::max(std::abs(movement.x()), std::abs(movement.y()))) + 2;
        const QRect source = area.adjusted(-margin, -margin, margin, margin).intersected(bounds);
        result = result.isEmpty() ? source : result.united(source);
    }
    return result;
}

QPainterPath translatedPath(const QPainterPath &path, QPoint offset) {
    QTransform transform;
    transform.translate(-offset.x(), -offset.y());
    return transform.map(path);
}

bool samePixel(const QImage &a, const QImage &b, int x, int y, int depth) {
    const auto left = nativePixel(a, x, y, depth), right = nativePixel(b, x, y, depth);
    return left.red == right.red && left.green == right.green && left.blue == right.blue && left.alpha == right.alpha;
}

bool outsideExact(const QImage &actual, const QImage &source, const QRect &affected, int depth) {
    if (actual.size() != source.size())
        return false;
    for (int y = 0; y < actual.height(); ++y)
        for (int x = 0; x < actual.width(); ++x)
            if (!affected.contains(x, y) && !samePixel(actual, source, x, y, depth))
                return false;
    return true;
}

bool metadataEqual(const QImage &left, const QImage &right) {
    return left.colorSpace() == right.colorSpace() && left.dotsPerMeterX() == right.dotsPerMeterX() &&
           left.dotsPerMeterY() == right.dotsPerMeterY();
}

QString gpuFailure(const char *test) {
    return QString("%1: %2").arg(QString::fromLatin1(test), gpuPreviewStatus());
}

qint64 median(QVector<qint64> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

} // namespace

class GpuWarpTests : public QObject {
    Q_OBJECT

  private slots:
    void gpuWarpSingleDabMatchesCpu() {
        for (const int depth : {8, 16}) {
            for (const bool smudge : {false, true}) {
                const QImage source = warpFixture({97, 73}, depth);
                const QVector<QPointF> points{{4.25, 5.75}, {13.5, 10.25}};
                const auto expected = cpuWarp(source, points, 18, .3, .7, smudge);
                resetGpuPreviewMetrics();
                const auto actual = warpGpuImage(source, points, 18, .3, .7, smudge);
                if (actual.isNull()) {
                    const auto message = gpuFailure("single dab");
                    if (qEnvironmentVariableIntValue("COMPOSITOR_REQUIRE_GPU"))
                        QFAIL(qPrintable(message));
                    QSKIP(qPrintable(message));
                }
                const auto metrics = gpuPreviewMetrics();
                QVERIFY2(metrics.processingDispatches > 0 && metrics.readbacks > 0,
                         qPrintable(QString("single dab used fallback: dispatches=%1 readbacks=%2")
                                        .arg(metrics.processingDispatches)
                                        .arg(metrics.readbacks)));
                QCOMPARE(actual.size(), source.size());
                QCOMPARE(actual.format(), source.format());
                QVERIFY2(metadataEqual(actual, source), "GPU warp did not preserve image metadata");
                const auto delta = difference(actual, expected, depth);
                qInfo() << "WARP_REFERENCE sequential warpDab" << "single" << "depth" << depth << "smudge" << smudge
                        << "maxNativeDelta" << delta.maximum << "valuesOver1" << delta.overOne;
                QVERIFY2(delta.maximum <= (depth == 8 ? 1 : 16),
                         qPrintable(QString("single dab parity depth=%1 smudge=%2 max=%3")
                                        .arg(depth)
                                        .arg(smudge)
                                        .arg(delta.maximum)));
                QVERIFY2(outsideExact(actual, source, affectedArea(points, 18, source.size()), depth),
                         "GPU warp changed pixels outside the dab ROI");
                QCOMPARE(source, warpFixture({97, 73}, depth));
            }
        }
    }

    void gpuWarpBatchSelectionEdgesAndTransparency() {
        for (const int depth : {8, 16}) {
            for (const bool smudge : {false, true}) {
                const QImage source = warpFixture({113, 79}, depth);
                const QVector<QPointF> points{{2.5, 3.5}, {16.25, 7.75}, {28.5, 14.25}, {36.75, 24.5}};
                const QRect hole(20, 9, 13, 12);
                const auto mask = selectionMaskWithHole(source.size(), hole);
                const auto selection = pathFromMaskHole(source.size(), hole);
                const auto expected = cpuWarp(source, points, 19, .42, .63, smudge, selection);
                resetGpuPreviewMetrics();
                const auto actual = warpGpuImage(source, points, 19, .42, .63, smudge, mask);
                if (actual.isNull()) {
                    const auto message = gpuFailure("batch selection");
                    if (qEnvironmentVariableIntValue("COMPOSITOR_REQUIRE_GPU"))
                        QFAIL(qPrintable(message));
                    QSKIP(qPrintable(message));
                }
                const auto metrics = gpuPreviewMetrics();
                QVERIFY2(metrics.processingDispatches > 0 && metrics.readbacks > 0,
                         qPrintable(QString("batch selection used fallback: dispatches=%1 readbacks=%2")
                                        .arg(metrics.processingDispatches)
                                        .arg(metrics.readbacks)));
                const auto delta = difference(actual, expected, depth);
                qInfo() << "WARP_REFERENCE sequential warpDab" << "batch selection" << "depth" << depth << "smudge"
                        << smudge << "maxNativeDelta" << delta.maximum << "valuesOver1" << delta.overOne;
                QVERIFY2(delta.maximum <= (depth == 8 ? 1 : 16),
                         qPrintable(QString("batch selection parity depth=%1 smudge=%2 max=%3")
                                        .arg(depth)
                                        .arg(smudge)
                                        .arg(delta.maximum)));
                QVERIFY2(outsideExact(actual, source, affectedArea(points, 19, source.size()), depth),
                         "GPU batch changed pixels outside the ROI");
                for (int y = hole.top(); y <= hole.bottom(); ++y)
                    for (int x = hole.left(); x <= hole.right(); ++x)
                        QVERIFY2(samePixel(actual, source, x, y, depth), "selection hole was modified");
                QVERIFY2(metadataEqual(actual, source), "GPU batch did not preserve image metadata");
            }
        }
    }

    void acceleratedWrapperFallbackAndThreshold() {
        for (const int depth : {8, 16}) {
            const QImage source = warpFixture({129, 97}, depth);
            const QVector<QPointF> smallPoints{{20, 24}, {22, 25}};
            const auto expected = cpuWarp(source, smallPoints, 8, .35, .7, false);
            QImage actual = source;
            {
                ScopedEnvironment disabled("COMPOSITOR_DISABLE_GPU", "1");
                resetGpuPreviewMetrics();
                warpStrokeAccelerated(actual, smallPoints, 8, .35, .7, false);
                const auto metrics = gpuPreviewMetrics();
                QCOMPARE(metrics.processingDispatches, quint64(0));
                QCOMPARE(metrics.readbacks, quint64(0));
                QCOMPARE(actual, expected);
            }
            QCOMPARE(source, warpFixture({129, 97}, depth));
            QVERIFY2(metadataEqual(actual, source), "CPU fallback changed image metadata");

            const QImage largeSource = warpFixture({257, 257}, depth);
            const QVector<QPointF> largePoints{{74, 128}, {166, 128}, {198, 134}};
            resetGpuPreviewMetrics();
            const auto probe = warpGpuImage(largeSource, largePoints, 96, .3, .7, false);
            if (probe.isNull()) {
                const auto message = gpuFailure("accelerated wrapper threshold");
                if (qEnvironmentVariableIntValue("COMPOSITOR_REQUIRE_GPU"))
                    QFAIL(qPrintable(message));
                QSKIP(qPrintable(message));
            }
            const auto largeExpected = cpuWarp(largeSource, largePoints, 96, .3, .7, false);
            actual = largeSource;
            resetGpuPreviewMetrics();
            warpStrokeAccelerated(actual, largePoints, 96, .3, .7, false);
            const auto metrics = gpuPreviewMetrics();
            QVERIFY2(metrics.processingDispatches > 0 && metrics.readbacks > 0,
                     qPrintable(QString("large wrapper fell back: dispatches=%1 readbacks=%2")
                                    .arg(metrics.processingDispatches)
                                    .arg(metrics.readbacks)));
            const auto delta = difference(actual, largeExpected, depth);
            qInfo() << "WARP_REFERENCE sequential warpDab" << "wrapper large" << "depth" << depth << "maxNativeDelta"
                    << delta.maximum << "valuesOver1" << delta.overOne;
            QVERIFY2(delta.maximum <= (depth == 8 ? 1 : 16),
                     qPrintable(QString("large wrapper parity depth=%1 max=%2").arg(depth).arg(delta.maximum)));
            QVERIFY2(outsideExact(actual, largeSource, affectedArea(largePoints, 96, largeSource.size()), depth),
                     "large wrapper changed pixels outside the ROI");
            QCOMPARE(largeSource, warpFixture({257, 257}, depth));
            QVERIFY2(metadataEqual(actual, largeSource), "GPU wrapper did not preserve image metadata");
        }
    }

    void acceleratedWrapperUsesOffsetRoiAndSelectionPath() {
        const QSize documentSize(3200, 2800);
        const double radius = 250;
        const double strength = .67;
        const QVector<QPointF> points{{1540, 1360}, {1544, 1364}, {1551, 1370}, {1558, 1378},
                                      {1566, 1387}, {1574, 1396}, {1582, 1404}, {1590, 1412}};
        // The outer shape is deliberately non-rectangular; the inner rectangle is
        // an odd-even hole that must remain byte-for-byte unchanged.
        const QRect hole(1530, 1335, 90, 70);
        QPainterPath selection;
        selection.setFillRule(Qt::OddEvenFill);
        selection.addEllipse(QRectF(1430, 1260, 460, 360));
        selection.addRect(QRectF(hole));
        for (const int depth : {8, 16}) {
            for (const bool smudge : {false, true}) {
                const QImage source = warpFixture(documentSize, depth);
                const QRect roi = sourceRoi(points, radius, strength, smudge, documentSize);
                QVERIFY2(!roi.isEmpty() && roi.width() < 900 && roi.height() < 900,
                         qPrintable(QString("unexpected ROI %1x%2").arg(roi.width()).arg(roi.height())));
                QVERIFY(quint64(roi.width()) * roi.height() < 8ull * 1024 * 1024);
                const QImage roiSource = source.copy(roi);
                const QVector<QPointF> localPoints = [&] {
                    QVector<QPointF> result;
                    result.reserve(points.size());
                    for (const auto &point : points)
                        result.append(point - roi.topLeft());
                    return result;
                }();
                const auto expected = cpuWarp(roiSource, localPoints, radius, .3, strength, smudge,
                                              translatedPath(selection, roi.topLeft()));
                QImage actual = source;
                resetGpuPreviewMetrics();
                warpStrokeAccelerated(actual, points, radius, .3, strength, smudge, selection);
                const auto metrics = gpuPreviewMetrics();
                if (metrics.processingDispatches == 0 || metrics.readbacks == 0) {
                    const auto message = QString("offset ROI wrapper used CPU: depth=%1 smudge=%2 dispatches=%3 readbacks=%4")
                                             .arg(depth)
                                             .arg(smudge)
                                             .arg(metrics.processingDispatches)
                                             .arg(metrics.readbacks);
                    if (qEnvironmentVariableIntValue("COMPOSITOR_REQUIRE_GPU"))
                        QFAIL(qPrintable(message));
                    QSKIP(qPrintable(message));
                }
                const quint64 fullBytes = quint64(documentSize.width()) * documentSize.height() * (depth == 16 ? 8 : 4);
                QVERIFY2(metrics.processingUploadedBytes > 0 && metrics.processingUploadedBytes * 8 < fullBytes,
                         qPrintable(QString("ROI upload is not substantially below full image: uploaded=%1 full=%2")
                                        .arg(metrics.processingUploadedBytes)
                                        .arg(fullBytes)));
                const auto delta = difference(actual.copy(roi), expected, depth);
                qInfo() << "WARP_REFERENCE local ROI sequential warpDab" << "depth" << depth << "smudge" << smudge
                        << "roi" << roi << "maxNativeDelta" << delta.maximum << "valuesOver1" << delta.overOne
                        << "processingUploadedBytes" << metrics.processingUploadedBytes << "fullBytes" << fullBytes;
                QVERIFY2(delta.maximum <= (depth == 8 ? 1 : 16),
                         qPrintable(QString("offset ROI parity depth=%1 smudge=%2 max=%3")
                                        .arg(depth)
                                        .arg(smudge)
                                        .arg(delta.maximum)));
                QVERIFY2(outsideExact(actual, source, roi, depth), "offset ROI wrapper changed pixels outside its ROI");
                for (int y = hole.top(); y <= hole.bottom(); ++y)
                    for (int x = hole.left(); x <= hole.right(); ++x)
                        QVERIFY2(samePixel(actual, source, x, y, depth), "path selection hole was modified");
                QVERIFY2(metadataEqual(actual, source), "offset ROI wrapper changed DPI/color metadata");
                QCOMPARE(source, warpFixture(documentSize, depth));
            }
        }
    }

    void canvasRetouchUsesGpuForLargeBrushAndUndo() {
        const int depth = 8;
        const QImage image = warpFixture({257, 257}, depth);
        const QVector<QPointF> probePoints{{74, 128}, {166, 128}};
        resetGpuPreviewMetrics();
        if (warpGpuImage(image, probePoints, 96, .3, .7, false).isNull()) {
            const auto message = gpuFailure("Canvas large brush");
            if (qEnvironmentVariableIntValue("COMPOSITOR_REQUIRE_GPU"))
                QFAIL(qPrintable(message));
            QSKIP(qPrintable(message));
        }

        Document document;
        document.reset(warpState(image), true);
        Canvas canvas(&document);
        canvas.resize(700, 700);
        canvas.show();
        canvas.fit();
        QCoreApplication::processEvents();
        canvas.setTool(Tool::Blur);
        canvas.setRetouchMode(2);
        canvas.setBrushSize(192);
        canvas.setHardness(.3);
        canvas.setFlow(.7);
        const QImage before = document.state().layers[0].image;
        resetGpuPreviewMetrics();
        const QPoint start = canvas.viewPoint(probePoints[0]).toPoint();
        const QPoint end = canvas.viewPoint(probePoints[1]).toPoint();
        QTest::mousePress(&canvas, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(&canvas, end, 1);
        QTest::mouseRelease(&canvas, Qt::LeftButton, Qt::NoModifier, end);
        QCoreApplication::processEvents();
        const auto metrics = gpuPreviewMetrics();
        QVERIFY2(metrics.processingDispatches > 0 && metrics.readbacks > 0,
                 qPrintable(QString("Canvas large brush used fallback: dispatches=%1 readbacks=%2")
                                .arg(metrics.processingDispatches)
                                .arg(metrics.readbacks)));
        QVERIFY(document.canUndo());
        QVERIFY(document.state().layers[0].image != before);
        document.undo();
        QCOMPARE(document.state().layers[0].image, before);
    }

    void canvasRetouchUsesCpuForSmallBrush() {
        const QImage image = warpFixture({129, 97}, 8);
        Document document;
        document.reset(warpState(image), true);
        Canvas canvas(&document);
        canvas.resize(640, 520);
        canvas.show();
        canvas.fit();
        QCoreApplication::processEvents();
        canvas.setTool(Tool::Blur);
        canvas.setRetouchMode(2);
        canvas.setBrushSize(8);
        canvas.setHardness(.3);
        canvas.setFlow(.7);
        const QImage before = document.state().layers[0].image;
        resetGpuPreviewMetrics();
        const QPoint start = canvas.viewPoint({48, 48}).toPoint();
        const QPoint end = canvas.viewPoint({50, 48}).toPoint();
        QTest::mousePress(&canvas, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(&canvas, end, 1);
        QTest::mouseRelease(&canvas, Qt::LeftButton, Qt::NoModifier, end);
        QCoreApplication::processEvents();
        const auto metrics = gpuPreviewMetrics();
        QCOMPARE(metrics.processingDispatches, quint64(0));
        QCOMPARE(metrics.readbacks, quint64(0));
        QVERIFY(document.canUndo());
        QVERIFY(document.state().layers[0].image != before);
        document.undo();
        QCOMPARE(document.state().layers[0].image, before);
    }

    void canvasSixteenBitSoftSelectionAndMaskTargetUndo() {
        const QSize size(257, 257);
        const QImage image = warpFixture(size, 16);
        const QVector<QPointF> probePoints{{74, 128}, {166, 128}};
        resetGpuPreviewMetrics();
        if (warpGpuImage(image, probePoints, 96, .3, .7, false).isNull()) {
            const auto message = gpuFailure("Canvas 16-bit selection");
            if (qEnvironmentVariableIntValue("COMPOSITOR_REQUIRE_GPU"))
                QFAIL(qPrintable(message));
            QSKIP(qPrintable(message));
        }

        QImage softMask(size, QImage::Format_Grayscale16);
        for (int y = 0; y < size.height(); ++y) {
            auto *row = reinterpret_cast<quint16 *>(softMask.scanLine(y));
            for (int x = 0; x < size.width(); ++x)
                row[x] = x < 80 ? 0 : x < 128 ? 32768 : 65535;
        }
        Document document;
        document.reset(warpState(image), true);
        document.setSelectionMask(softMask);
        Canvas canvas(&document);
        canvas.resize(700, 700);
        canvas.show();
        canvas.fit();
        QCoreApplication::processEvents();
        canvas.setTool(Tool::Blur);
        canvas.setRetouchMode(2);
        canvas.setBrushSize(192);
        canvas.setHardness(.3);
        canvas.setFlow(.7);
        const QImage before = document.state().layers[0].image;
        resetGpuPreviewMetrics();
        const QPoint start = canvas.viewPoint(probePoints[0]).toPoint();
        const QPoint end = canvas.viewPoint(probePoints[1]).toPoint();
        QTest::mousePress(&canvas, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(&canvas, end, 1);
        QTest::mouseRelease(&canvas, Qt::LeftButton, Qt::NoModifier, end);
        QCoreApplication::processEvents();
        const auto metrics = gpuPreviewMetrics();
        QVERIFY2(metrics.processingDispatches > 0 && metrics.readbacks > 0,
                 qPrintable(QString("16-bit soft selection used fallback: dispatches=%1 readbacks=%2")
                                .arg(metrics.processingDispatches)
                                .arg(metrics.readbacks)));
        const auto &after = document.state().layers[0].image;
        bool zeroCoverageUnchanged = true, selectedPixelsChanged = false;
        for (int y = 0; y < size.height(); ++y) {
            for (int x = 0; x < size.width(); ++x) {
                const bool equal = samePixel(after, before, x, y, 16);
                if (x < 80)
                    zeroCoverageUnchanged = zeroCoverageUnchanged && equal;
                else
                    selectedPixelsChanged = selectedPixelsChanged || !equal;
            }
        }
        QVERIFY2(zeroCoverageUnchanged, "soft selection changed a zero-coverage region");
        QVERIFY2(selectedPixelsChanged, "soft selection produced no covered retouch pixels");
        QVERIFY(document.canUndo());
        document.undo();
        QCOMPARE(document.state().layers[0].image, before);
        QCOMPARE(document.state().selectionMask, softMask);

        State maskState = warpState(image);
        maskState.layers[0].mask = QImage(size, QImage::Format_Grayscale16);
        for (int y = 0; y < size.height(); ++y) {
            auto *row = reinterpret_cast<quint16 *>(maskState.layers[0].mask.scanLine(y));
            for (int x = 0; x < size.width(); ++x)
                row[x] = quint16((x * 977 + y * 313 + ((x / 9 + y / 7) % 2) * 8192) % 65536);
        }
        maskState.layers[0].maskLinked = false;
        maskState.layers[0].maskPlacement.size = size;
        Document maskDocument;
        maskDocument.reset(maskState, true);
        Canvas maskCanvas(&maskDocument);
        maskCanvas.resize(700, 700);
        maskCanvas.show();
        maskCanvas.fit();
        QCoreApplication::processEvents();
        maskCanvas.setMaskTarget(true);
        maskCanvas.setTool(Tool::Blur);
        maskCanvas.setRetouchMode(2);
        maskCanvas.setBrushSize(192);
        maskCanvas.setHardness(.3);
        maskCanvas.setFlow(.7);
        const QImage beforeMask = maskDocument.state().layers[0].mask;
        resetGpuPreviewMetrics();
        const QPoint maskStart = maskCanvas.viewPoint(probePoints[0]).toPoint();
        const QPoint maskEnd = maskCanvas.viewPoint(probePoints[1]).toPoint();
        QTest::mousePress(&maskCanvas, Qt::LeftButton, Qt::NoModifier, maskStart);
        QTest::mouseMove(&maskCanvas, maskEnd, 1);
        QTest::mouseRelease(&maskCanvas, Qt::LeftButton, Qt::NoModifier, maskEnd);
        QCoreApplication::processEvents();
        const auto maskMetrics = gpuPreviewMetrics();
        QVERIFY2(maskMetrics.processingDispatches > 0 && maskMetrics.readbacks > 0,
                 qPrintable(QString("16-bit mask target used fallback: dispatches=%1 readbacks=%2")
                                .arg(maskMetrics.processingDispatches)
                                .arg(maskMetrics.readbacks)));
        QVERIFY(maskDocument.canUndo());
        QVERIFY(maskDocument.state().layers[0].mask != beforeMask);
        QVERIFY(wideImage(maskDocument.state().layers[0].mask));
        maskDocument.undo();
        QCOMPARE(maskDocument.state().layers[0].mask, beforeMask);
    }

    void acceleratedWrapperLowBudgetFallsBackWithoutPartialEdits() {
        for (const int depth : {8,16}) {
            const auto source = warpFixture({512,512},depth);
            const QVector<QPointF> points{{200,256},{208,260},{216,258}};
            for (const bool smudge : {false,true}) {
                auto actual = source;
                const auto expected = cpuWarp(source,points,160,.3,.7,smudge);
                ScopedEnvironment budget("COMPOSITOR_GPU_CACHE_MB","1");
                clearGpuPreviewCache();resetGpuPreviewMetrics();
                warpStrokeAccelerated(actual,points,160,.3,.7,smudge);
                QCOMPARE(actual,expected);
                QCOMPARE(gpuPreviewMetrics().processingDispatches,quint64(0));
                QCOMPARE(gpuPreviewMetrics().processingUploads,quint64(0));
                QCOMPARE(gpuPreviewMetrics().processingReadbackBytes,quint64(0));
            }
        }
    }

    void gpuWarpBenchmarkWhenRequested() {
        if (!qEnvironmentVariableIntValue("COMPOSITOR_GPU_BENCHMARK"))
            QSKIP("set COMPOSITOR_GPU_BENCHMARK=1 to run the opt-in benchmark");
        for (const auto &entry : {std::pair{512, 160}, std::pair{1024, 320}}) {
            for (const int depth : {8, 16}) {
                const QSize size(entry.first, entry.first);
                const QImage source = warpFixture(size, depth);
                const QPointF start(entry.second + 36, entry.first / 2.);
                QVector<QPointF> oneDab{start, start + QPointF(8, 4)};
                QVector<QPointF> batch{start};
                for (int step = 0; step < 7; ++step)
                    batch.append(batch.last() + QPointF(4 + (step % 3) * 4, (step % 2) ? 3 : -2));
                for (const bool smudge : {false, true}) {
                    for (const auto &caseData : {std::pair{QString("one-dab"), oneDab}, std::pair{QString("batch"), batch}}) {
                        const auto &points = caseData.second;
                        resetGpuPreviewMetrics();
                        if (warpGpuImage(source, points, entry.second, .3, .7, smudge).isNull()) {
                            const auto message = gpuFailure("benchmark");
                            if (qEnvironmentVariableIntValue("COMPOSITOR_REQUIRE_GPU"))
                                QFAIL(qPrintable(message));
                            QSKIP(qPrintable(message));
                        }

                        // Warm the complete wrapper once; the three samples below are the requested warm median.
                        QImage warm = source;
                        resetGpuPreviewMetrics();
                        warpStrokeAccelerated(warm, points, entry.second, .3, .7, smudge);
                        const auto warmMetrics = gpuPreviewMetrics();
                        QVERIFY2(warmMetrics.processingDispatches > 0 && warmMetrics.readbacks > 0,
                                 qPrintable(QString("benchmark %1 depth=%2 smudge=%3 used CPU fallback")
                                                .arg(caseData.first)
                                                .arg(depth)
                                                .arg(smudge)));
                        const auto expected = cpuWarp(source, points, entry.second, .3, .7, smudge);
                        const auto warmDelta = difference(warm, expected, depth);
                        QVERIFY2(warmDelta.maximum <= (depth == 8 ? 1 : 16),
                                 qPrintable(QString("benchmark warm parity %1 depth=%2 smudge=%3 max=%4")
                                                .arg(caseData.first)
                                                .arg(depth)
                                                .arg(smudge)
                                                .arg(warmDelta.maximum)));

                        QVector<qint64> gpuTimes, cpuTimes;
                        quint64 dispatches = 0, readbacks = 0;
                        for (int repeat = 0; repeat < 3; ++repeat) {
                            QImage gpu = source;
                            resetGpuPreviewMetrics();
                            QElapsedTimer timer;
                            timer.start();
                            warpStrokeAccelerated(gpu, points, entry.second, .3, .7, smudge);
                            gpuTimes.append(timer.nsecsElapsed());
                            const auto gpuMetrics = gpuPreviewMetrics();
                            QVERIFY2(gpuMetrics.processingDispatches > 0 && gpuMetrics.readbacks > 0,
                                     qPrintable(QString("benchmark sample %1 depth=%2 smudge=%3 used CPU fallback")
                                                    .arg(caseData.first)
                                                    .arg(depth)
                                                    .arg(smudge)));
                            dispatches += gpuMetrics.processingDispatches;
                            readbacks += gpuMetrics.readbacks;
                            const auto delta = difference(gpu, expected, depth);
                            QVERIFY2(delta.maximum <= (depth == 8 ? 1 : 16),
                                     qPrintable(QString("benchmark sample parity %1 depth=%2 smudge=%3 max=%4")
                                                    .arg(caseData.first)
                                                    .arg(depth)
                                                    .arg(smudge)
                                                    .arg(delta.maximum)));

                            QImage cpu = source;
                            timer.restart();
                            const auto cpuResult = cpuWarp(cpu, points, entry.second, .3, .7, smudge);
                            cpuTimes.append(timer.nsecsElapsed());
                            QCOMPARE(cpuResult, expected);
                        }
                        qInfo() << "WARP_BENCHMARK" << caseData.first << "depth" << depth << "size" << size
                                << "radius" << entry.second << "smudge" << smudge << "gpuMedianNs" << median(gpuTimes)
                                << "cpuMedianNs" << median(cpuTimes) << "processingDispatches" << dispatches
                                << "readbacks" << readbacks << "maxNativeDelta" << warmDelta.maximum;
                    }
                }
            }
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
    QCoreApplication::setOrganizationName("CompositorGpuWarpTests");
    QCoreApplication::setApplicationName("GpuWarpTests");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    GpuWarpTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "GpuWarpTests.moc"
