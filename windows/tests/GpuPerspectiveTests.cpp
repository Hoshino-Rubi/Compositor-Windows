#include "GpuPerspective.h"

#include "GpuPreview.h"
#include "Pixels.h"
#include "Warp.h"

#include <QColorSpace>
#include <QElapsedTimer>
#include <QLineF>
#include <QTest>
#include <QVector>
#include <array>
#include <algorithm>
#include <cmath>

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

QImage fixture(QSize size, int depth) {
    QImage result(size, pixelFormat(depth));
    if (result.isNull())
        return {};
    result.setColorSpace(QColorSpace::SRgb);
    result.setDotsPerMeterX(2835);
    result.setDotsPerMeterY(5669);
    const int maximum = depth == 16 ? 65535 : 255;
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            const int alpha = maximum * (1 + ((x * 13 + y * 7) % 9)) / 10;
            const int red = alpha * x / std::max(1, size.width() - 1);
            const int green = alpha * y / std::max(1, size.height() - 1);
            const int blue = alpha * (x * 17 + y * 29) /
                             std::max(1, (size.width() - 1) * 17 + (size.height() - 1) * 29);
            if (depth == 16)
                reinterpret_cast<QRgba64 *>(result.scanLine(y))[x] = rgba64(red, green, blue, alpha);
            else
                reinterpret_cast<QRgb *>(result.scanLine(y))[x] = qRgba(red, green, blue, alpha);
        }
    }
    return result;
}

QImage whiteFixture(QSize size, int depth) {
    QImage result(size, pixelFormat(depth));
    if (result.isNull())
        return {};
    const int maximum = depth == 16 ? 65535 : 255;
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            if (depth == 16)
                reinterpret_cast<QRgba64 *>(result.scanLine(y))[x] = rgba64(maximum, maximum, maximum, maximum);
            else
                reinterpret_cast<QRgb *>(result.scanLine(y))[x] = qRgba(maximum, maximum, maximum, maximum);
        }
    }
    return result;
}

int nativeDifference(const QImage &left, const QImage &right, int depth) {
    if (left.size() != right.size() || left.format() != right.format())
        return depth == 16 ? 65535 : 255;
    int maximum = 0;
    for (int y = 0; y < left.height(); ++y) {
        for (int x = 0; x < left.width(); ++x) {
            if (depth == 16) {
                const auto a = reinterpret_cast<const QRgba64 *>(left.constScanLine(y))[x];
                const auto b = reinterpret_cast<const QRgba64 *>(right.constScanLine(y))[x];
                maximum = std::max({maximum, std::abs(int(a.red()) - int(b.red())),
                                    std::abs(int(a.green()) - int(b.green())),
                                    std::abs(int(a.blue()) - int(b.blue())),
                                    std::abs(int(a.alpha()) - int(b.alpha()))});
            } else {
                const QRgb a = reinterpret_cast<const QRgb *>(left.constScanLine(y))[x];
                const QRgb b = reinterpret_cast<const QRgb *>(right.constScanLine(y))[x];
                maximum = std::max({maximum, std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)),
                                    std::abs(qBlue(a) - qBlue(b)), std::abs(qAlpha(a) - qAlpha(b))});
            }
        }
    }
    return maximum;
}

struct DifferenceSample {
    int maximum = 0;
    QPoint position;
    int channel = -1;
    std::array<int, 4> actual{};
    std::array<int, 4> expected{};
};

std::array<int, 4> nativeChannels(const QImage &image, int x, int y, int depth) {
    if (depth == 16) {
        const auto p = reinterpret_cast<const QRgba64 *>(image.constScanLine(y))[x];
        return {p.red(), p.green(), p.blue(), p.alpha()};
    }
    const QRgb p = reinterpret_cast<const QRgb *>(image.constScanLine(y))[x];
    return {qRed(p), qGreen(p), qBlue(p), qAlpha(p)};
}

