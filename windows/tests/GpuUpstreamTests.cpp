#include "CameraRaw.h"
#include "CameraRawTables.h"
#include "GpuPreview.h"
#include "Importers.h"
#include "MainWindow.h"
#include "Pixels.h"
#include "WideKernels.h"
#include <QAction>
#include <QCheckBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QJsonDocument>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <cmath>
#include <vector>
extern "C" {
#include "PatchPixels.h"
#include "HealPixels.h"
}
using namespace comp;
namespace {
QImage pixels(int depth) {
    QImage image(96, 80, depth == 16 ? QImage::Format_RGBA64_Premultiplied : QImage::Format_RGBA8888_Premultiplied);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const auto color = QColor::fromRgba64(10001 + x * 73, 14003 + y * 59, 23009 + (x + y) * 17, 65535);
            image.setPixelColor(x, y, color);
        }
    return image;
}
State document(int depth) {
    State s; s.id = newId(); s.bitDepth = depth;
    Layer l; l.id = newId(); l.name = "Layer"; l.image = pixels(depth);
    s.size = l.image.size(); l.placement.size = s.size;
    s.layers.append(l); s.active = l.id; return s;
}
void manifest(const QString &directory, const std::function<void(QJsonObject &)> &change) {
    QFile f(directory + "/manifest.json");
    if (!f.open(QIODevice::ReadOnly)) qFatal("Missing fixture manifest");
    auto json = QJsonDocument::fromJson(f.readAll()).object(); f.close(); change(json);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) qFatal("Cannot rewrite fixture");
    f.write(QJsonDocument(json).toJson());
}
int patch(QImage &image, const QImage &mask, int depth) {
    return depth == 16 ? wide::patch_fill(reinterpret_cast<quint16 *>(image.bits()), image.bytesPerLine() / 2,
                                          mask.constBits(), mask.bytesPerLine(), image.width(), image.height(), 123)
                       : patch_fill(image.bits(), image.bytesPerLine(), mask.constBits(), mask.bytesPerLine(), image.width(), image.height(), 123);
}
bool rejects(const QString &key, const std::function<void()> &operation) {
    try { operation(); } catch (const Error &e) { return e.key == key; }
    return false;
}
}
class GpuUpstreamTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        Q_INIT_RESOURCE(translations); Q_INIT_RESOURCE(icons); Q_INIT_RESOURCE(controls);
        QCoreApplication::setOrganizationName("CompositorTests");
        QCoreApplication::setApplicationName("UpstreamTests");
    }
    void upstreamPngProjectsInferDepthAndPreserveOriginalBytes() {
        for (int depth : {8, 16}) for (int version : {12, 13, 14}) {
            QTemporaryDir dir; QVERIFY(dir.isValid());
            const auto input = document(depth);
            const auto path = dir.path() + "/input.comp";
            writeProject(path, input);
            manifest(path, [=](QJsonObject &j) { j["version"] = version; j.remove("bitDepth"); });
            const auto loaded = readProject(path);
            QCOMPARE(loaded.state.bitDepth, depth);
            QCOMPARE(loaded.state.layers[0].image, input.layers[0].image.convertToFormat(pixelFormat(depth)));
            QVERIFY(!loaded.state.layers[0].originalPng.isEmpty());
            const auto copy = dir.path() + "/copy.comp";
            writeProject(copy, loaded.state);
            const auto reopened = readProject(copy);
            QCOMPARE(reopened.state.layers[0].originalPng, loaded.state.layers[0].originalPng);
            QCOMPARE(reopened.state.layers[0].image, loaded.state.layers[0].image);
        }
    }
    void unsupportedHdrSmoothingAndFutureProjectsAreExplicitlyRejected() {
        QTemporaryDir dir; const auto path = dir.path() + "/file.comp";
        const auto input = document(16); writeProject(path, input);
        manifest(path, [](QJsonObject &j) {
            j["version"] = 13; auto layers = j["layers"].toArray(); auto l = layers[0].toObject();
            l["imageFile"] = l["id"].toString() + ".exr"; layers[0] = l; j["layers"] = layers;
        });
        QVERIFY(rejects("unsupportedFeature", [&] { readProject(path); }));
        const auto other = dir.path() + "/shape.comp"; writeProject(other, input);
        manifest(other, [](QJsonObject &j) {
            j["version"] = 14; auto layers = j["layers"].toArray(); auto l = layers[0].toObject();
            l["shape"] = QJsonObject{{"kind", "Rectangle"}, {"smoothing", true}}; layers[0] = l; j["layers"] = layers;
        });
        QVERIFY(rejects("unsupportedFeature", [&] { readProject(other); }));
        manifest(other, [](QJsonObject &j) { j["version"] = 15; });
        QVERIFY(rejects("unsupportedVersion", [&] { readProject(other); }));
    }
    void patchMatchPreservesOutsideAndSixteenBitSamples() {
        for (int depth : {8, 16}) {
            QImage image = pixels(depth); const QRect hole(40, 30, 12, 10);
            for (int y = hole.top(); y <= hole.bottom(); ++y)
                for (int x = hole.left(); x <= hole.right(); ++x) image.setPixelColor(x, y, Qt::red);
            const auto before = image;
            QImage mask(image.size(), QImage::Format_Grayscale8); mask.fill(Qt::black);
            for (int y = hole.top(); y <= hole.bottom(); ++y)
                for (int x = hole.left(); x <= hole.right(); ++x) mask.scanLine(y)[x] = 255;
            QCOMPARE(patch(image, mask, depth), 1);
            QVERIFY(image != before);
            bool nativePrecision = false;
            for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
                if (!hole.contains(x, y)) QCOMPARE(image.pixelColor(x, y), before.pixelColor(x, y));
                else if (depth == 16) nativePrecision |= image.pixelColor(x, y).rgba64().red() % 257 != 0;
            }
            if (depth == 16) QVERIFY(nativePrecision);
            auto repeat = before; QCOMPARE(patch(repeat, mask, depth), 1); QCOMPARE(repeat, image);
            mask.fill(Qt::white); repeat = before;
            QCOMPARE(patch(repeat, mask, depth), 0); QCOMPARE(repeat, before);
        }
    }
    void contentAwareHealingUsesPatchMatchWithCoverageAndOpacity() {
        for (int depth : {8, 16}) {
            const auto source = pixels(depth); auto filled = source, actual = source;
            QImage mask(source.size(), QImage::Format_Grayscale8); mask.fill(Qt::black);
            for (int y = 30; y < 40; ++y) for (int x = 40; x < 52; ++x) mask.scanLine(y)[x] = 128;
            QCOMPARE(patch(filled, mask, depth), 1);
            const float opacity = .65f;
            const int status = depth == 16
                ? wide::spot_heal(reinterpret_cast<quint16 *>(actual.bits()), mask.constBits(), source.width(), source.height(), actual.bytesPerLine() / 2, opacity, 0, 123)
                : spot_heal(actual.bits(), mask.constBits(), source.width(), source.height(), actual.bytesPerLine(), opacity, 0, 123);
            QCOMPARE(status, 0);
            for (int y = 0; y < actual.height(); ++y) for (int x = 0; x < actual.width(); ++x) {
                if (!mask.constScanLine(y)[x]) { QCOMPARE(actual.pixelColor(x, y), source.pixelColor(x, y)); continue; }
                const int maximum = depth == 16 ? 65535 : 255;
                const double amount = mask.constScanLine(y)[x] / 255.0 * opacity;
                const auto a = source.pixelColor(x, y), b = filled.pixelColor(x, y), c = actual.pixelColor(x, y);
                QCOMPARE(qRound(c.redF() * maximum), qRound((a.redF() + (b.redF() - a.redF()) * amount) * maximum));
            }
        }
    }
    void adaptiveShadowsUseNativePrecisionAndReferenceRouting() {
        for (int depth : {8, 16}) {
            const auto source = pixels(depth);
            const QJsonObject settings{{"kind", "Camera Raw"}, {"shadows", 75}};
            const auto expected = cameraRaw(source, settings);
            QVERIFY(expected.convertToFormat(source.format()) != source);
            resetGpuPreviewMetrics();
            QCOMPARE(filteredAccelerated(source, settings), expected);
            QCOMPARE(gpuPreviewMetrics().processingDispatches, quint64(0));
            QCOMPARE(gpuPreviewMetrics().processingUploads, quint64(0));
            QCOMPARE(gpuPreviewMetrics().readbacks, quint64(0));
            QCOMPARE(wideImage(expected), depth == 16);
            bool nativePrecision = false;
            for (int y = 0; y < source.height(); ++y) for (int x = 0; x < source.width(); ++x) {
                QCOMPARE(expected.pixelColor(x,y).alpha(), source.pixelColor(x,y).alpha());
                if (depth == 16) nativePrecision |= expected.pixelColor(x,y).rgba64().red() % 257 != 0;
            }
            if (depth == 16) QVERIFY(nativePrecision);
            QVERIFY(cameraRaw(source, settings, .05) != expected);
            auto disabled = settings; disabled["enableLight"] = false;
            QCOMPARE(cameraRaw(source, disabled), cameraRaw(source, {}));
        }
    }
    void adaptiveShadowsMatchesUpstreamLandscapeReference() {
        // Same fixed-seed landscape and measured tolerances as upstream CameraRawTests.swift.
        auto field = [](int width, int height, quint64 seed) {
            std::vector<double> out(size_t(width) * height);
            double amplitude = 1, total = 0;
            for (int octave = 0; octave < 7; ++octave) {
                const int cell = std::max(1, std::max(width, height) >> (octave + 2));
                const int gw = width / cell + 2, gh = height / cell + 2;
                std::vector<double> grid(size_t(gw) * gh);
                for (auto &v : grid) { seed = seed * 6364136223846793005ull + 1442695040888963407ull; v = double(seed >> 11) / 9007199254740992.; }
                for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                    const double fx = double(x) / cell, fy = double(y) / cell;
                    const int x0 = int(fx), y0 = int(fy); const double tx = fx - x0, ty = fy - y0;
                    const double a = grid[y0 * gw + x0] * (1 - tx) + grid[y0 * gw + x0 + 1] * tx;
                    const double b = grid[(y0 + 1) * gw + x0] * (1 - tx) + grid[(y0 + 1) * gw + x0 + 1] * tx;
                    out[y * width + x] += amplitude * (a * (1 - ty) + b * ty);
                }
                total += amplitude; amplitude *= .6;
            }
            for (auto &v : out) v /= total;
            return out;
        };
        for (int width : {600, 1200}) {
            const int height = width * 2 / 3;
            const auto clouds = field(width, height, 1), lay = field(width, height, 2);
            QImage image(width, height, QImage::Format_RGBA8888_Premultiplied);
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                const int i = y * width + x; const double horizon = height * (.5 + .25 * (lay[x] - .5));
                const double sky = 140 + 80 * (1 - y / horizon) + 60 * (clouds[i] - .5);
                const double ground = 4 + 70 * std::pow(clouds[i], 2.2) + 20 * (lay[i] - .5);
                const uchar v = uchar(std::clamp(qRound(y < horizon ? sky : ground), 0, 255));
                auto *p = image.scanLine(y) + x * 4; p[0] = p[1] = p[2] = v; p[3] = 255;
            }
            for (int amount : {100, 50, -100}) {
                const auto out = cameraRaw(image, {{"shadows", amount}}).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
                auto mean = [&](int first, int last) {
                    double sum = 0;
                    for (int y = first; y < last; ++y) for (int x = 0; x < width; ++x) sum += out.constScanLine(y)[x * 4 + 1];
                    return sum / ((last - first) * width);
                };
                const double ground = mean(height * 4 / 5, height), sky = mean(0, height / 5);
                const double expectedGround = width == 1200 ? (amount == 100 ? 59.0 : amount == 50 ? 34.9 : 4.2)
                                                           : (amount == 100 ? 51.0 : amount == 50 ? 32.2 : 5.5);
                const double expectedSky = width == 1200 ? (amount == 100 ? 205.9 : amount == 50 ? 205.3 : 203.6)
                                                        : (amount == 100 ? 205.8 : amount == 50 ? 205.2 : 203.6);
                qInfo() << "UPSTREAM_SHADOWS" << width << amount << ground << sky;
                QVERIFY(std::abs(ground - expectedGround) <= (width == 600 && amount == 100 ? 10 : 4));
                QVERIFY(std::abs(sky - expectedSky) <= 2);
            }
        }
    }
    void sectionResetLeavesOtherSectionsAndRestoresVisibility() {
        QJsonObject original{{"exposure", 2}, {"shadows", 70}, {"temperature", 31}, {"enableLight", false},
            {"detail", QJsonObject{{"noiseLuminance", 45}}}, {"mixer", QJsonObject{{"points", QJsonArray{QJsonObject{{"hue", 30}}}}}}};
        const auto reset = resetCameraRawSection(original, "light");
        QVERIFY(!reset.contains("exposure")); QVERIFY(!reset.contains("shadows")); QVERIFY(reset["enableLight"].toBool());
        QCOMPARE(reset["temperature"], original["temperature"]); QCOMPARE(reset["detail"], original["detail"]);
        QCOMPARE(reset["mixer"], original["mixer"]);
        QCOMPARE(resetCameraRawSection(original, "unknown"), original);
        QVERIFY(!resetCameraRawSection(original, "pointColor")["mixer"].toObject().contains("points"));
    }
    void cameraRawSectionResetIsWiredToTheDialog() {
        MainWindow window; const auto before = document(16); window.document()->reset(before, true);
        bool checked = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); if (!dialog) return;
            auto *exposure = dialog->findChild<QDoubleSpinBox *>("raw_exposure");
            auto *temperature = dialog->findChild<QDoubleSpinBox *>("raw_temperature");
            auto *reset = dialog->findChild<QPushButton *>("rawReset_light");
            if (exposure && temperature && reset) {
                exposure->setValue(2); temperature->setValue(31); reset->click();
                checked = exposure->value() == 0 && temperature->value() == 31;
            }
            dialog->reject();
        });
        auto *action = window.findChild<QAction *>("filter.cameraRawAction"); QVERIFY(action); action->trigger();
        QVERIFY(checked); QCOMPARE(window.document()->state().layers[0].image, before.layers[0].image);
    }
    void imageFolderImportIsSortedNonrecursiveAndSkipsProjects() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); QDir root(dir.path()); root.mkdir("nested"); root.mkdir("project.comp");
        for (const auto &name : {"B.PNG", "a.hif", "C.dng", "ignored.txt", "nested/inside.png"}) {
            QFile f(root.filePath(name)); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("fixture");
        }
        const auto found = imageFilesInFolder(dir.path()); QCOMPARE(found.size(), 3);
        QCOMPARE(QFileInfo(found[0]).fileName(), QString("a.hif")); QCOMPARE(QFileInfo(found[1]).fileName(), QString("B.PNG"));
        QVERIFY(imageFilesInFolder(root.filePath("project.comp")).isEmpty());
    }
};
QTEST_MAIN(GpuUpstreamTests)
#include "GpuUpstreamTests.moc"
