#include "Export.h"
#include "GpuPreview.h"
#include "Pixels.h"
#include "Preferences.h"
#include "PreviewRenderer.h"
#include "Renderer.h"
#include "RetouchStroke.h"
#include "Selection.h"
#include "Updates.h"
#include "Warp.h"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPainter>
#include <QSettings>
#include <QSignalSpy>
#include <QSslSocket>
#include <QTest>
using namespace comp;
namespace {
struct Env {
    QByteArray name, old;
    bool set;
    Env(const char *n, const char *v) : name(n), old(qgetenv(n)), set(qEnvironmentVariableIsSet(n)) { qputenv(n, v); }
    ~Env() {
        if (set)
            qputenv(name, old);
        else
            qunsetenv(name);
    }
};
QImage fixture(QSize size, int depth) {
    QImage im(size, pixelFormat(depth));
    im.setColorSpace(QColorSpace::SRgb);
    im.setDotsPerMeterX(11811);
    im.setDotsPerMeterY(11811);
    const int maximum = depth == 16 ? 65535 : 255;
    for (int y = 0; y < size.height(); ++y)
        for (int x = 0; x < size.width(); ++x) {
            const int a = maximum * 3 / 4, r = (x * maximum / std::max(1, size.width() - 1)) * 3 / 4,
                      g = (y * maximum / std::max(1, size.height() - 1)) * 3 / 4,
                      b = (x + y) * maximum / (size.width() + size.height()) * 3 / 4;
            if (depth == 16)
                reinterpret_cast<QRgba64 *>(im.scanLine(y))[x] = rgba64(r, g, b, a);
            else
                reinterpret_cast<QRgb *>(im.scanLine(y))[x] = qRgba(r, g, b, a);
        }
    return im;
}
int difference(QImage a, QImage b) {
    a = a.convertToFormat(QImage::Format_RGBA64_Premultiplied);
    b = b.convertToFormat(QImage::Format_RGBA64_Premultiplied);
    int max = 0;
    QPoint worst;
    for (int y = 0; y < a.height(); ++y) {
        auto pa = reinterpret_cast<const quint16 *>(a.constScanLine(y));
        auto pb = reinterpret_cast<const quint16 *>(b.constScanLine(y));
        for (int x = 0; x < a.width() * 4; ++x)
            if (std::abs(int(pa[x]) - pb[x]) > max) {
                max = std::abs(int(pa[x]) - pb[x]);
                worst = {x / 4, y};
            }
    }
    if (max > 5000)
        qInfo() << "WORST" << worst << a.pixelColor(worst) << b.pixelColor(worst);
    return max;
}
State state(QSize size, int depth) {
    State s;
    s.id = newId();
    s.size = size;
    s.bitDepth = depth;
    Layer l;
    l.id = newId();
    l.image = fixture(QSize(97, 79), depth);
    l.placement.size = size;
    s.layers.append(l);
    s.active = l.id;
    return s;
}
QJsonObject release(QString ver, bool pre = true) {
    const QString file = "Compositor-" + ver + "-win-x64.zip", tag = "windows-v" + ver;
    return {{"tag_name", tag},
            {"prerelease", pre},
            {"draft", false},
            {"assets", QJsonArray{QJsonObject{{"name", file},
                                              {"size", 1234},
                                              {"digest", "sha256:" + QString(64, 'a')},
                                              {"browser_download_url",
                                               "https://github.com/Hoshino-Rubi/Compositor-Windows/releases/download/" +
                                                   tag + "/" + file}}}}};
}
} // namespace
class CompletionTests : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        Q_INIT_RESOURCE(translations);
        QCoreApplication::setOrganizationName("CompositorTests");
        QCoreApplication::setApplicationName("CompletionTests");
    }
    void retiredEditingCannotBeReenabledByOldSettings() {
        QSettings s;
        s.setValue("rendering/gpuEditingExperimental", true);
        QVERIFY(!Preferences::gpuEditingEnabled());
        QVERIFY(!Preferences::load().gpuEditingExperimental);
        Preferences::load().save();
        QVERIFY(!s.contains("rendering/gpuEditingExperimental"));
    }
    void statefulRetouchCpuGpuParityAndTransfers() {
        for (int depth : {8, 16})
            for (bool smudge : {false, true}) {
                const auto source = fixture({257, 193}, depth);
                QImage cpu = source, gpu = source;
                QImage mask(source.size(), maskFormat(depth));
                mask.fill(Qt::white);
                for (int y = 0; y < mask.height(); ++y)
                    for (int x = 0; x < 40; ++x) {
                        if (depth == 16)
                            reinterpret_cast<quint16 *>(mask.scanLine(y))[x] = 0;
                        else
                            mask.scanLine(y)[x] = 0;
                    }
                std::unique_ptr<RetouchStroke> reference;
                {
                    Env disabled("COMPOSITOR_DISABLE_GPU", "1");
                    reference = std::make_unique<RetouchStroke>(source, 70, .35, .65, smudge, mask);
                }
                auto accelerated = std::make_unique<RetouchStroke>(source, 70, .35, .65, smudge, mask);
                for (int segment = 0; segment < 3; ++segment) {
                    QVector<QPointF> points;
                    for (int i = 0; i < 7; ++i)
                        points.append({double(70 + segment * 18 + i * 3), double(90 + segment * 6 + i)});
                    {
                        Env disabled("COMPOSITOR_DISABLE_GPU", "1");
                        reference->append(cpu, points);
                    }
                    resetGpuPreviewMetrics();
                    accelerated->append(gpu, points);
                    const auto m = gpuPreviewMetrics();
                    if (qEnvironmentVariableIntValue("COMPOSITOR_REQUIRE_GPU"))
                        QVERIFY(m.processingDispatches > 0);
                    if (m.processingDispatches) {
                        QCOMPARE(m.nativeReadbackOperations, quint64(1));
                        if (segment)
                            QCOMPARE(m.processingUploadedBytes, quint64(0));
                    }
                    const int delta = difference(cpu, gpu);
                    qInfo() << "STROKE_PARITY" << depth << smudge << segment << delta;
                    QVERIFY(delta <= (depth == 8 ? 257 : 16));
                }
                QCOMPARE(gpu.copy(0, 0, 40, gpu.height()), source.copy(0, 0, 40, source.height()));
                QCOMPARE(gpu.colorSpace(), source.colorSpace());
                QCOMPARE(gpu.dotsPerMeterX(), source.dotsPerMeterX());
                // Mid-gesture device loss replays from the original, not partial GPU state.
                {
                    Env disabled("COMPOSITOR_DISABLE_GPU", "1");
                    const QVector<QPointF> next{{124, 108}, {129, 110}};
                    reference->append(cpu, next);
                    accelerated->append(gpu, next);
                }
                QCOMPARE(gpu, cpu);
            }
    }
    void statefulRetouchEdgesBudgetAndLifecycle() {
        for (int depth : {8, 16})
            for (bool smudge : {false, true}) {
                const auto source = fixture({129, 97}, depth);
                QVector<QPointF> points{{-5.5, -3.5}, {-.5, -.5}};
                for (int i = 1; i <= 12; ++i)
                    points.append({i * 2.75, i * 2.25});
                QImage cpu = source, gpu = source;
                {
                    Env disabled("COMPOSITOR_DISABLE_GPU", "1");
                    RetouchStroke s(source, 65, .3, .7, smudge);
                    s.append(cpu, points);
                }
                clearGpuPreviewCache();
                const auto before = gpuPreviewMetrics().externalBytes;
                {
                    RetouchStroke s(source, 65, .3, .7, smudge);
                    s.append(gpu, points);
                    if (qEnvironmentVariableIntValue("COMPOSITOR_REQUIRE_GPU"))
                        QVERIFY(gpuPreviewMetrics().externalBytes > before);
                }
                QCOMPARE(gpuPreviewMetrics().externalBytes, before);
                QVERIFY(difference(cpu, gpu) <= (depth == 8 ? 257 : 16));
                {
                    Env budget("COMPOSITOR_GPU_CACHE_MB", "1");
                    RetouchStroke s(source, 65, .3, .7, smudge);
                    QImage actual = source;
                    s.append(actual, points);
                    QVERIFY(difference(cpu, actual) <= (depth == 8 ? 257 : 16));
                }
            }
    }
    void highDepthRetouchPreservesFractionAtLargeCoordinates() {
        auto source = fixture({8193, 129}, 16);
        for (int y = 0; y < source.height(); ++y)
            for (int x = 0; x < source.width(); ++x)
                reinterpret_cast<QRgba64 *>(source.scanLine(y))[x] =
                    rgba64((x * 997 + y * 179) % 65536, (x * 313 + y * 127) % 65536, 10001, 65535);
        const QVector<QPointF> points{{7500.125, 60.25}, {7501.375, 60.5}, {7502.625, 60.75}, {7503.875, 61},
                                      {7505.125, 61.25}, {7506.375, 61.5}, {7507.625, 61.75}, {7508.875, 62},
                                      {7510.125, 62.25}, {7511.375, 62.5}, {7512.625, 62.75}, {7513.875, 63}};
        auto cpu = source, gpu = source;
        {
            Env disabled("COMPOSITOR_DISABLE_GPU", "1");
            RetouchStroke stroke(source, 80, .3, .7, false);
            stroke.append(cpu, points);
        }
        resetGpuPreviewMetrics();
        RetouchStroke stroke(source, 80, .3, .7, false);
        stroke.append(gpu, points);
        if (qEnvironmentVariableIntValue("COMPOSITOR_REQUIRE_GPU"))
            QVERIFY(gpuPreviewMetrics().processingDispatches > 0);
        qInfo() << "LARGE_COORDINATE_NATIVE_DELTA" << difference(cpu, gpu);
        QVERIFY(difference(cpu, gpu) <= 16);
    }
    void liquifyReversalRetainsDetail() {
        Env disabled("COMPOSITOR_DISABLE_GPU", "1");
        for (int depth : {8, 16}) {
            QImage source(300, 120, pixelFormat(depth));
            source.fill(Qt::white);
            {
                QPainter p(&source);
                for (int x = 0; x < 300; x += 4)
                    p.fillRect(x, 0, 2, 120, Qt::black);
            }
            QImage accumulated = source, repeated = source;
            RetouchStroke stroke(source, 30, .3, .7, false);
            QVector<QPointF> points{{60, 60}};
            for (int x = 63; x <= 201; x += 3)
                points.append({double(x), 60});
            for (int x = 198; x >= 60; x -= 3)
                points.append({double(x), 60});
            stroke.append(accumulated, points);
            for (qsizetype i = 1; i < points.size(); ++i)
                warpDab(repeated, points[i - 1], points[i], 30, .3, .7, false);
            auto contrast = [](const QImage &im) {
                qint64 v = 0;
                for (int x = 40; x < 225; ++x)
                    v += std::abs(im.pixelColor(x, 60).red() - im.pixelColor(x + 1, 60).red());
                return v;
            };
            qInfo() << "LIQUIFY_CONTRAST" << depth << contrast(accumulated) << contrast(repeated);
            QVERIFY(contrast(accumulated) > contrast(repeated) * 2);
        }
    }
    void smudgeCarriesOneFadingTrail() {
        Env disabled("COMPOSITOR_DISABLE_GPU", "1");
        QImage im(300, 100, pixelFormat(8));
        im.fill(QColor(20, 20, 20));
        {
            QPainter p(&im);
            p.fillRect(55, 43, 10, 14, Qt::white);
        }
        RetouchStroke stroke(im, 20, .5, .6, true);
        QVector<QPointF> points{{60, 50}};
        for (int x = 61; x <= 240; ++x)
            points.append({double(x), 50});
        stroke.append(im, points);
        int peaks = 0;
        for (int x = 80; x < 239; ++x)
            if (im.pixelColor(x, 50).red() > im.pixelColor(x - 1, 50).red() + 2)
                ++peaks;
        QCOMPARE(peaks, 0);
        QVERIFY(im.pixelColor(65, 50).red() > im.pixelColor(235, 50).red() + 20);
    }
    void globalPreviewPanReusesComposite() {
        Env disabled("COMPOSITOR_DISABLE_GPU", "1");
        auto s = state({1100, 700}, 8);
        Layer adjustment;
        adjustment.id = newId();
        adjustment.metadata["adjustment"] = QJsonObject{{"kind", "Add Noise"}, {"amount", 12}};
        s.layers.append(adjustment);
        auto first = renderCanvasPreview(s, {0, 0, 500, 500}, {200, 127}, true, 1, {}, {});
        QVERIFY(!first.fullReference.isNull());
        const auto key = first.fullReference.cacheKey();
        auto pan = renderCanvasPreview(s, {700, 0, 400, 500}, {200, 127}, true, 1, first, {});
        QCOMPARE(pan.fullReference.cacheKey(), key);
        auto changed = renderCanvasPreview(s, {700, 0, 400, 500}, {200, 127}, true, 2, pan, {});
        QVERIFY(changed.fullReference.cacheKey() != key);
        s.id = newId();
        auto other = renderCanvasPreview(s, {0, 0, 500, 500}, {200, 127}, true, 2, changed, {});
        QVERIFY(other.fullReference.cacheKey() != changed.fullReference.cacheKey());
    }
    void referencePreviewMatchesCpuWithoutGpu() {
        auto s = state({512, 512}, 16);
        Layer adjustment;
        adjustment.id = newId();
        adjustment.metadata["adjustment"] = QJsonObject{{"kind", "Exposure"}, {"exposure", .3}};
        s.layers.append(adjustment);
        resetGpuPreviewMetrics();
        const auto frame = renderCanvasPreview(s, {0, 0, 512, 512}, {512, 512}, false, 1, {}, {}, true, true);
        QCOMPARE(frame.overview, render(s));
        QVERIFY(!frame.overviewTexture);
        QCOMPARE(gpuPreviewMetrics().dispatches, quint64(0));
        QCOMPARE(gpuPreviewMetrics().readbacks, quint64(0));
    }
    void largeReferenceExportTilesWithoutGpu() {
        for (int depth : {8, 16}) {
            auto s = state({3073, 2731}, depth);
            s.layers[0].image = fixture({1, 1}, depth);
            s.layers[0].image.fill(QColor(51, 102, 153));
            resetGpuPreviewMetrics();
            const auto actual = renderExportImage(s, s.size, ExportRenderer::ReferenceCpu);
            QCOMPARE(gpuPreviewMetrics().processingDispatches, quint64(0));
            QCOMPARE(gpuPreviewMetrics().dispatches, quint64(0));
            QCOMPARE(actual, render(s));
            if (depth == 8) {
                s.layers[0].image = fixture({97, 79}, depth);
                s.layers[0].placement.size = {1850.5, 1730.25};
                s.layers[0].placement.origin = {217.25, 188.5};
                s.layers[0].placement.rotation = 13.5;
                s.layers[0].metadata["effects"] =
                    QJsonObject{{"shadow", QJsonObject{{"blur", 4}, {"distance", 7}, {"opacity", .7}}}};
                const auto tiled = renderExportImage(s, s.size, ExportRenderer::ReferenceCpu), full = render(s);
                qInfo() << "TILED_EXPORT_TRANSFORM_DELTA" << difference(tiled, full);
                QCOMPARE(tiled, full);
            }
        }
    }
    void connectedSelectionRejectsBackgroundAndOtherObjects() {
        QImage mask(41, 23, QImage::Format_Grayscale8);
        mask.fill(0);
        for (int y = 3; y < 18; ++y)
            for (int x = 2; x < 13; ++x)
                mask.scanLine(y)[x] = 200;
        for (int y = 3; y < 18; ++y)
            for (int x = 24; x < 37; ++x)
                mask.scanLine(y)[x] = 255;
        const auto first = connectedObjectMask(mask, {5, 8});
        QCOMPARE(first.constScanLine(8)[5], uchar(200));
        QCOMPARE(first.constScanLine(8)[26], uchar(0));
        const auto empty = connectedObjectMask(mask, {19, 8});
        QCOMPARE(empty.constScanLine(8)[5], uchar(0));
        const auto invalid = connectedObjectMask(mask, {-1, 8});
        QCOMPARE(invalid, empty);
    }
    void subjectInferenceRespectsTransparentHighDepthPixels() {
        auto image = fixture({73, 257}, 16);
        for (int y = 0; y < 70; ++y)
            for (int x = 0; x < 73; ++x)
                reinterpret_cast<QRgba64 *>(image.scanLine(y))[x] = rgba64(0, 0, 0, 0);
        QImage narrow(1,2000,pixelFormat(16));narrow.fill(Qt::transparent);
        const auto narrowMask=subjectMask(narrow);QCOMPARE(narrowMask.size(),narrow.size());
        const auto mask = subjectMask(image);
        QCOMPARE(mask.size(), image.size());
        for (int y = 0; y < 70; ++y)
            for (int x = 0; x < 73; ++x)
                QCOMPARE(mask.constScanLine(y)[x], uchar(0));
        for (int y = 70; y < 257; ++y)
            for (int x = 0; x < 73; ++x)
                QVERIFY(mask.constScanLine(y)[x] <= 192);
    }
    void updatesValidateVersionChannelAndDownload() {
        const auto json =
            QJsonDocument(QJsonArray{release("0.4.1a10"), release("0.4.1a9"), release("0.4.0", false)}).toJson();
        QVERIFY(!findUpdate(json, "0.4.0"));
        auto info = findUpdate(json, "0.4.1a8");
        QVERIFY(info);
        QCOMPARE(info->version, QString("0.4.1a10"));
        QVERIFY(!findUpdate(json, "0.4.1a10"));
        QVERIFY(!findUpdate(json, "invalid"));
        auto stable = QJsonDocument(QJsonArray{release("0.4.1", false), release("0.4.2a1")}).toJson();
        QCOMPARE(findUpdate(stable, "0.4.0")->version, QString("0.4.1"));
        for (const QString key : {"digest", "browser_download_url", "name", "size"}) {
            auto bad = release("0.4.1a9");
            auto asset = bad["assets"].toArray()[0].toObject();
            asset.remove(key);
            bad["assets"] = QJsonArray{asset};
            QVERIFY(!findUpdate(QJsonDocument(QJsonArray{bad}).toJson(), "0.4.1a8"));
        }
        auto bad = release("0.4.1a9");
        auto asset = bad["assets"].toArray()[0].toObject();
        asset["browser_download_url"] = "https://example.com/Compositor.zip";
        bad["assets"] = QJsonArray{asset};
        QVERIFY(!findUpdate(QJsonDocument(QJsonArray{bad}).toJson(), "0.4.1a8"));
    }
    void updateTlsBackendAndLiveMetadata() {
#ifdef Q_OS_WIN
        QVERIFY(QSslSocket::availableBackends().contains("schannel"));
#endif
        QVERIFY(QSslSocket::supportsSsl());
        if (!qEnvironmentVariableIntValue("COMPOSITOR_UPDATE_NETWORK_TEST"))
            return;
        QNetworkAccessManager manager;
        QNetworkRequest request(
            QUrl("https://api.github.com/repos/Hoshino-Rubi/Compositor-Windows/releases?per_page=10"));
        request.setRawHeader("User-Agent", "Compositor-Update-Verification");
        request.setTransferTimeout(20000);
        auto reply = manager.get(request);
        QSignalSpy done(reply, &QNetworkReply::finished);
        if (!reply->isFinished())
            QVERIFY(done.wait(25000));
        if (reply->error() != QNetworkReply::NoError)
            qInfo() << "UPDATE_HTTP_ERROR" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                    << reply->readAll().left(800);
        QCOMPARE(reply->error(), QNetworkReply::NoError);
        const auto info = findUpdate(reply->readAll(), "0.4.1a1");
        QVERIFY(info);
        QVERIFY(info->size > 0);
        qInfo() << "UPDATE_TLS_METADATA" << QSslSocket::activeBackend() << info->version << info->size;
    }
    void statefulRetouchPerformance() {
        if (!qEnvironmentVariableIntValue("COMPOSITOR_GPU_BENCHMARK"))
            QSKIP("Opt-in benchmark");
        for (int depth : {8, 16})
            for (bool smudge : {false, true}) {
                auto original = fixture({512, 512}, depth);
                QVector<qint64> timings[2];
                for (int cpu = 0; cpu < 2; ++cpu)
                    for (int run = 0; run < 3; ++run) {
                        Env disabled("COMPOSITOR_DISABLE_GPU", cpu ? "1" : "0");
                        QImage image = original;
                        RetouchStroke stroke(original, 160, .3, .7, smudge);
                        stroke.append(image, {{160, 256}, {163, 256}});
                        QElapsedTimer timer;
                        timer.start();
                        for (int segment = 0; segment < 6; ++segment) {
                            QVector<QPointF> points;
                            for (int i = 0; i < 8; ++i)
                                points.append({double(163 + segment * 14 + i * 2), 256});
                            stroke.append(image, points);
                        }
                        timings[cpu].append(timer.nsecsElapsed());
                    }
                for (auto &v : timings)
                    std::sort(v.begin(), v.end());
                qInfo() << "STROKE_BENCHMARK" << depth << smudge << "gpuNs" << timings[0][1] << "cpuNs"
                        << timings[1][1];
            }
    }
};
QTEST_MAIN(CompletionTests)
#include "CompletionTests.moc"