DifferenceSample maxDifferenceSample(const QImage &actual, const QImage &expected, int depth) {
    DifferenceSample result;
    for (int y = 0; y < actual.height(); ++y) {
        for (int x = 0; x < actual.width(); ++x) {
            const auto a = nativeChannels(actual, x, y, depth);
            const auto b = nativeChannels(expected, x, y, depth);
            for (int channel = 0; channel < 4; ++channel) {
                const int delta = std::abs(a[channel] - b[channel]);
                if (delta > result.maximum) {
                    result.maximum = delta;
                    result.position = {x, y};
                    result.channel = channel;
                    result.actual = a;
                    result.expected = b;
                }
            }
        }
    }
    return result;
}

int interiorDifference(const QImage &actual, const QImage &expected, int depth, const QPolygonF &destination,
                       const QRect &bounds) {
    int maximum = 0;
    const QPolygonF local = [&] {
        QPolygonF result;
        result.reserve(destination.size());
        for (const auto &point : destination)
            result.append(point - QPointF(bounds.topLeft()));
        return result;
    }();
    for (int y = 0; y < actual.height(); ++y) {
        for (int x = 0; x < actual.width(); ++x) {
            const QPointF center(x + .5, y + .5);
            if (!local.containsPoint(center, Qt::OddEvenFill))
                continue;
            bool safelyInterior = true;
            for (int edge = 0; edge < local.size(); ++edge) {
                const QPointF a = local[edge], b = local[(edge + 1) % local.size()];
                const QPointF edgeVector = b - a;
                const double lengthSquared = QPointF::dotProduct(edgeVector, edgeVector);
                const double along = lengthSquared > 0
                                         ? std::clamp(QPointF::dotProduct(center - a, edgeVector) / lengthSquared, 0., 1.)
                                         : 0.;
                const QPointF nearest = a + edgeVector * along;
                if (QLineF(nearest, center).length() <= 1.) {
                    safelyInterior = false;
                    break;
                }
            }
            if (!safelyInterior)
                continue;
            const auto a = nativeChannels(actual, x, y, depth);
            const auto b = nativeChannels(expected, x, y, depth);
            for (int channel = 0; channel < 4; ++channel)
                maximum = std::max(maximum, std::abs(a[channel] - b[channel]));
        }
    }
    return maximum;
}

QPointF diagnosticSourceCoordinate(const QPolygonF &destination, QSize sourceSize, const QRect &bounds,
                                   QPoint outputPosition) {
    const QPolygonF sourceQuad{{0, 0}, {double(sourceSize.width()), 0},
                               {double(sourceSize.width()), double(sourceSize.height())}, {0, double(sourceSize.height())}};
    QPolygonF localDestination;
    localDestination.reserve(destination.size());
    for (const auto &point : destination)
        localDestination.append(point - QPointF(bounds.topLeft()));
    QTransform local;
    bool inverseValid = QTransform::quadToQuad(sourceQuad, localDestination, local);
    if (!inverseValid)
        return {};
    bool invertible = false;
    const QTransform inverse = local.inverted(&invertible);
    if (!invertible)
        return {};
    return inverse.map(QPointF(outputPosition) + QPointF(.5, .5)) - QPointF(.5, .5);
}

int reportDifference(const char *stage, const QImage &actual, const QImage &expected, int depth,
                     const QPolygonF &destination, QSize sourceSize) {
    const DifferenceSample sample = maxDifferenceSample(actual, expected, depth);
    const QRect bounds = destination.boundingRect().toAlignedRect();
    qInfo() << "PERSPECTIVE_PERF_DIAGNOSTIC" << stage << "depth" << depth << "max" << sample.maximum
            << "pos" << sample.position << "channel" << sample.channel << "actual" << sample.actual[0]
            << sample.actual[1] << sample.actual[2] << sample.actual[3] << "expected" << sample.expected[0]
            << sample.expected[1] << sample.expected[2] << sample.expected[3] << "interiorMax"
            << interiorDifference(actual, expected, depth, destination, bounds) << "sourceCoordinate"
            << diagnosticSourceCoordinate(destination, sourceSize, bounds, sample.position);
    return sample.maximum;
}

