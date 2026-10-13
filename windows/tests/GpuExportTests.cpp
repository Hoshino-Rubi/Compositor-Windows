#include "Export.h"
#include "GpuPreview.h"
#include "Pixels.h"
#include "Renderer.h"
#include <QColorSpace>
#include <QFileInfo>
#include <QImageReader>
#include <QTemporaryDir>
#include <QTest>
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
    QByteArray name_, old_;
    bool had_ = false;
};

State exportFixture(QSize size, int depth, bool solid = false) {
    State state;
    state.id = newId();
    state.size = size;
    state.resolution = 300;
    state.bitDepth = depth;
    Layer layer;
    layer.id = newId();
    layer.name = "GPU export fixture";
    layer.image = QImage(size, pixelFormat(depth));
    layer.image.setColorSpace(QColorSpace::SRgb);
    layer.image.setDotsPerMeterX(qRound(state.resolution / .0254));
    layer.image.setDotsPerMeterY(qRound(state.resolution / .0254));
    const int maximum = depth == 16 ? 65535 : 255;
    if (solid) {
        if (depth == 16)
            reinterpret_cast<QRgba64 *>(layer.image.bits())[0] = rgba64(maximum / 3, maximum / 2, maximum * 3 / 4, maximum);
        else
            reinterpret_cast<QRgb *>(layer.image.bits())[0] = qRgba(85, 127, 191, 255);
        layer.image = layer.image.copy(0, 0, 1, 1);
        layer.placement.size = size;
    } else {
        for (int y = 0; y < size.height(); ++y) {
            for (int x = 0; x < size.width(); ++x) {
                const int alpha = maximum;
                const int red = (x * 313 + y * 17) % (maximum + 1);
                const int green = (x * 71 + y * 541) % (maximum + 1);
                const int blue = (x * 997 + y * 31) % (maximum + 1);
                if (depth == 16)
                    reinterpret_cast<QRgba64 *>(layer.image.scanLine(y))[x] = rgba64(red, green, blue, alpha);
                else
                    reinterpret_cast<QRgb *>(layer.image.scanLine(y))[x] = qRgba(red, green, blue, alpha);
            }
        }
        layer.placement.size = size;
    }
    state.layers.append(layer);
    state.active = layer.id;
    return state;
}

int maximumDifference(const QImage &left, const QImage &right) {
    if (left.size() != right.size()) return 65535;
    const auto a = left.convertToFormat(QImage::Format_RGBA64_Premultiplied);
    const auto b = right.convertToFormat(QImage::Format_RGBA64_Premultiplied);
    int maximum = 0;
    for (int y = 0; y < a.height(); ++y) {
        const auto *pa = reinterpret_cast<const quint16 *>(a.constScanLine(y));
        const auto *pb = reinterpret_cast<const quint16 *>(b.constScanLine(y));
        for (int x = 0; x < a.width() * 4; ++x)
            maximum = std::max(maximum, std::abs(int(pa[x]) - int(pb[x])));
    }
    return maximum;
}

void requireGpuOrSkip(const char *where) {
    const auto metrics = gpuPreviewMetrics();
    if (metrics.dispatches || metrics.adjustmentDispatches || metrics.processingDispatches) return;
    const auto message = QString("%1: %2").arg(QString::fromLatin1(where), gpuPreviewStatus());
    if (qEnvironmentVariableIntValue("COMPOSITOR_REQUIRE_GPU"))
        QFAIL(qPrintable(message));
    QSKIP(qPrintable(message));
}

} // namespace

class GpuExportTests : public QObject {
    Q_OBJECT

  private slots:
    void gpuExportPreservesDepthAndMetadata() {
        for (const int depth : {8, 16}) {
            const auto state = exportFixture({384, 257}, depth);
            resetGpuPreviewMetrics();
            const auto actual = renderExportImage(state, state.size, ExportRenderer::AcceleratedGpu);
            requireGpuOrSkip(depth == 8 ? "8-bit export" : "16-bit export");
            QVERIFY(!actual.isNull());
            QCOMPARE(actual.size(), state.size);
            const auto expected = render(state, state.size);
            QCOMPARE(actual.colorSpace(), expected.colorSpace());
            QCOMPARE(actual.dotsPerMeterX(), qRound(state.resolution / .0254));
            QCOMPARE(actual.dotsPerMeterY(), qRound(state.resolution / .0254));
            QCOMPARE(actual.pixelFormat().redSize(), depth);
            QVERIFY2(maximumDifference(actual, expected) <= (depth == 8 ? 1 : 16),
                     qPrintable(QString("depth=%1 delta=%2").arg(depth).arg(maximumDifference(actual, expected))));
        }
    }

    void gpuExportTilesBeyondEightMegapixels() {
        const auto state = exportFixture({3072, 3072}, 8, true);
        resetGpuPreviewMetrics();
        const auto actual = renderExportImage(state, state.size, ExportRenderer::AcceleratedGpu);
        requireGpuOrSkip("tiled export");
        QVERIFY(!actual.isNull());
        const auto metrics = gpuPreviewMetrics();
        QVERIFY2(metrics.dispatches > 1, qPrintable(QString("only %1 GPU dispatches").arg(metrics.dispatches)));
        QCOMPARE(actual.pixelColor(0, 0), QColor(85, 127, 191));
        QCOMPARE(actual.pixelColor(1536, 1536), QColor(85, 127, 191));
        QCOMPARE(actual.pixelColor(3071, 3071), QColor(85, 127, 191));
    }

    void acceleratedExportFallsBackToReferenceWhenGpuDisabled() {
        const auto state = exportFixture({384, 257}, 16);
        const auto expected = renderExportImage(state, state.size, ExportRenderer::ReferenceCpu);
        ScopedEnvironment disabled("COMPOSITOR_DISABLE_GPU", "1");
        resetGpuPreviewMetrics();
        const auto actual = renderExportImage(state, state.size, ExportRenderer::AcceleratedGpu);
        QCOMPARE(actual, expected);
        QCOMPARE(gpuPreviewMetrics().dispatches, quint64(0));
        QCOMPARE(gpuPreviewMetrics().readbacks, quint64(0));
    }

    void gpuExportPngSaveRetainsPixelsAndDpi() {
        const auto state = exportFixture({384, 257}, 16);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("gpu-export.png");
        resetGpuPreviewMetrics();
        exportDocument(state, path, "png", state.size, 92, ExportRenderer::AcceleratedGpu);
        requireGpuOrSkip("PNG export");
        QVERIFY(QFileInfo::exists(path));
        QImageReader reader(path);
        const auto actual = reader.read();
        QVERIFY2(!actual.isNull(), qPrintable(reader.errorString()));
        const auto expected = renderExportImage(state, state.size, ExportRenderer::ReferenceCpu);
        QVERIFY(maximumDifference(actual, expected) <= 16);
        QCOMPARE(actual.dotsPerMeterX(), qRound(state.resolution / .0254));
        QCOMPARE(actual.dotsPerMeterY(), qRound(state.resolution / .0254));
        QCOMPARE(actual.pixelFormat().redSize(), 16);
    }
};

QTEST_MAIN(GpuExportTests)
#include "GpuExportTests.moc"