qint64 median(QVector<qint64> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

void requireGpuOrSkip(const QImage &image, const char *testName) {
    if (!image.isNull())
        return;
    if (qEnvironmentVariableIntValue("COMPOSITOR_REQUIRE_GPU"))
        QFAIL(testName);
    QSKIP(testName);
}

} // namespace

class GpuPerspectiveTests : public QObject {
    Q_OBJECT

  private slots:
    void arbitraryQuadrilateralMatchesReferenceAndBounds() {
        const QPolygonF destination{{5.25, -2.5}, {49.75, 4.0}, {44.5, 36.25}, {-3.0, 29.5}};
        for (const int depth : {8, 16}) {
            const QImage source = fixture({37, 25}, depth);
            const QImage actual = perspectiveGpuImage(source, destination);
            requireGpuOrSkip(actual, "Direct3D perspective GPU unavailable");
            const QImage expected = perspectiveWarp(source, destination);
            QCOMPARE(actual.size(), destination.boundingRect().toAlignedRect().size());
            QCOMPARE(actual.format(), pixelFormat(depth));
            QCOMPARE(actual.colorSpace(), source.colorSpace());
            QCOMPARE(actual.dotsPerMeterX(), source.dotsPerMeterX());
            QCOMPARE(actual.dotsPerMeterY(), source.dotsPerMeterY());
            const int tolerance = depth == 8 ? 2 : 32;
            const auto sample = maxDifferenceSample(actual, expected, depth);
            const QPolygonF sourceQuad{{0, 0}, {double(source.width()), 0}, {double(source.width()), double(source.height())},
                                       {0, double(source.height())}};
            const QRect diagnosticBounds = destination.boundingRect().toAlignedRect();
            QPolygonF localDestination;
            for (const auto &point : destination)
                localDestination.append(point - QPointF(diagnosticBounds.topLeft()));
            QTransform local;
            QTransform::quadToQuad(sourceQuad, localDestination, local);
            bool inverseValid = false;
            const QTransform inverse = local.inverted(&inverseValid);
            const QPointF outputCenter = QPointF(sample.position) + QPointF(.5, .5);
            const QPointF mappedSource = inverseValid ? inverse.map(outputCenter) - QPointF(.5, .5) : QPointF();
            qInfo() << "PERSPECTIVE_DIAGNOSTIC" << "depth" << depth << "max" << sample.maximum << "pos"
                    << sample.position << "channel" << sample.channel << "actual" << sample.actual[0]
                    << sample.actual[1] << sample.actual[2] << sample.actual[3] << "expected" << sample.expected[0]
                    << sample.expected[1] << sample.expected[2] << sample.expected[3] << "interiorMax"
                    << interiorDifference(actual, expected, depth, destination,
                                          diagnosticBounds)
                    << "sourceCoordinate" << mappedSource;
            QVERIFY2(nativeDifference(actual, expected, depth) <= tolerance,
                     qPrintable(QString("perspective depth=%1 max native delta=%2")
                                    .arg(depth)
                                    .arg(nativeDifference(actual, expected, depth))));
        }
    }

    void sixteenBitTransparentOutsideAndMetadata() {
        const QImage source = fixture({33, 24}, 16);
        const QPolygonF destination{{4.5, 0.0}, {38.5, 2.0}, {34.0, 29.5}, {-1.5, 25.0}};
        const QImage actual = perspectiveGpuImage(source, destination);
        requireGpuOrSkip(actual, "Direct3D perspective GPU unavailable");
        QCOMPARE(actual.format(), QImage::Format_RGBA64_Premultiplied);
        QCOMPARE(actual.colorSpace(), source.colorSpace());
        QCOMPARE(actual.dotsPerMeterX(), source.dotsPerMeterX());
        QCOMPARE(actual.dotsPerMeterY(), source.dotsPerMeterY());
        // The output bounding rectangle contains pixels outside the transformed
        // quadrilateral.  They must remain transparent rather than edge-clamped.
        QVERIFY(pixel64(actual, 0, 0).alpha() == 0);
        QVERIFY(pixel64(actual, actual.width() - 1, 0).alpha() == 0);
    }

    void repeatedSizeAndStrictLowBudget() {
        const QPolygonF destination{{5.25, -2.5}, {49.75, 4.0}, {44.5, 36.25}, {-3.0, 29.5}};
        for (const int depth : {8, 16}) {
            const QImage source = fixture({37, 25}, depth);
            const QImage first = perspectiveGpuImage(source, destination);
            requireGpuOrSkip(first, "Direct3D perspective GPU unavailable");
            const QImage second = perspectiveGpuImage(source, destination);
            QVERIFY(!second.isNull());
            QCOMPARE(second, first);
        }

        // The callback receives zero when no transient bytes are available;
        // that is an explicit no-resource request, not an unlimited budget.
        ScopedEnvironment budget("COMPOSITOR_GPU_CACHE_MB", "1");
        clearGpuPreviewCache();
        resetGpuPreviewMetrics();
        for (const int depth : {8, 16}) {
            const QImage source = fixture({512, 512}, depth);
            QVERIFY(perspectiveGpuImage(source, destination).isNull());
            const auto metrics = gpuPreviewMetrics();
            QCOMPARE(metrics.processingDispatches, quint64(0));
            QCOMPARE(metrics.processingUploads, quint64(0));
            QCOMPARE(metrics.processingReadbackBytes, quint64(0));
        }
    }

    void optionalWarmMedianPerformanceReport() {
        if (!qEnvironmentVariableIntValue("COMPOSITOR_PERSPECTIVE_PERF"))
            QSKIP("set COMPOSITOR_PERSPECTIVE_PERF=1 to run the opt-in benchmark");

        const QPolygonF destination{{-7.25, 13.5}, {518.75, -4.0}, {536.0, 527.5}, {8.5, 541.25}};
        for (const int depth : {8, 16}) {
            const QImage source = fixture({512, 512}, depth);
            const QImage warmGpu = perspectiveGpuImage(source, destination);
            requireGpuOrSkip(warmGpu, "Direct3D perspective GPU unavailable");
            const QImage expected = perspectiveWarp(source, destination);
            const int tolerance = depth == 8 ? 2 : 32;
            const int warmDelta = reportDifference("warm", warmGpu, expected, depth, destination, source.size());
            QVERIFY2(warmDelta <= tolerance,
                     qPrintable(QString("perspective perf warm depth=%1 max native delta=%2")
                                    .arg(depth)
                                    .arg(warmDelta)));
            const QImage warmCpu = perspectiveWarp(source, destination);
            QCOMPARE(warmCpu, expected);

            QVector<qint64> gpuTimes;
            QVector<qint64> cpuTimes;
            for (int repeat = 0; repeat < 3; ++repeat) {
                QElapsedTimer timer;
                timer.start();
                const QImage gpu = perspectiveGpuImage(source, destination);
                gpuTimes.append(timer.nsecsElapsed());
                QVERIFY(!gpu.isNull());
                const int gpuDelta = reportDifference("sample", gpu, expected, depth, destination, source.size());
                QVERIFY2(gpuDelta <= tolerance,
                         qPrintable(QString("perspective perf sample depth=%1 max native delta=%2")
                                        .arg(depth)
                                        .arg(gpuDelta)));

                timer.restart();
                const QImage cpu = perspectiveWarp(source, destination);
                cpuTimes.append(timer.nsecsElapsed());
                QCOMPARE(cpu, expected);
            }
            qInfo() << "PERSPECTIVE_BENCHMARK" << "depth" << depth << "size" << source.size()
                    << "gpuMedianNs" << median(gpuTimes) << "cpuMedianNs" << median(cpuTimes)
                    << "warmSamples" << 3;
        }
    }

    void largeWhiteQuadCoverageMatchesCpu() {
        const QPolygonF destination{{-7.25, 13.5}, {518.75, -4.0}, {536.0, 527.5}, {8.5, 541.25}};
        for (const int depth : {8, 16}) {
            const QImage source = whiteFixture({512, 512}, depth);
            const QImage actual = perspectiveGpuImage(source, destination);
            requireGpuOrSkip(actual, "Direct3D perspective GPU unavailable");
            const QImage expected = perspectiveWarp(source, destination);
            QCOMPARE(actual.size(), expected.size());

            QPoint firstMismatch{-1, -1};
            int actualAlpha = 0;
            int expectedAlpha = 0;
            for (int y = 0; y < actual.height() && firstMismatch.x() < 0; ++y) {
                for (int x = 0; x < actual.width(); ++x) {
                    const int actualValue = depth == 16
                                                ? int(reinterpret_cast<const QRgba64 *>(actual.constScanLine(y))[x].alpha())
                                                : qAlpha(reinterpret_cast<const QRgb *>(actual.constScanLine(y))[x]);
                    const int expectedValue = depth == 16
                                                  ? int(reinterpret_cast<const QRgba64 *>(expected.constScanLine(y))[x].alpha())
                                                  : qAlpha(reinterpret_cast<const QRgb *>(expected.constScanLine(y))[x]);
                    // Coverage must agree exactly; Qt's 16-bit interpolator
                    // can turn opaque 65535 into 65533 on a white source.
                    if ((actualValue == 0) != (expectedValue == 0) ||
                        std::abs(actualValue - expectedValue) > (depth == 16 ? 4 : 0)) {
                        firstMismatch = {x, y};
                        actualAlpha = actualValue;
                        expectedAlpha = expectedValue;
                        break;
                    }
                }
            }
            QVERIFY2(firstMismatch.x() < 0,
                     qPrintable(QString("white coverage depth=%1 mismatch at (%2,%3): actual alpha=%4 expected=%5")
                                    .arg(depth)
                                    .arg(firstMismatch.x())
                                    .arg(firstMismatch.y())
                                    .arg(actualAlpha)
                                    .arg(expectedAlpha)));
        }
    }

    void strictRejectionAndReferenceErrors() {
        const QImage source = fixture({16, 12}, 8);
        QVERIFY(perspectiveGpuImage(source, QPolygonF{{0, 0}, {1, 1}, {2, 2}}).isNull());
        QVERIFY(perspectiveGpuImage(source, QPolygonF{{0, 0}, {qQNaN(), 1}, {16, 12}, {0, 12}}).isNull());
        const QPolygonF tooLarge{{0, 0}, {30001, 0}, {30001, 12}, {0, 12}};
        QVERIFY(perspectiveGpuImage(source, tooLarge).isNull());
        try {
            perspectiveWarpAccelerated(source, QPolygonF{{0, 0}, {1, 1}, {2, 2}});
            QFAIL("invalid homography was not rejected");
        } catch (const Error &error) {
            QCOMPARE(error.key, QString("invalidTransform"));
        }
        try {
            perspectiveWarpAccelerated(source, tooLarge);
            QFAIL("oversized perspective was not rejected");
        } catch (const Error &error) {
            QCOMPARE(error.key, QString("tooLarge"));
        }

        // The accelerated wrapper is also the safe CPU fallback when a device
        // is absent or COMPOSITOR_DISABLE_GPU is set; metadata must survive
        // either route.
        const QPolygonF valid{{0.5, -1.0}, {20.0, 0.0}, {18.0, 15.0}, {-1.0, 13.5}};
        const QImage accelerated = perspectiveWarpAccelerated(source, valid);
        QCOMPARE(accelerated.size(), valid.boundingRect().toAlignedRect().size());
        QCOMPARE(accelerated.format(), QImage::Format_ARGB32_Premultiplied);
        QCOMPARE(accelerated.colorSpace(), source.colorSpace());
        QCOMPARE(accelerated.dotsPerMeterX(), source.dotsPerMeterX());
        QCOMPARE(accelerated.dotsPerMeterY(), source.dotsPerMeterY());
    }
};

QTEST_MAIN(GpuPerspectiveTests)
#include "GpuPerspectiveTests.moc"
