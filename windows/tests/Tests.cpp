#include "MainWindow.h"
#include "Preferences.h"
#include <QRegularExpression>
#include <QTabWidget>
#include "Pixels.h"
#include "WideKernels.h"
#include "Renderer.h"
#include "Filters.h"
#include "Icons.h"
#include "Effects.h"
#include "Importers.h"
#include "CameraRaw.h"
#include "Selection.h"
#include "Warp.h"
#include "Text.h"
#include "Export.h"
#include <QListWidget>
#include <QLineEdit>
#include <QImageReader>
#include <cmath>
#include <QTabWidget>
#include <QMessageBox>
#include <QKeySequenceEdit>
#include <QTableWidget>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QTextEdit>
#include <QPushButton>
#include <QTimer>
#include <QPainter>
#include <QApplication>
#include <QFile>
#include <QFileDialog>
#include <QJsonDocument>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
using namespace comp;
namespace {
State fixture(QSize size = {8, 8}) {
    State s;
    s.id = newId();
    s.size = size;
    Layer l;
    l.id = newId();
    l.name = "底图 Background";
    l.image = QImage(size, QImage::Format_ARGB32_Premultiplied);
    l.image.fill(QColor(180, 120, 60));
    l.placement.size = size;
    s.layers.append(l);
    s.active = l.id;
    return s;
}
void rewrite(const QString &directory, const std::function<void(QJsonObject &)> &change) {
    QFile file(directory + "/manifest.json");
    if (!file.open(QIODevice::ReadOnly))
        qFatal("read");
    auto j = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    change(j);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        qFatal("write");
    file.write(QJsonDocument(j).toJson());
}
bool rejects(const QString &key, const std::function<void()> &f) {
    try {
        f();
    } catch (const Error &e) {
        return e.key == key;
    }
    return false;
}
struct PickResult {
    bool shown = false;
    QFileDialog::FileMode mode = QFileDialog::AnyFile;
    QStringList filters;
};
PickResult pickFile(MainWindow &window, const char *action, const QString &path = {}) {
    PickResult result;
    QTimer::singleShot(0, &window, [&] {
        auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
        if (!dialog)
            return;
        result = {true, dialog->fileMode(), dialog->nameFilters()};
        if (path.isEmpty()) {
            dialog->reject();
        } else {
            dialog->setDirectory(QFileInfo(path).absolutePath());
            dialog->selectFile(path);
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        }
    });
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &window, [&] {
        if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
            dialog->reject();
    });
    timeout.start(5000);
    window.findChild<QAction *>(action)->trigger();
    return result;
}
} // namespace
class Tests : public QObject {
    Q_OBJECT
  private slots:
    void sixteenBitProjectPngAndDepthConversion() {
        QTemporaryDir dir;
        auto s = fixture({1024, 4});
        s.bitDepth = 16;
        QImage image(s.size, QImage::Format_RGBA64_Premultiplied);
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                reinterpret_cast<QRgba64 *>(image.scanLine(y))[x] = rgba64(12001 + x, 23002 + x, 34003 + x, 65535);
        s.layers[0].image = image;
        image.setColorSpace(QColorSpace::SRgb);
        s.layers[0].image = image;
        const auto png = pngBytes(image);
        QCOMPARE(quint8(png[24]), quint8(16));
        QFile file(dir.filePath("input.png"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(png);
        file.close();
        const auto decoded = readImage(file.fileName());
        QCOMPARE(decoded, image);
        const QString project = dir.filePath("Wide.comp");
        writeProject(project, s);
        auto loaded = readProject(project);
        QCOMPARE(loaded.state.bitDepth, 16);
        QCOMPARE(loaded.state.metadata["version"].toInt(), 12);
        QCOMPARE(loaded.state.layers[0].image, image);
        QCOMPARE(render(loaded.state), image);
        exportDocument(s, dir.filePath("out.png"), "png", s.size);
        QCOMPARE(readImage(dir.filePath("out.png")), image);
        Document d;
        d.reset(s, true);
        d.setBitDepth(8);
        QCOMPARE(d.state().bitDepth, 8);
        QVERIFY(!wideImage(d.state().layers[0].image));
        d.undo();
        QCOMPARE(d.state().bitDepth, 16);
        QCOMPARE(d.state().layers[0].image, image);
        auto eight = fixture();
        writeProject(dir.filePath("Eight.comp"), eight);
        QCOMPARE(readProject(dir.filePath("Eight.comp")).state.metadata["version"].toInt(), 11);
        rewrite(project, [](QJsonObject &m) { m["version"] = 11; });
        QVERIFY(rejects("missingAsset", [&] { readProject(project); }) ||
                rejects("invalidProject", [&] { readProject(project); }));
    }
    void sixteenBitPhotoshopCompressionAndLayers() {
        for (const auto &ext : QStringList{"psd", "psb"})
            for (int compression = 0; compression < 4; ++compression) {
                for (const auto &kind : QStringList{"layers", "flat"}) {
                    const auto path =
                        QString(COMPOSITOR_FIXTURES) + QString("/wide-%1-%2.%3").arg(kind).arg(compression).arg(ext);
                    const auto imported = readPhotoshop(path);
                    QCOMPARE(imported.state.bitDepth, 16);
                    QCOMPARE(imported.state.size, QSize(17, 9));
                    QCOMPARE(imported.state.layers.size(), kind == "layers" ? 2 : 1);
                    const auto &bottom = imported.state.layers[0];
                    QVERIFY(wideImage(bottom.image));
                    QCOMPARE(pixel64(bottom.image, 0, 0).red(), quint16(10001));
                    QCOMPARE(pixel64(bottom.image, 1, 0).red(), quint16(kind == "layers" ? 10008 : 10032));
                    if (kind == "layers") {
                        const auto &top = imported.state.layers[1];
                        QCOMPARE(top.placement.origin, QPointF(3, 2));
                        QCOMPARE(top.mask.format(), QImage::Format_Grayscale16);
                        QCOMPARE(reinterpret_cast<const quint16 *>(top.mask.constScanLine(0))[1], quint16(31002));
                        QCOMPARE(pixel64(top.image, 0, 0).alpha(), quint16(40001));
                    }
                    QTemporaryDir dir;
                    writeProject(dir.filePath("test.comp"), imported.state);
                    const auto reopened = readProject(dir.filePath("test.comp"));
                    QCOMPARE(reopened.state.bitDepth, 16);
                    QCOMPARE(reopened.state.layers[0].image, bottom.image);
                }
            }
        const QString references = qEnvironmentVariable("COMPOSITOR_PSD16_FIXTURES");
        if (!references.isEmpty())
            for (const auto &name : QStringList{"16bit5x5.psd", "16bit5x5.psb", "4x4_16bit_rgb.psd"}) {
                const auto imported = readPhotoshop(references + "/" + name);
                QCOMPARE(imported.state.bitDepth, 16);
                QVERIFY(!imported.state.layers.isEmpty());
                QVERIFY(wideImage(render(imported.state)));
            }
    }
    void sixteenBitFiltersPreserveFineTones() {
        QImage image(1024, 16, QImage::Format_RGBA64_Premultiplied);
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                reinterpret_cast<QRgba64 *>(image.scanLine(y))[x] = rgba64(12001 + x, 23002 + x, 34003 + x, 65535);
        for (const auto &kind : QStringList{"Invert", "Exposure", "Levels", "Curves", "Hue/Saturation", "Gaussian Blur",
                                            "Motion Blur", "Black & White", "Color Balance", "Gradient Map", "Grain",
                                            "Add Noise", "Camera Raw", "Scanlines"}) {
            QJsonObject settings{{"kind", kind}};
            if (kind == "Grain")
                settings["grainSettings"] = QJsonObject{{"amount", 0}};
            if (kind == "Add Noise")
                settings["noiseAmount"] = 0;
            auto out = filtered(image, settings);
            QVERIFY2(wideImage(out), qPrintable(kind));
            QSet<int> tones;
            bool fractional = false;
            for (int y = 4; y < 12; ++y)
                for (int x = 100; x < 900; ++x) {
                    const auto pixel = pixel64(out, x, y);
                    tones.insert(pixel.red());
                    fractional |= pixel.red() % 257 != 0;
                }
            QVERIFY2(fractional, qPrintable(kind));
            if (kind != "Scanlines")
                QVERIFY2(tones.size() > 300, qPrintable(kind + ":" + QString::number(tones.size())));
        }
        auto inverse = filtered(image, {{"kind", "Invert"}});
        QCOMPARE(pixel64(inverse, 0, 0).red(), quint16(65535 - 12001));
        auto neutral = filtered(image, {{"kind", "Exposure"}});
        QCOMPARE(neutral, image);
        auto s = fixture(image.size());
        s.bitDepth = 16;
        s.layers[0].image = image;
        Document d;
        d.reset(s, true);
        d.applyFilter({{"kind", "Invert"}});
        d.undo();
        QCOMPARE(d.state().layers[0].image, image);
    }
    void sixteenBitBlendsEffectsAndRetouching() {
        auto s = fixture({48, 32});
        s.bitDepth = 16;
        QImage image(s.size, pixelFormat(16));
        image.fill(QColor::fromRgba64(12345, 23456, 34567, 65535));
        s.layers[0].image = image;
        Layer top = s.layers[0];
        top.id = newId();
        top.image.fill(QColor::fromRgba64(45678, 34567, 12345, 32769));
        s.layers.append(top);
        for (const auto &mode : blendModes()) {
            s.layers[1].blend = mode;
            auto out = render(s);
            QVERIFY2(wideImage(out), qPrintable(mode));
            const auto a = pixel64(out, 3, 3);
            QVERIFY(a.red() % 257 || a.green() % 257 || a.blue() % 257);
        }
        for (const auto &key :
             QStringList{"shadow", "innerShadow", "innerGlow", "outerGlow", "colorOverlay", "stroke"}) {
            QJsonObject effect{{key, QJsonObject{{"opacity", .37},
                                                 {"red", .12345},
                                                 {"green", .23456},
                                                 {"blue", .34567},
                                                 {"size", 2},
                                                 {"blur", 2},
                                                 {"distance", 1}}}};
            const auto out = applyEffects(top.image, effect, 1);
            QVERIFY(wideImage(out));
        }
        auto warped = image;
        reinterpret_cast<QRgba64 *>(warped.scanLine(12))[22] = rgba64(12401, 23502, 34603, 65535);
        const auto before = warped;
        warpDab(warped, {22, 12}, {25, 14}, 6, .5, .8, false);
        QVERIFY(wideImage(warped));
        QVERIFY(warped != before);
        QCOMPARE(pixel64(warped, 1, 1), pixel64(before, 1, 1));
        auto perspective = perspectiveWarp(before, QPolygonF{{0, 0}, {48, 1}, {47, 32}, {1, 31}});
        QVERIFY(wideImage(perspective));
        QByteArray coverage(48 * 32, 0);
        coverage[15 * 48 + 24] = char(-1);
        auto healing = image;
        QCOMPARE(wide::spot_heal(reinterpret_cast<quint16 *>(healing.bits()),
                                 reinterpret_cast<const uint8_t *>(coverage.constData()), 48, 32,
                                 healing.bytesPerLine() / 2, 1, 0, 0),
                 0);
        QCOMPARE(pixel64(healing, 24, 15), pixel64(image, 24, 15));
        QCOMPARE(wide::content_fill(reinterpret_cast<quint16 *>(healing.bits()), healing.bytesPerLine() / 2,
                                    reinterpret_cast<const uint8_t *>(coverage.constData()), 48, 48, 32),
                 1);
        QCOMPARE(pixel64(healing, 24, 15), pixel64(image, 24, 15));
    }
    void sixteenBitCanvasBrushAndRawDevelopment() {
        MainWindow window;
        window.show();
        auto s = fixture({120, 80});
        s.bitDepth = 16;
        s.layers[0].image = QImage(s.size, pixelFormat(16));
        s.layers[0].image.fill(QColor::fromRgba64(12345, 23456, 34567, 65535));
        window.document()->reset(s, true);
        auto *canvas = window.canvas();
        canvas->fit();
        canvas->setTool(Tool::Brush);
        canvas->setColor(QColor::fromRgba64(45678, 12345, 23456, 65535));
        const auto point = canvas->viewPoint(QPointF(60, 40)).toPoint();
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, point);
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, point);
        QVERIFY(wideImage(window.document()->state().layers[0].image));
        QVERIFY(pixel64(window.document()->state().layers[0].image, 60, 40).red() % 257 != 0);
        QCOMPARE(pixel64(window.document()->state().layers[0].image, 1, 1), pixel64(s.layers[0].image, 1, 1));
        window.document()->undo();
        QCOMPARE(window.document()->state().layers[0].image, s.layers[0].image);
        window.document()->maskAction("addMask");
        QCOMPARE(window.document()->state().layers[0].mask.format(), QImage::Format_Grayscale16);
        window.document()->fill(QColor::fromRgba64(30001, 30001, 30001, 65535), false, true);
        QVERIFY(wideImage(window.document()->state().layers[0].mask));
        QCOMPARE(reinterpret_cast<const quint16 *>(window.document()->state().layers[0].mask.constScanLine(40))[60],
                 quint16(30001));
        window.document()->undo();
        auto raw = developRaw(decodeRaw(QString(COMPOSITOR_FIXTURES) + "/linear.dng"));
        QVERIFY(wideImage(raw));
        QImage photo(400, 16, QImage::Format_RGBA64);
        for (int y = 0; y < photo.height(); ++y)
            for (int x = 0; x < photo.width(); ++x)
                reinterpret_cast<QRgba64 *>(photo.scanLine(y))[x] = rgba64(10001 + x, 10001 + x, 10001 + x, 65535);
        RawFrame frame{photo, "Synthetic"};
        auto developed = developRaw(frame);
        QSet<int> tones;
        for (int x = 0; x < 400; ++x)
            tones.insert(pixel64(developed, x, 0).red());
        QVERIFY(tones.size() > 256);
        window.document()->markSaved(window.document()->revision());
    }
    void sixteenBitCameraRawControls() {
        QImage image(64, 40, pixelFormat(16));
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                reinterpret_cast<QRgba64 *>(image.scanLine(y))[x] =
                    rgba64(10001 + x * 211, 15002 + y * 307, 22003 + x * 73, 40001);
        const auto identity = cameraRaw(image, {});
        QCOMPARE(identity, image);
        const auto cases = QJsonDocument::fromJson(R"([
            {"exposure":0.5,"contrast":25,"highlights":-20,"shadows":20},
            {"temperature":35,"tint":-10,"vibrance":15},
            {"curve":{"rgb":[{"x":0,"y":0},{"x":0.5,"y":0.6},{"x":1,"y":1}]}},
            {"mixer":{"saturation":[20,10,-15,30,5,-10,5,20]}},
            {"mixer":{"points":[{"hue":210,"saturation":0.5,"luminance":0.5,"hueShift":20,"saturationShift":30,"luminanceShift":10,"hueRange":180,"saturationRange":1,"luminanceRange":1}]}},
            {"grading":{"midtones":{"hue":40,"saturation":35,"luminance":10}}},
            {"texture":35,"clarity":25,"dehaze":10,"glow":20,"grainAmount":5},
            {"vignetteAmount":-40},
            {"detail":{"sharpenAmount":60,"noiseLuminance":20,"noiseColor":20}},
            {"optics":{"distortion":25,"chromaticAberration":25,"purpleAmount":20,"vignette":25}},
            {"geometry":{"rotate":3,"vertical":10,"constrainCrop":true}},
            {"calibration":{"redHue":30,"shadowTint":10}}
        ])")
                               .array();
        QCOMPARE(cases.size(), 12);
        for (const auto &entry : cases) {
            const auto settings = entry.toObject();
            const auto out = cameraRaw(image, settings);
            QVERIFY2(wideImage(out), qPrintable(QString::fromUtf8(QJsonDocument(settings).toJson())));
            bool fine = false;
            for (int y = 8; y < 32; ++y)
                for (int x = 8; x < 56; ++x) {
                    const auto p = pixel64(out, x, y);
                    fine |= p.red() % 257 || p.green() % 257 || p.blue() % 257;
                    QVERIFY(p.red() <= p.alpha() && p.green() <= p.alpha() && p.blue() <= p.alpha());
                }
            QVERIFY(fine);
        }
        QVERIFY(wideImage(cameraRaw(image, {{"sharpenMask", true}})));
        QVERIFY(wideImage(cameraRaw(image, {}, 1, 1)));
    }
    void sixteenBitCreateAndConvertUi() {
        MainWindow window;
        window.show();
        bool chosen = false;
        QTimer pick;
        connect(&pick, &QTimer::timeout, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            auto *depth = dialog->findChild<QComboBox *>("canvasBitDepth");
            if (!depth)
                return;
            depth->setCurrentIndex(depth->findData(16));
            chosen = true;
            dialog->accept();
        });
        pick.start(20);
        window.findChild<QAction *>("newAction")->trigger();
        pick.stop();
        QVERIFY(chosen);
        QCOMPARE(window.document()->state().bitDepth, 16);
        QVERIFY(window.findChild<QAction *>("depth16Action")->isChecked());
        bool canceled = false;
        QTimer cancel;
        connect(&cancel, &QTimer::timeout, this, [&] {
            if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                canceled = true;
                message->button(QMessageBox::Cancel)->click();
            }
        });
        cancel.start(20);
        window.findChild<QAction *>("depth8Action")->trigger();
        cancel.stop();
        QVERIFY(canceled);
        QCOMPARE(window.document()->state().bitDepth, 16);
        QVERIFY(window.findChild<QAction *>("depth16Action")->isChecked());
        bool accepted = false;
        QTimer confirm;
        connect(&confirm, &QTimer::timeout, this, [&] {
            if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                accepted = true;
                message->button(QMessageBox::Ok)->click();
            }
        });
        confirm.start(20);
        window.findChild<QAction *>("depth8Action")->trigger();
        confirm.stop();
        QVERIFY(accepted);
        QCOMPARE(window.document()->state().bitDepth, 8);
        window.document()->undo();
        QCOMPARE(window.document()->state().bitDepth, 16);
        QTemporaryDir dir;
        writeProject(dir.filePath("Blank16.comp"), window.document()->state());
        QCOMPARE(readProject(dir.filePath("Blank16.comp")).state.bitDepth, 16);
        window.document()->markSaved(window.document()->revision());
    }
    void sixteenBitTiffAndSelectionPrecision() {
        const auto image = readImage(QString(COMPOSITOR_FIXTURES) + "/wide-rgb.tif");
        QVERIFY(wideImage(image));
        QCOMPARE(image.size(), QSize(17, 9));
        QCOMPARE(pixel64(image, 1, 1).red(), quint16(10032));
        QCOMPARE(pixel64(image, 1, 1).green(), quint16(20031));
        QImage fine(2, 1, pixelFormat(16));
        auto *pixels = reinterpret_cast<QRgba64 *>(fine.bits());
        pixels[0] = rgba64(10001, 20002, 30003, 65535);
        pixels[1] = rgba64(10002, 20002, 30003, 65535);
        const auto selected = wandPath(fine, {0, 0}, 0, false);
        QVERIFY(selected.contains(QPointF(.5, .5)));
        QVERIFY(!selected.contains(QPointF(1.5, .5)));
        auto state = fixture({64, 40});
        state.bitDepth = 16;
        state.layers[0].image = QImage(state.size, pixelFormat(16));
        state.layers[0].image.fill(QColor::fromRgba64(12345, 23456, 34567, 65535));
        Document doc;
        doc.reset(state, true);
        doc.maskAction("addMask");
        doc.fill(QColor::fromRgba64(32001, 32001, 32001, 65535), false, true);
        QCOMPARE(reinterpret_cast<const quint16 *>(doc.state().layers[0].mask.constScanLine(1))[1], quint16(32001));
        doc.maskAction("applyMask");
        QCOMPARE(pixel64(doc.state().layers[0].image, 1, 1).alpha(), quint16(32001));
        doc.undo();
        QVERIFY(!doc.state().layers[0].mask.isNull());
        doc.resizeCanvas({128, 80}, true, false);
        QVERIFY(wideImage(doc.state().layers[0].image));
        doc.undo();
        QCOMPARE(doc.state().layers[0].image, state.layers[0].image);
    }
    void sixteenBitMasksAndSelections() {
        auto s = fixture({32, 24});
        s.bitDepth = 16;
        s.layers[0].image = QImage(s.size, pixelFormat(16));
        s.layers[0].image.fill(QColor::fromRgba64(12345, 23456, 34567, 65535));
        s.layers[0].mask = QImage(s.size, QImage::Format_Grayscale16);
        for (int y = 0; y < s.size.height(); ++y)
            for (int x = 0; x < s.size.width(); ++x)
                reinterpret_cast<quint16 *>(s.layers[0].mask.scanLine(y))[x] = quint16(30001 + x);
        s.layers[0].maskPlacement = s.layers[0].placement;
        auto out = render(s);
        QVERIFY(wideImage(out));
        QCOMPARE(pixel64(out, 1, 1).alpha(), quint16(30002));
        QTemporaryDir dir;
        writeProject(dir.filePath("Mask.comp"), s);
        QCOMPARE(readProject(dir.filePath("Mask.comp")).state.layers[0].mask, s.layers[0].mask);
        Document d;
        d.reset(s, true);
        d.setSelectionMask(s.layers[0].mask);
        d.fill(QColor::fromRgba64(45678, 12345, 23456, 65535));
        QVERIFY(wideImage(d.state().layers[0].image));
        QVERIFY(pixel64(d.state().layers[0].image, 1, 1).red() % 257 != 0);
        d.undo();
        QCOMPARE(d.state().layers[0].image, s.layers[0].image);
        d.selectAll();
        d.featherSelection(2);
        QCOMPARE(d.state().selectionMask.format(), QImage::Format_Grayscale16);
    }
    void upstream149CameraRawReferenceValues() {
        struct Case {
            QColor input, expected;
            QJsonObject settings;
        };
        const QList<Case> cases{
            {QColor(128, 128, 128), QColor(181, 181, 181), {{"exposure", 1}}},
            {QColor(128, 128, 128), QColor(235, 189, 117), {{"temperature", 100}}},
            {QColor(128, 128, 128), QColor(102, 146, 210), {{"temperature", -50}}},
            {QColor(128, 128, 128), QColor(159, 125, 215), {{"tint", 100}}},
            {QColor(92, 92, 230), QColor(164, 164, 210), {{"vibrance", -100}}},
            {QColor(230, 115, 0),
             QColor(189, 130, 107),
             {{"mixer", QJsonObject{{"saturation", QJsonArray{0, -60, 0, 0, 0, 0, 0, 0}}}}}},
            {QColor(92, 92, 230), QColor(149, 149, 226), {{"calibration", QJsonObject{{"blueSaturation", -60}}}}},
            {QColor(130, 130, 130), QColor(101, 101, 101), {{"curve", QJsonObject{{"darks", -60}}}}}};
        for (const auto &c : cases) {
            QImage input(8, 8, QImage::Format_ARGB32_Premultiplied);
            input.fill(c.input);
            auto out = cameraRaw(input, c.settings).pixelColor(3, 3);
            const auto detail = QString::fromUtf8(QJsonDocument(c.settings).toJson()) + out.name();
            QVERIFY2(std::abs(out.red() - c.expected.red()) <= 4 && std::abs(out.green() - c.expected.green()) <= 4 &&
                         std::abs(out.blue() - c.expected.blue()) <= 4,
                     qPrintable(detail));
        }
        QImage ramp(256, 2, QImage::Format_ARGB32_Premultiplied);
        for (int x = 0; x < 256; ++x)
            for (int y = 0; y < 2; ++y)
                ramp.setPixelColor(x, y, QColor(x, x, x, y ? 128 : 255));
        for (auto pair : {std::pair{100, 0}, std::pair{-100, 0}, std::pair{0, 100}, std::pair{0, -100}}) {
            auto out = cameraRaw(ramp, {{"shadows", pair.first}, {"highlights", pair.second}});
            for (int x = 1; x < 256; ++x)
                QVERIFY(out.pixelColor(x, 0).green() >= out.pixelColor(x - 1, 0).green());
            for (int x = 0; x < 256; ++x)
                QCOMPARE(out.pixelColor(x, 1).alpha(), 128);
        }
    }
    void rawDitherFollowsGradient() {
        RawFrame frame;
        frame.linear = QImage(400, 64, QImage::Format_RGBA64);
        for (int y = 0; y < 64; ++y) {
            const double encoded = .30 + .02 * (y + .5) / 64.;
            const auto linear = quint16(qRound(std::pow((encoded + .055) / 1.055, 2.4) * 65535));
            auto *row = reinterpret_cast<QRgba64 *>(frame.linear.scanLine(y));
            for (int x = 0; x < 400; ++x)
                row[x] = QRgba64::fromRgba64(linear, linear, linear, 65535);
        }
        const auto first = developRaw(frame, 0, 6500, 0, 1, 0, 8), second = developRaw(frame, 0, 6500, 0, 1, 0, 8);
        QCOMPARE(first, second);
        for (int y = 0; y < 64; ++y) {
            double sum = 0;
            for (int x = 0; x < 400; ++x) {
                sum += first.pixelColor(x, y).red();
                QCOMPARE(first.pixelColor(x, y).alpha(), 255);
            }
            QVERIFY(std::abs(sum / 400 - (.30 + .02 * (y + .5) / 64.) * 255) < .25);
        }
    }
    void canvasQuarterTurnKeepsPixelsMasksAndUndo() {
        auto s = fixture({12, 8});
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 12; ++x)
                s.layers[0].image.setPixelColor(x, y, QColor(x * 20, y * 30, 60));
        s.layers[0].originalPng = pngBytes(s.layers[0].image);
        s.layers[0].mask = QImage(12, 8, QImage::Format_Grayscale8);
        s.layers[0].mask.fill(255);
        s.layers[0].maskLinked = false;
        s.layers[0].maskPlacement = s.layers[0].placement;
        s.layers[0].metadata["text"] = QJsonObject{{"content", "Keep text"}, {"fontName", "Arial"}, {"fontSize", 12}};
        s.guides = {QJsonObject{{"id", newId()}, {"axis", "horizontal"}, {"position", 2}}};
        s.selection.addRect(2, 1, 3, 2);
        s.selectionActive = true;
        s.selectionMask = QImage(s.size, QImage::Format_Grayscale8);
        s.selectionMask.fill(127);
        const auto image = render(s);
        Document doc;
        doc.reset(s, true);
        doc.rotateCanvas(true);
        QCOMPARE(doc.state().size, QSize(8, 12));
        const auto rotated = render(doc.state()), expected = image.transformed(QTransform().rotate(90));
        for (int y = 0; y < rotated.height(); ++y)
            for (int x = 0; x < rotated.width(); ++x)
                QVERIFY2(rotated.pixelColor(x, y) == expected.pixelColor(x, y),
                         qPrintable(QString("pixel %1,%2: %3 expected %4")
                                        .arg(x)
                                        .arg(y)
                                        .arg(rotated.pixelColor(x, y).name(QColor::HexArgb))
                                        .arg(expected.pixelColor(x, y).name(QColor::HexArgb))));
        QCOMPARE(doc.state().layers[0].originalPng, s.layers[0].originalPng);
        QCOMPARE(doc.state().layers[0].metadata, s.layers[0].metadata);
        QCOMPARE(doc.state().selectionMask.size(), QSize(8, 12));
        QCOMPARE(doc.state().guides[0].toObject()["position"].toDouble(), 6.);
        doc.undo();
        QCOMPARE(render(doc.state()), image);
        QVERIFY(!doc.dirty());
        doc.redo();
        doc.rotateCanvas(false);
        QCOMPARE(render(doc.state()), image);
    }
    void scanlinesKeepAlphaAndUndo() {
        auto s = fixture({80, 64});
        s.layers[0].image.fill(QColor(180, 100, 70, 128));
        s.layers[0].image.setPixelColor(0, 0, Qt::transparent);
        s.layers[0].metadata["effects"] = QJsonObject{{"colorOverlay", QJsonObject{{"red", 1}, {"opacity", .2}}}};
        Document doc;
        doc.reset(s, true);
        QJsonObject settings{{"kind", "Scanlines"}, {"colors", 1}, {"displace", 20},
                             {"split", 2},          {"dots", 50},  {"glow", 20}};
        doc.applyFilter(settings);
        const auto &out = doc.state().layers[0].image;
        QVERIFY(out != s.layers[0].image);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 80; ++x)
                QCOMPARE(out.pixelColor(x, y).alpha(), s.layers[0].image.pixelColor(x, y).alpha());
        QCOMPARE(doc.state().layers[0].metadata["effects"], s.layers[0].metadata["effects"]);
        doc.undo();
        QCOMPARE(doc.state().layers[0].image, s.layers[0].image);
        doc.resizeCanvas({160, 128}, true);
        QVERIFY(doc.state().layers[0].metadata.contains("effects"));
    }
    void translucentInnerEffectsAndResize() {
        QImage source(20, 20, QImage::Format_ARGB32_Premultiplied);
        source.fill(QColor(255, 0, 0, 128));
        for (auto key : {"innerGlow", "innerShadow"}) {
            const auto image = applyEffects(source,
                                            {{key, QJsonObject{{"red", 0},
                                                               {"green", 0},
                                                               {"blue", 1},
                                                               {"opacity", 1},
                                                               {"size", 0},
                                                               {"blur", 0},
                                                               {"distance", 0}}}},
                                            1);
            QCOMPARE(image.pixelColor(10, 10).alpha(), 128);
            QVERIFY(image.pixelColor(10, 10).blue() >= 120);
        }
        auto s = fixture({20, 20});
        s.layers[0].metadata["effects"] =
            QJsonObject{{"shadow", QJsonObject{{"distance", 8}, {"blur", 4}}}, {"stroke", QJsonObject{{"size", 3}}}};
        Document doc;
        doc.reset(s, true);
        doc.resizeCanvas({40, 40}, true);
        const auto effects = doc.state().layers[0].metadata["effects"].toObject();
        QCOMPARE(effects["shadow"].toObject()["distance"].toDouble(), 16.);
        QCOMPARE(effects["shadow"].toObject()["blur"].toDouble(), 8.);
        QCOMPARE(effects["stroke"].toObject()["size"].toDouble(), 6.);
        doc.undo();
        QCOMPARE(doc.state().layers[0].metadata, s.layers[0].metadata);
    }
    void sizedPngJpegAndPdfExport() {
        QTemporaryDir dir;
        auto s = fixture({80, 40});
        s.resolution = 300;
        for (const auto &format : QList<QByteArray>{"png", "jpeg", "pdf"}) {
            QString path = dir.path() + "/export." + format;
            exportDocument(s, path, format, {120, 60}, 90);
            QFile f(path);
            QVERIFY(f.open(QIODevice::ReadOnly));
            auto data = f.readAll();
            if (format == "pdf") {
                QVERIFY(data.startsWith("%PDF-"));
                QVERIFY(data.contains("/Type /Page"));
                QVERIFY(data.contains("/MediaBox"));
            } else {
                const auto image = readImage(path);
                QCOMPARE(image.size(), QSize(120, 60));
            }
        }
    }
    void webpImportKeepsAlpha() {
        QTemporaryDir dir;
        QImage image(12, 8, QImage::Format_ARGB32);
        image.fill(QColor(30, 150, 80, 90));
        auto path = dir.path() + "/transparent.webp";
        QVERIFY(image.save(path, "webp", 100));
        const auto result = readImage(path);
        QCOMPARE(result.size(), image.size());
        QCOMPARE(result.pixelColor(3, 3).alpha(), 90);
    }
    void paletteInvokesTranslatedCommandAndRepeatsFilter() {
        MainWindow window;
        window.show();
        window.document()->reset(fixture({64, 48}), true);
        window.localization()->setLanguage("zh-CN");
        QTimer::singleShot(80, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto *search = dialog->findChild<QLineEdit *>("commandSearch");
            QVERIFY(search);
            search->setText("顺时针");
            const auto capture = qEnvironmentVariable("COMPOSITOR_TEST_CAPTURE_DIR");
            if (!capture.isEmpty())
                dialog->grab().save(capture + "/command-palette-031.png");
            auto *list = dialog->findChild<QListWidget *>("commandResults");
            QVERIFY(list);
            QCOMPARE(list->count(), 1);
            QTest::keyClick(search, Qt::Key_Return);
        });
        window.findChild<QAction *>("searchCommandsAction")->trigger();
        QCOMPARE(window.document()->state().size, QSize(48, 64));
        window.document()->undo();
        const auto before = window.document()->state().layers[0].image;
        QTimer::singleShot(80, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->accept();
        });
        window.findChild<QAction *>("filter.invertAction")->trigger();
        QVERIFY(window.document()->state().layers[0].image != before);
        auto *repeat = window.findChild<QAction *>("lastFilterAction");
        QVERIFY(repeat->isEnabled());
        repeat->trigger();
        QCOMPARE(window.document()->state().layers[0].image, before);
        window.document()->markSaved(window.document()->revision());
    }
    void physicalCanvasAndMiddleZoom() {
        MainWindow window;
        window.show();
        QTimer::singleShot(80, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->findChild<QComboBox *>("canvasUnits")->setCurrentIndex(1);
            dialog->findChild<QComboBox *>("canvasDpi")->setCurrentIndex(1);
            dialog->findChild<QDoubleSpinBox *>("canvasWidth")->setValue(2);
            dialog->findChild<QDoubleSpinBox *>("canvasHeight")->setValue(1);
            dialog->accept();
        });
        window.findChild<QAction *>("newAction")->trigger();
        QCOMPARE(window.document()->state().size, QSize(600, 300));
        QCOMPARE(window.document()->state().resolution, 300.);
        auto *canvas = window.canvas();
        canvas->fit();
        double original = canvas->zoom();
        QTest::mousePress(canvas, Qt::MiddleButton, Qt::ControlModifier, {100, 100});
        QTest::mouseMove(canvas, {150, 100});
        QTest::mouseRelease(canvas, Qt::MiddleButton, Qt::ControlModifier, {150, 100});
        QVERIFY(canvas->zoom() > original);
        canvas->setZoom(4);
        const auto capture = qEnvironmentVariable("COMPOSITOR_TEST_CAPTURE_DIR");
        if (!capture.isEmpty())
            window.grab().save(capture + "/navigator-031.png");
        const auto center = canvas->documentPoint(QPointF(canvas->width() / 2., canvas->height() / 2.));
        QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(canvas->width() - 25, canvas->height() - 25));
        QVERIFY(canvas->documentPoint(QPointF(canvas->width() / 2., canvas->height() / 2.)) != center);
        window.document()->markSaved(window.document()->revision());
    }
    void projectRoundTrip() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.path() + "/作品.comp";
        auto s = fixture();
        s.layers[0].placement.origin = {-2, 3};
        s.layers[0].placement.rotation = 23;
        s.layers[0].opacity = .6;
        s.layers[0].blend = "Multiply";
        const QByteArray digest = writeProject(path, s);
        const auto loaded = readProject(path);
        QCOMPARE(loaded.digest, digest);
        QCOMPARE(loaded.state.id, s.id);
        QCOMPARE(loaded.state.layers[0].name, s.layers[0].name);
        QCOMPARE(loaded.state.layers[0].placement.json(), s.layers[0].placement.json());
        QCOMPARE(loaded.state.layers[0].image, s.layers[0].image);
        const auto before = loaded.state.layers[0].originalPng;
        auto updated = loaded.state;
        updated.layers[0].name = "renamed";
        writeProject(path, updated, digest);
        QCOMPARE(readProject(path).state.layers[0].originalPng, before);
        QFile file(path + "/manifest.json");
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto json = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(json["version"].toInt(), 11);
        QVERIFY(json["layers"].toArray()[0].toObject()["transform"].toObject()["origin"].isArray());
    }
    void preservesMetadata() {
        QTemporaryDir directory;
        auto s = fixture();
        s.metadata["custom"] = "keep";
        s.layers[0].metadata["customLayer"] = "keep";
        s.layers[0].metadata["text"] = QJsonObject{{"content", "hello"}, {"fontName", "Helvetica"}, {"fontSize", 72}};
        const QString path = directory.path() + "/metadata.comp";
        writeProject(path, s);
        const auto loaded = readProject(path);
        QCOMPARE(loaded.state.metadata["custom"].toString(), "keep");
        QCOMPARE(loaded.state.layers[0].metadata["text"].toObject()["content"].toString(), "hello");
        writeProject(path, loaded.state, loaded.digest);
        QCOMPARE(readProject(path).state.layers[0].originalPng, loaded.state.layers[0].originalPng);
    }
    void externalChangeProtectsOriginal() {
        QTemporaryDir directory;
        auto s = fixture();
        const QString path = directory.path() + "/safe.comp";
        const auto original = writeProject(path, s);
        rewrite(path, [](QJsonObject &j) { j["resolution"] = 144; });
        const auto modified = projectDigest(path);
        QVERIFY(rejects("externalChange", [&] { writeProject(path, s, original); }));
        QCOMPARE(projectDigest(path), modified);
        auto invalid = s;
        invalid.size = {30000, 30000};
        QVERIFY(rejects("tooLarge", [&] { writeProject(path, invalid); }));
        QCOMPARE(projectDigest(path), modified);
    }
    void rejectsUnsafeAssetsAndFeatures() {
        QTemporaryDir directory;
        auto s = fixture();
        const QString path = directory.path() + "/invalid.comp";
        writeProject(path, s);
        rewrite(path, [](QJsonObject &j) {
            auto layers = j["layers"].toArray();
            auto l = layers[0].toObject();
            l["imageFile"] = "../secret.png";
            layers[0] = l;
            j["layers"] = layers;
        });
        QVERIFY(rejects("invalidProject", [&] { readProject(path); }));
        const QString effectPath = directory.path() + "/effects.comp";
        writeProject(effectPath, s);
        rewrite(effectPath, [](QJsonObject &j) {
            auto layers = j["layers"].toArray();
            auto l = layers[0].toObject();
            l["effects"] = QJsonObject{{"unsupportedEffect", QJsonObject{{"opacity", .5}}}};
            layers[0] = l;
            j["layers"] = layers;
        });
        QVERIFY(rejects("unsupportedFeature", [&] { readProject(effectPath); }));
        const QString future = directory.path() + "/future.comp";
        writeProject(future, s);
        rewrite(future, [](QJsonObject &j) { j["version"] = 13; });
        QVERIFY(rejects("unsupportedVersion", [&] { readProject(future); }));
    }
    void rejectsCycles() {
        auto s = fixture();
        Layer group;
        group.id = newId();
        group.name = "Group";
        group.group = true;
        group.parent = group.id;
        s.layers.append(group);
        QVERIFY(rejects("invalidProject", [&] { validateState(s); }));
        s = fixture();
        s.layers[0].maskSource = s.layers[0].id;
        QVERIFY(rejects("invalidProject", [&] { validateState(s); }));
    }
    void renderGroupsMasksAndClipping() {
        auto s = fixture({4, 4});
        Layer group;
        group.id = newId();
        group.name = "group";
        group.group = true;
        group.opacity = .5;
        group.placement.size = s.size;
        s.layers[0].parent = group.id;
        s.layers.append(group);
        auto image = render(s);
        QVERIFY(std::abs(image.pixelColor(1, 1).alpha() - 128) <= 1);
        s.layers[1].visible = false;
        QCOMPARE(render(s).pixelColor(1, 1).alpha(), 0);
        s.layers[1].visible = true;
        s.layers[0].mask = QImage(1, 1, QImage::Format_Grayscale8);
        s.layers[0].mask.fill(QColor(128, 128, 128));
        s.layers[0].maskPlacement = s.layers[0].placement;
        image = render(s);
        QVERIFY(std::abs(image.pixelColor(1, 1).alpha() - 64) <= 2);
        s.layers[1].opacity = 1;
        s.layers[0].parent.clear();
        s.layers[0].visible = false;
        Layer clip = s.layers[0];
        clip.id = newId();
        clip.name = "clip";
        clip.visible = true;
        clip.mask = QImage();
        clip.maskSource = s.layers[0].id;
        clip.image.fill(Qt::blue);
        s.layers.append(clip);
        s.active = clip.id;
        image = render(s);
        QVERIFY(std::abs(image.pixelColor(1, 1).alpha() - 128) <= 2);
        QCOMPARE(image.pixelColor(1, 1).blue(), 255);
        QTemporaryDir dir;
        const QString path = dir.path() + "/mask.comp";
        writeProject(path, s);
        QCOMPARE(readProject(path).state.layers[0].mask.pixelColor(0, 0).red(), 128);
    }
    void blendModesAlpha() {
        for (const auto &mode : blendModes()) {
            const QRgb result = blendPixel(qRgba(90, 45, 0, 128), qRgba(0, 64, 64, 128), mode);
            QVERIFY(qAlpha(result) >= 190 && qAlpha(result) <= 193);
            QVERIFY(qRed(result) <= qAlpha(result));
            QVERIFY(qGreen(result) <= qAlpha(result));
            QVERIFY(qBlue(result) <= qAlpha(result));
            QCOMPARE(blendPixel(qRgba(90, 45, 0, 128), qRgba(0, 0, 0, 0), mode), qRgba(90, 45, 0, 128));
        }
        QCOMPARE(blendPixel(qRgb(128, 128, 128), qRgb(128, 128, 128), "Multiply"), qRgb(64, 64, 64));
        QCOMPARE(blendPixel(qRgb(128, 128, 128), qRgb(128, 128, 128), "Screen"), qRgb(192, 192, 192));
    }
    void historyAndCrop() {
        Document d;
        auto s = fixture({20, 20});
        d.reset(s, true);
        QVERIFY(!d.dirty());
        d.moveBy({3, 4});
        QVERIFY(d.dirty());
        d.undo();
        QVERIFY(!d.dirty());
        QCOMPARE(d.state().layers[0].placement.origin, QPointF());
        d.redo();
        QCOMPARE(d.state().layers[0].placement.origin, QPointF(3, 4));
        const auto revision = d.revision();
        d.markSaved(revision);
        QVERIFY(!d.dirty());
        d.crop(QRect(2, 3, 10, 12));
        QCOMPARE(d.state().size, QSize(10, 12));
        QCOMPARE(d.state().layers[0].placement.origin, QPointF(1, 1));
        d.undo();
        QCOMPARE(d.state().size, QSize(20, 20));
        QVERIFY(!d.dirty());
        d.duplicate("copy");
        QCOMPARE(d.state().layers.size(), 2);
        d.remove();
        QCOMPARE(d.state().layers.size(), 1);
        d.undo();
        QCOMPARE(d.state().layers.size(), 2);
    }
    void selectionEditingAndClipboard() {
        Document d;
        auto s = fixture({30, 20});
        d.reset(s, true);
        QPainterPath rect;
        rect.addRect(5, 3, 10, 8);
        d.setSelection(rect);
        d.fill(Qt::blue);
        QCOMPARE(d.state().layers[0].image.pixelColor(8, 6), QColor(Qt::blue));
        QCOMPARE(d.state().layers[0].image.pixelColor(2, 2), QColor(180, 120, 60));
        QCOMPARE(d.copyPixels().size(), QSize(10, 8));
        d.fill(Qt::transparent, true);
        QCOMPARE(d.state().layers[0].image.pixelColor(8, 6).alpha(), 0);
        QCOMPARE(d.state().layers[0].image.pixelColor(2, 2).alpha(), 255);
        d.undo();
        QCOMPARE(d.state().layers[0].image.pixelColor(8, 6), QColor(Qt::blue));
        d.invertSelection();
        d.fill(Qt::green);
        QCOMPARE(d.state().layers[0].image.pixelColor(2, 2), QColor(Qt::green));
        QCOMPARE(d.state().layers[0].image.pixelColor(8, 6), QColor(Qt::blue));
        d.deselect();
        d.applyFilter({{"kind", "Invert"}});
        QCOMPARE(d.state().layers[0].image.pixelColor(8, 6), QColor(Qt::yellow));
    }
    void transformedSelectionAndMaskActions() {
        Document d;
        auto s = fixture({30, 30});
        s.layers[0].image = QImage(10, 10, QImage::Format_ARGB32_Premultiplied);
        s.layers[0].image.fill(Qt::red);
        s.layers[0].placement.origin = {10, 10};
        s.layers[0].placement.size = {10, 10};
        d.reset(s, true);
        QPainterPath selection;
        selection.addRect(10, 10, 5, 10);
        d.setSelection(selection);
        d.fill(Qt::blue);
        QCOMPARE(d.state().layers[0].image.pixelColor(2, 2), QColor(Qt::blue));
        QCOMPARE(d.state().layers[0].image.pixelColor(7, 2), QColor(Qt::red));
        d.maskAction("addMask");
        auto rendered = comp::render(d.state());
        QCOMPARE(rendered.pixelColor(12, 12).alpha(), 255);
        QCOMPARE(rendered.pixelColor(17, 12).alpha(), 0);
        d.maskAction("invertMask");
        rendered = comp::render(d.state());
        QCOMPARE(rendered.pixelColor(12, 12).alpha(), 0);
        QCOMPARE(rendered.pixelColor(17, 12).alpha(), 255);
        d.maskAction("applyMask");
        QVERIFY(d.state().layers[0].mask.isNull());
        QCOMPARE(d.state().layers[0].image.pixelColor(2, 2).alpha(), 0);
        QCOMPARE(d.state().layers[0].image.pixelColor(7, 2).alpha(), 255);
        d.undo();
        QVERIFY(!d.state().layers[0].mask.isNull());
    }
    void wandConnectivityAndFilterMath() {
        QImage image(12, 8, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::red);
        QPainter p(&image);
        p.fillRect(5, 0, 2, 8, Qt::blue);
        p.end();
        auto region = wandPath(image, {1, 1}, 0, true);
        QVERIFY(region.contains({2, 2}));
        QVERIFY(!region.contains({8, 2}));
        auto all = wandPath(image, {1, 1}, 0, false);
        QVERIFY(all.contains({8, 2}));
        QVERIFY(!all.contains({5, 2}));
        auto gray = filtered(image, {{"kind", "Black & White"}});
        QVERIFY(abs(gray.pixelColor(1, 1).red() - 102) <= 1);
        QCOMPARE(gray.pixelColor(1, 1).red(), gray.pixelColor(1, 1).blue());
        auto exposure =
            filtered(image, {{"kind", "Exposure"},
                             {"exposureSettings", QJsonObject{{"exposure", -1}, {"offset", 0}, {"gamma", 1}}}});
        QVERIFY(abs(exposure.pixelColor(1, 1).red() - 188) <= 1);
        auto hsv = filtered(image, {{"kind", "Hue/Saturation"}, {"hue", 120}, {"saturation", 0}, {"lightness", 0}});
        QVERIFY(hsv.pixelColor(1, 1).green() > 250);
        QVERIFY(hsv.pixelColor(1, 1).red() < 5);
    }
    void adjustmentLayerRoundTrip() {
        auto s = fixture({10, 10});
        s.layers[0].image.fill(Qt::red);
        Layer adjust;
        adjust.id = newId();
        adjust.name = "Invert";
        adjust.placement.size = s.size;
        adjust.metadata["adjustment"] = QJsonObject{{"kind", "Invert"}};
        s.layers.append(adjust);
        QCOMPARE(comp::render(s).pixelColor(2, 2), QColor(Qt::cyan));
        s.layers[1].opacity = .5;
        auto pixel = comp::render(s).pixelColor(2, 2);
        QVERIFY(abs(pixel.red() - 128) <= 1);
        QVERIFY(abs(pixel.green() - 128) <= 1);
        s.layers[1].mask = QImage(10, 10, QImage::Format_Grayscale8);
        s.layers[1].mask.fill(Qt::black);
        s.layers[1].maskPlacement = s.layers[1].placement;
        QCOMPARE(comp::render(s).pixelColor(2, 2), QColor(Qt::red));
        QTemporaryDir dir;
        auto path = dir.path() + "/adjustment.comp";
        writeProject(path, s);
        QCOMPARE(comp::render(readProject(path).state), comp::render(s));
    }
    void layerEffectsRoundTrip() {
        auto s = fixture({40, 40});
        s.layers[0].image.fill(Qt::transparent);
        {
            QPainter p(&s.layers[0].image);
            p.fillRect(12, 12, 16, 16, Qt::red);
        }
        s.layers[0].metadata["effects"] = QJsonObject{
            {"colorOverlay", QJsonObject{{"red", 0}, {"green", 1}, {"blue", 0}, {"opacity", 1}}},
            {"stroke",
             QJsonObject{{"size", 3}, {"red", 0}, {"green", 0}, {"blue", 1}, {"opacity", 1}, {"inside", false}}}};
        auto rendered = comp::render(s);
        QCOMPARE(rendered.pixelColor(20, 20), QColor(Qt::green));
        QVERIFY(rendered.pixelColor(10, 20).blue() > 200);
        QCOMPARE(rendered.pixelColor(1, 1).alpha(), 0);
        QTemporaryDir dir;
        QString path = dir.path() + "/effects.comp";
        writeProject(path, s);
        QCOMPARE(comp::render(readProject(path).state), rendered);
    }
    void colorOverlayKeepsAlpha() {
        QImage source(5, 5, QImage::Format_ARGB32_Premultiplied);
        source.fill(QColor(255, 0, 0, 128));
        QJsonObject effects{{"colorOverlay", QJsonObject{{"red", 0}, {"green", 1}, {"blue", 0}, {"opacity", 1}}}};
        auto image = applyEffects(source, effects, 1);
        QCOMPARE(image.pixelColor(2, 2).alpha(), 128);
        QVERIFY(image.pixelColor(2, 2).green() > 250);
    }
    void adjustmentValidation() {
        QVERIFY(rejects("invalidProject",
                        [] { validateAdjustment({{"kind", "Grain"}, {"grainSettings", QJsonObject{{"size", 0}}}}); }));
        QVERIFY(rejects("invalidProject", [] {
            validateAdjustment({{"kind", "Curves"}, {"curves", QJsonObject{{"channels", QJsonArray{}}}}});
        }));
    }
    void layersGroupingResizeAndHistory() {
        Document d;
        auto s = fixture({20, 10});
        d.reset(s, true);
        d.duplicate("top");
        const QStringList ids{s.active, d.state().active};
        d.groupLayers(ids, "group");
        QCOMPARE(d.state().layers.size(), 3);
        const QString group = d.state().active;
        QCOMPARE(d.state().descendants(group).size(), 2);
        d.ungroup();
        QCOMPARE(d.state().layers.size(), 2);
        for (const auto &layer : d.state().layers)
            QVERIFY(layer.parent.isEmpty());
        d.undo();
        QCOMPARE(d.state().descendants(group).size(), 2);
        d.redo();
        d.resizeCanvas({40, 20}, true);
        QCOMPARE(d.state().layers[0].placement.size, QSizeF(40, 20));
        d.undo();
        QCOMPARE(d.state().size, QSize(20, 10));
        auto saved = d.session();
        d.moveBy({40, 40});
        d.restore(saved);
        QCOMPARE(d.state().layers[0].placement.size, QSizeF(20, 10));
        QVERIFY(d.canRedo());
    }
    void contentAwareFillUsesUpstreamKernel() {
        Document d;
        auto s = fixture({20, 20});
        s.layers[0].image.fill(Qt::green);
        s.layers[0].image.setPixelColor(10, 10, Qt::red);
        d.reset(s, true);
        QPainterPath spot;
        spot.addRect(10, 10, 1, 1);
        d.setSelection(spot);
        d.contentFill();
        QCOMPARE(d.state().layers[0].image.pixelColor(10, 10), QColor(Qt::green));
        d.undo();
        QCOMPARE(d.state().layers[0].image.pixelColor(10, 10), QColor(Qt::red));
    }
    void nativeSelectionGradientClone() {
        MainWindow window;
        window.show();
        auto s = fixture({160, 120});
        s.layers[0].image.fill(Qt::white);
        window.document()->reset(s, true);
        auto *canvas = window.canvas();
        canvas->fit();
        QCoreApplication::processEvents();
        auto point = [&](int x, int y) { return canvas->viewPoint({double(x), double(y)}).toPoint(); };
        canvas->setTool(Tool::Marquee);
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, point(20, 20));
        QTest::mouseMove(canvas, point(100, 90));
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, point(100, 90));
        QVERIFY(window.document()->state().selectionActive);
        QVERIFY(window.document()->state().selection.contains({50, 50}));
        QVERIFY(!window.document()->state().selection.contains({110, 50}));
        canvas->setTool(Tool::Gradient);
        canvas->setColor(Qt::red);
        canvas->setBackgroundColor(Qt::blue);
        canvas->setTransparentGradient(false);
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, point(20, 50));
        QTest::mouseMove(canvas, point(100, 50));
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, point(100, 50));
        auto image = window.document()->state().layers[0].image;
        QVERIFY(image.pixelColor(30, 50).red() > image.pixelColor(80, 50).red());
        QCOMPARE(image.pixelColor(110, 50), QColor(Qt::white));
        window.document()->deselect();
        canvas->setTool(Tool::Clone);
        canvas->setBrushSize(14);
        QTest::mouseClick(canvas, Qt::LeftButton, Qt::AltModifier, point(30, 50));
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, point(120, 50));
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, point(120, 50));
        QVERIFY(window.document()->state().layers[0].image.pixelColor(120, 50).green() < 30);
        window.document()->markSaved(window.document()->revision());
    }
    void textDialogProducesEditablePixels() {
        MainWindow window;
        window.show();
        window.document()->reset(fixture({300, 200}), true);
        window.canvas()->fit();
        window.canvas()->setTool(Tool::Text);
        QTimer::singleShot(30, [] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            dialog->findChild<QTextEdit *>("textContent")->setPlainText("Hello");
            dialog->findChild<QSpinBox *>("textFontSize")->setValue(28);
            dialog->accept();
        });
        QTest::mouseClick(window.canvas(), Qt::LeftButton, Qt::NoModifier,
                          window.canvas()->viewPoint({30, 30}).toPoint());
        QCOMPARE(window.document()->state().layers.size(), 2);
        const auto textId = window.document()->state().active;
        const QImage before = window.document()->state().layers.last().image;
        QCOMPARE(window.document()->state().layers.last().metadata["text"].toObject()["content"].toString(),
                 QString("Hello"));
        QTimer::singleShot(30, [] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            dialog->findChild<QTextEdit *>("textContent")->setPlainText("A much longer line");
            dialog->accept();
        });
        window.findChild<QAction *>("editTextAction")->trigger();
        QVERIFY(window.document()->state().layers.last().image.width() > before.width());
        QCOMPARE(window.document()->state().active, textId);
        window.document()->undo();
        QCOMPARE(window.document()->state().layers.last().image, before);
        window.document()->markSaved(window.document()->revision());
    }
    void filterDialogsAndProjectTabs() {
        bool unexpectedModal = false;
        QTimer modalCheck;
        connect(&modalCheck, &QTimer::timeout, this, [&] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                unexpectedModal = true;
                qWarning() << box->text();
                box->reject();
            }
        });
        modalCheck.start(500);
        MainWindow window;
        window.show();
        window.sample();
        const QString first = window.document()->state().id;
        window.document()->moveBy({7, 8});
        auto position = window.document()
                            ->state()
                            .layers[window.document()->state().index(window.document()->state().active)]
                            .placement.origin;
        window.sample();
        const QString second = window.document()->state().id;
        QVERIFY(first != second);
        auto *tabs = window.findChild<QTabBar *>("documentTabs");
        QCOMPARE(tabs->count(), 2);
        tabs->setCurrentIndex(0);
        QCOMPARE(window.document()->state().id, first);
        QCOMPARE(window.document()
                     ->state()
                     .layers[window.document()->state().index(window.document()->state().active)]
                     .placement.origin,
                 position);
        QVERIFY(window.document()->canUndo());
        window.document()->undo();
        QCOMPARE(window.document()
                     ->state()
                     .layers[window.document()->state().index(window.document()->state().active)]
                     .placement.origin,
                 position - QPointF(7, 8));
        QTimer::singleShot(30, [] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (dialog)
                dialog->accept();
        });
        window.findChild<QAction *>("adjustment.invertAction")->trigger();
        QVERIFY(window.document()->state().layers.last().metadata["adjustment"].isObject() ||
                window.document()
                    ->state()
                    .layers[window.document()->state().index(window.document()->state().active)]
                    .metadata["adjustment"]
                    .isObject());
        window.document()->markSaved(window.document()->revision());
        tabs->setCurrentIndex(1);
        QCOMPARE(window.document()->state().id, second);
        QVERIFY(!window.document()->canUndo());
        window.document()->markSaved(window.document()->revision());
        window.findChild<QAction *>("closeDocumentAction")->trigger();
        QCOMPARE(tabs->count(), 1);
        QVERIFY(!unexpectedModal);
        QCOMPARE(window.document()->state().id, first);
        window.document()->markSaved(window.document()->revision());
    }
    void vectorIconsAndNarrowLayout() {
        for (const auto &key : ToolKeys) {
            auto image = icon(key).pixmap(QSize(24, 24), 2.).toImage();
            int count = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    if (image.pixelColor(x, y).alpha() > 20)
                        ++count;
            QVERIFY2(count > 80, qPrintable(key));
        }
        MainWindow window;
        window.resize(1000, 650);
        window.show();
        window.sample();
        QCoreApplication::processEvents();
        QCOMPARE(window.width(), 1000);
        QVERIFY(window.canvas()->width() > 400);
        for (const auto &key : ToolKeys) {
            if (key == "eraser" || key == "ellipse")
                continue;
            auto *button = window.findChild<QToolButton *>(key + "Button");
            QVERIFY(button);
            QCOMPARE(button->iconSize(), QSize(24, 24));
            QVERIFY(button->width() >= 36);
        }
        window.document()->markSaved(window.document()->revision());
    }
    void actionTranslationsComplete() {
        MainWindow window;
        for (const auto &language : QStringList{"en", "zh-CN", "zh-TW", "ja", "ko", "de", "fr", "es"}) {
            window.localization()->setLanguage(language);
            for (auto *action : window.findChildren<QAction *>()) {
                const QString name = action->objectName();
                if (name.endsWith("Action")) {
                    const QString key = name.chopped(6);
                    QVERIFY2(action->text() != key, qPrintable(language + " " + key));
                }
            }
        }
    }
    void photoshopLayerImport() {
        for (bool large : {false, true})
            for (int compression = 0; compression < 4; ++compression) {
                QString path = QString(COMPOSITOR_FIXTURES) + "/layers-" + QString::number(compression) +
                               (large ? ".psb" : ".psd");
                auto imported = readPhotoshop(path);
                QCOMPARE(imported.state.size, QSize(8, 6));
                QCOMPARE(imported.state.layers.size(), 2);
                QCOMPARE(imported.state.layers[0].name, QString("Background"));
                QCOMPARE(imported.state.layers[1].name, QString("Top"));
                QCOMPARE(imported.state.layers[1].placement.origin, QPointF(2, 1));
                auto image = comp::render(imported.state);
                QCOMPARE(image.pixelColor(0, 0), QColor(160, 30, 20));
                QCOMPARE(image.pixelColor(3, 2), QColor(20, 180, 60));
                QTemporaryDir dir;
                auto saved = dir.path() + "/Imported.comp";
                writeProject(saved, imported.state);
                QCOMPARE(comp::render(readProject(saved).state), image);
            }
    }
    void rawDecodesSensorData() {
        auto frame = decodeRaw(QString(COMPOSITOR_FIXTURES) + "/linear.dng");
        QVERIFY(!frame.linear.isNull());
        QCOMPARE(frame.linear.format(), QImage::Format_RGBA64);
        auto neutral = developRaw(frame), bright = developRaw(frame, 1);
        QCOMPARE(neutral.size(), frame.linear.size());
        QVERIFY(bright.pixelColor(bright.width() / 2, bright.height() / 2).red() >
                neutral.pixelColor(neutral.width() / 2, neutral.height() / 2).red());
        auto warm = developRaw(frame, 0, 10000);
        QVERIFY(warm != neutral);
    }
    void svgRendersNative() {
        auto image = readImage(QString(COMPOSITOR_FIXTURES) + "/vector.svg");
        QCOMPARE(image.size(), QSize(48, 32));
        QCOMPARE(image.pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(image.pixelColor(24, 16), QColor(Qt::blue));
    }
    void cameraRawCompletePipeline() {
        QImage image(40, 30, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < 30; ++y)
            for (int x = 0; x < 40; ++x)
                image.setPixelColor(x, y, QColor(40 + x * 4, 30 + y * 4, 70, 255));
        auto identity = cameraRaw(image, {});
        QVERIFY(abs(identity.pixelColor(20, 15).red() - image.pixelColor(20, 15).red()) <= 2);
        for (const auto &settings : QList<QJsonObject>{
                 {{"exposure", 1}},
                 {{"temperature", 60}},
                 {{"curve", QJsonObject{{"darks", 70}}}},
                 {{"mixer", QJsonObject{{"saturation", QJsonArray{80, 80, 80, 80, 80, 80, 80, 80}}}}},
                 {{"grading",
                   QJsonObject{{"global", QJsonObject{{"hue", 220}, {"saturation", 60}, {"luminance", 0}}}}}},
                 {{"texture", 70}, {"clarity", 60}, {"dehaze", 50}},
                 {{"glow", 70}},
                 {{"vignetteAmount", -80}},
                 {{"grainAmount", 70}},
                 {{"detail", QJsonObject{{"sharpenAmount", 80}, {"noiseLuminance", 30}}}},
                 {{"optics", QJsonObject{{"distortion", 70}}}},
                 {{"geometry", QJsonObject{{"rotate", 15}, {"vertical", 30}}}},
                 {{"calibration", QJsonObject{{"redHue", 80}}}}}) {
            auto output = cameraRaw(image, settings);
            QCOMPARE(output.size(), image.size());
            QVERIFY2(output != identity, qPrintable(QString::fromUtf8(QJsonDocument(settings).toJson())));
        }
    }
    void liquifyAndSmudgeAreUndoable() {
        QImage image(80, 50, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        {
            QPainter p(&image);
            p.fillRect(20, 0, 15, 50, Qt::black);
        }
        auto original = image;
        warpDab(image, {30, 25}, {40, 25}, 18, .3, 1, false);
        QVERIFY(image != original);
        QCOMPARE(image.pixel(1, 1), original.pixel(1, 1));
        auto smudge = original;
        warpDab(smudge, {30, 25}, {40, 25}, 18, .3, .7, true);
        QVERIFY(smudge != original);
        QPainterPath selection;
        selection.addRect(0, 0, 15, 15);
        auto clipped = original;
        warpDab(clipped, {30, 25}, {40, 25}, 18, .3, 1, false, selection);
        QCOMPARE(clipped, original);
    }
    void featherSelectionPreservesSoftEdges() {
        Document document;
        auto state = fixture({60, 40});
        state.layers[0].image.fill(Qt::transparent);
        document.reset(state, true);
        QPainterPath path;
        path.addRect(20, 10, 20, 20);
        document.setSelection(path);
        document.featherSelection(4);
        document.fill(Qt::red);
        auto image = document.state().layers[0].image;
        QVERIFY(image.pixelColor(20, 20).alpha() > 0 && image.pixelColor(20, 20).alpha() < 255);
        QVERIFY(image.pixelColor(17, 20).alpha() > 0);
        QCOMPARE(image.pixelColor(1, 1).alpha(), 0);
        document.undo();
        QCOMPARE(document.state().layers[0].image.pixelColor(30, 20).alpha(), 0);
    }
    void rangedHueAndMotionBlur() {
        QImage image(32, 20, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::blue);
        {
            QPainter p(&image);
            p.fillRect(0, 0, 16, 20, Qt::red);
        }
        QJsonObject hsv{
            {"range", "Reds"},
            {"colorize", false},
            {"adjustments", QJsonArray{"Reds", QJsonObject{{"hue", 120}, {"saturation", 0}, {"lightness", 0}}}}};
        auto result = filtered(image, {{"kind", "Hue/Saturation"}, {"hsvSettings", hsv}});
        QVERIFY(result.pixelColor(3, 3).green() > 240);
        QCOMPARE(result.pixelColor(25, 3), QColor(Qt::blue));
        auto blurred = filtered(image, {{"kind", "Motion Blur"}, {"motionDistance", 8}, {"motionAngle", 0}});
        QVERIFY(blurred.pixelColor(16, 10).red() > 0);
    }
    void richTextRoundTrip() {
        QJsonObject style{
            {"content", "Red Blue"},
            {"fontName", "Arial"},
            {"fontSize", 24},
            {"red", 1},
            {"green", 0},
            {"blue", 0},
            {"alignment", "Left"},
            {"leading", 0},
            {"tracking", 0},
            {"colorRuns",
             QJsonArray{QJsonObject{{"location", 4}, {"length", 4}, {"red", 0}, {"green", 0}, {"blue", 1}}}},
            {"fontRuns", QJsonArray{QJsonObject{{"location", 4}, {"length", 4}, {"fontName", "Courier New"}}}}};
        QTextDocument document;
        loadText(document, style);
        auto restored = textStyle(document, style);
        QVERIFY(!restored["colorRuns"].toArray().isEmpty());
        QVERIFY(!restored["fontRuns"].toArray().isEmpty());
        QVERIFY(!renderText(restored).isNull());
    }
    void localSubjectModelRuns() {
        QImage image(96, 80, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        {
            QPainter p(&image);
            p.setBrush(QColor(200, 40, 20));
            p.drawEllipse(28, 15, 40, 50);
        }
        auto mask = subjectMask(image);
        QCOMPARE(mask.size(), image.size());
        QCOMPARE(mask.format(), QImage::Format_Grayscale8);
        int maximum = 0;
        for (int y = 0; y < mask.height(); ++y)
            for (int x = 0; x < mask.width(); ++x)
                maximum = std::max(maximum, int(mask.constScanLine(y)[x]));
        QVERIFY(maximum > 180);
        auto selected = subjectMask(image, {48, 40});
        QCOMPARE(selected.size(), image.size());
    }
    void groupEffectsRender() {
        auto state = fixture({20, 20});
        Layer group;
        group.id = newId();
        group.name = "group";
        group.group = true;
        group.placement.size = state.size;
        group.metadata["effects"] =
            QJsonObject{{"colorOverlay", QJsonObject{{"red", 0}, {"green", 1}, {"blue", 0}, {"opacity", 1}}}};
        state.layers[0].parent = group.id;
        state.layers.append(group);
        validateState(state);
        QCOMPARE(comp::render(state).pixelColor(10, 10), QColor(Qt::green));
    }
    void cameraRawDialogCancelAndApply() {
        MainWindow window;
        window.show();
        auto state = fixture({80, 50});
        window.document()->reset(state, true);
        window.localization()->setLanguage("zh-CN");
        auto original = window.document()->state().layers[0].image;
        QTimer::singleShot(450, [] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (dialog) {
                QCOMPARE(dialog->objectName(), QString("cameraRawDialog"));
                auto *tabs = dialog->findChild<QTabWidget *>();
                QVERIFY(tabs && tabs->count() == 11);
                auto capture = qEnvironmentVariable("COMPOSITOR_TEST_CAPTURE_DIR");
                if (!capture.isEmpty()) {
                    QDir().mkpath(capture);
                    dialog->grab().save(capture + "/camera-raw-zh-CN.png");
                    tabs->setCurrentIndex(9);
                    QCoreApplication::processEvents();
                    dialog->grab().save(capture + "/camera-raw-geometry.png");
                }
                dialog->findChild<QDoubleSpinBox *>("raw_exposure")->setValue(1);
                dialog->reject();
            }
        });
        window.findChild<QAction *>("filter.cameraRawAction")->trigger();
        QCOMPARE(window.document()->state().layers[0].image, original);
        QTimer::singleShot(150, [] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (dialog) {
                dialog->findChild<QDoubleSpinBox *>("raw_exposure")->setValue(1);
                dialog->accept();
            }
        });
        window.findChild<QAction *>("filter.cameraRawAction")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(window.document()->state().layers[0].image != original, 10000);
        window.document()->undo();
        QCOMPARE(window.document()->state().layers[0].image, original);
        window.document()->markSaved(window.document()->revision());
    }

    void photoshopStructuredLayers() {
        const QString root = COMPOSITOR_FIXTURES;
        auto grouped = readPhotoshop(root + "/group-mask.psd").state;
        QCOMPARE(grouped.layers.size(), 2);
        QVERIFY(grouped.layers.last().group);
        QCOMPARE(grouped.layers[0].parent, grouped.layers.last().id);
        auto rendered = comp::render(grouped);
        QVERIFY(rendered.pixelColor(1, 2).alpha() > 240);
        QCOMPARE(rendered.pixelColor(6, 2).alpha(), 0);
        auto curves = readPhotoshop(root + "/curves.psd").state;
        QVERIFY(curves.layers.last().metadata["adjustment"].isObject());
        QVERIFY(comp::render(curves).pixelColor(2, 2).red() > 80);
        auto shape = readPhotoshop(root + "/shape.psd").state;
        QCOMPARE(shape.layers[0].metadata["shape"].toObject()["kind"].toString(), QString("Rectangle"));
        QCOMPARE(shape.layers[0].image.pixelColor(2, 2), QColor(20, 180, 60));
        auto text = readPhotoshop(root + "/text.psd").state.layers[0].metadata["text"].toObject();
        QCOMPARE(text["content"].toString(), QString("Red Blue"));
        QCOMPARE(text["fontSize"].toInt(), 24);
        QCOMPARE(text["fontRuns"].toArray().size(), 2);
        QCOMPARE(text["colorRuns"].toArray().last().toObject()["blue"].toInt(), 1);
        for (int mode : {2, 3}) {
            auto flattened = readPhotoshop(root + QString("/flattened-%1.psd").arg(mode));
            QCOMPARE(flattened.state.layers[0].image.pixelColor(2, 2), QColor(160, 30, 20));
        }
        QTemporaryDir dir;
        auto path = dir.filePath("roundtrip.comp");
        writeProject(path, grouped);
        QCOMPARE(comp::render(readProject(path).state), rendered);
    }
    void cameraRawPresetsAreBounded() {
        auto settings = normalizedCameraRaw(
            {{"exposure", 1e9},
             {"detail", QJsonObject{{"sharpenRadius", -1e9}}},
             {"geometry", QJsonObject{{"guides", QJsonArray{QJsonArray{0, 0, 1, 1}, QJsonArray{0, 0, 99, 1}}}}},
             {"curve",
              QJsonObject{{"rgb", QJsonArray{QJsonObject{{"x", .5}, {"y", 0}}, QJsonObject{{"x", .4}, {"y", 1}}}}}}});
        QCOMPARE(settings["exposure"].toInt(), 5);
        QCOMPARE(settings["detail"].toObject()["sharpenRadius"].toInt(), 0);
        QCOMPARE(settings["geometry"].toObject()["guides"].toArray().size(), 1);
        QVERIFY(settings.value("curve").toObject().value("rgb").isUndefined());
        auto image = fixture({50, 40}).layers[0].image;
        QVERIFY(!cameraRaw(image, settings).isNull());
        auto clipped = cameraRaw(image, {{"exposure", 5}}, 1, 1);
        QVERIFY(clipped != image);
        auto mask = cameraRaw(image, {{"sharpenMask", true}, {"detail", QJsonObject{{"sharpenMasking", 70}}}});
        QVERIFY(mask != image);
    }
    void nativeLiquifyPolygonAndTransform() {
        MainWindow window;
        window.show();
        auto state = fixture({120, 80});
        state.layers[0].image.fill(Qt::white);
        {
            QPainter p(&state.layers[0].image);
            p.fillRect(35, 0, 20, 80, Qt::black);
        }
        window.document()->reset(state, true);
        auto *canvas = window.canvas();
        canvas->fit();
        QCoreApplication::processEvents();
        canvas->setTool(Tool::Blur);
        canvas->setRetouchMode(2);
        canvas->setBrushSize(32);
        auto a = canvas->viewPoint({45, 40}).toPoint(), b = canvas->viewPoint({61, 40}).toPoint();
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, a);
        QTest::mouseMove(canvas, b);
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, b);
        QVERIFY(window.document()->state().layers[0].image != state.layers[0].image);
        QVERIFY(window.document()->canUndo());
        window.document()->undo();
        QCOMPARE(window.document()->state().layers[0].image, state.layers[0].image);
        canvas->setTool(Tool::Lasso);
        canvas->setPolygonal(true);
        for (QPointF point : QList<QPointF>{{10, 10}, {90, 10}, {50, 70}})
            QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, canvas->viewPoint(point).toPoint());
        QTest::keyClick(canvas, Qt::Key_Return);
        QVERIFY(window.document()->state().selection.contains({50, 25}));
        QVERIFY(!window.document()->state().selection.contains({3, 3}));
        window.document()->deselect();
        QTimer::singleShot(30, [] {
            auto *d = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(d);
            d->findChild<QDoubleSpinBox *>("corner2X")->setValue(108);
            d->accept();
        });
        window.findChild<QAction *>("freeDistortAction")->trigger();
        QVERIFY(window.document()->state().layers[0].image != state.layers[0].image);
        window.document()->undo();
        Layer group;
        group.id = newId();
        group.name = "Group";
        group.group = true;
        group.placement.size = state.size;
        state.layers[0].parent = group.id;
        state.layers.append(group);
        state.active = group.id;
        window.document()->reset(state, true);
        QTimer::singleShot(30, [] {
            auto *d = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(d);
            d->findChild<QDoubleSpinBox *>("scale")->setValue(150);
            d->findChild<QDoubleSpinBox *>("rotation")->setValue(30);
            d->accept();
        });
        window.findChild<QAction *>("transformLayersAction")->trigger();
        QCOMPARE(window.document()->state().layers[0].placement.size, QSizeF(180, 120));
        QCOMPARE(window.document()->state().layers[0].placement.rotation, 30.);
        window.document()->undo();
        QCOMPARE(window.document()->state().layers[0].placement.size, QSizeF(120, 80));
        window.document()->markSaved(window.document()->revision());
    }
    void softSelectionFollowsCanvas() {
        Document document;
        document.reset(fixture({80, 60}), true);
        QPainterPath p;
        p.addRect(12, 10, 20, 20);
        document.setSelection(p);
        document.featherSelection(3);
        auto mask = document.state().selectionMask;
        document.maskAction("addMask");
        QCOMPARE(document.state().layers[0].mask, mask);
        document.crop({5, 5, 60, 40});
        QCOMPARE(document.state().selectionMask, mask.copy({5, 5, 60, 40}));
        auto crop = document.state().selectionMask;
        document.flipCanvas(true);
        QCOMPARE(document.state().selectionMask, crop.mirrored(true, false));
        document.resizeCanvas({120, 80}, true, false);
        QCOMPARE(document.state().selectionMask.size(), QSize(120, 80));
        document.undo();
        document.undo();
        document.undo();
        QCOMPARE(document.state().selectionMask, mask);
        auto image = fixture({400, 50}).layers[0].image;
        auto blurred = filtered(image, {{"kind", "Motion Blur"}, {"motionDistance", 300}});
        QVERIFY(blurred.pixelColor(200, 25).alpha() > 250);
        QVERIFY(abs(blurred.pixelColor(200, 25).red() - 180) <= 1);
    }
    void externalReloadKeepsUnsavedEdits() {
        QTemporaryDir dir;
        QString path = dir.filePath("Live.comp");
        auto state = fixture({40, 30});
        auto digest = writeProject(path, state);
        MainWindow window;
        window.show();
        window.openProject(path);
        QTRY_COMPARE_WITH_TIMEOUT(window.document()->state().id, state.id, 10000);
        state.layers[0].name = "External update";
        digest = writeProject(path, state, digest);
        QTRY_COMPARE_WITH_TIMEOUT(window.document()->state().layers[0].name, QString("External update"), 7000);
        window.document()->moveBy({3, 4});
        auto position = window.document()->state().layers[0].placement.origin;
        state.layers[0].name = "Do not overwrite dirty tab";
        writeProject(path, state, digest);
        QTest::qWait(2300);
        QCOMPARE(window.document()->state().layers[0].name, QString("External update"));
        QCOMPARE(window.document()->state().layers[0].placement.origin, position);
        QVERIFY(window.document()->dirty());
        window.document()->markSaved(window.document()->revision());
    }
    void subjectRemovalIsMaskAndUndoable() {
        MainWindow window;
        window.show();
        auto state = fixture({100, 80});
        state.layers[0].image.fill(Qt::white);
        {
            QPainter p(&state.layers[0].image);
            p.setBrush(QColor(200, 50, 20));
            p.drawEllipse(30, 15, 40, 50);
        }
        window.document()->reset(state, true);
        window.findChild<QAction *>("removeBackgroundAction")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!window.document()->state().layers[0].mask.isNull(), 10000);
        QCOMPARE(window.document()->state().layers[0].image, state.layers[0].image);
        window.document()->undo();
        QVERIFY(window.document()->state().layers[0].mask.isNull());
    }
    void openImagePickerKeepsDocumentsAndImport() {
        QTemporaryDir dir;
        MainWindow window;
        window.show();
        auto *tabs = window.findChild<QTabBar *>("documentTabs");
        QVERIFY(tabs);
        QString firstId;
        for (const auto &format : {QString("png"), QString("jpg")}) {
            const QString path = dir.filePath("照片 with spaces." + format);
            QImage image(37, 29, QImage::Format_ARGB32);
            image.fill(QColor(25, 80, 160));
            QVERIFY(image.save(path));
            const auto picked = pickFile(window, "openAction", path);
            QVERIFY(picked.shown);
            QCOMPARE(picked.mode, QFileDialog::ExistingFile);
            QVERIFY(picked.filters.join(' ').contains("*.png"));
            QVERIFY(picked.filters.join(' ').contains("*.psd"));
            QVERIFY(picked.filters.join(' ').contains("*.dng"));
            const int count = format == "png" ? 1 : 2;
            QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), count, 10000);
            QCOMPARE(window.document()->state().size, image.size());
            QCOMPARE(window.document()->state().layers.size(), 1);
            QCOMPARE(window.document()->state().layers[0].image, readImage(path));
            QVERIFY(tabs->tabText(tabs->currentIndex()).contains("照片 with spaces"));
            if (format == "png") {
                firstId = window.document()->state().id;
                window.document()->moveBy({3, 4});
            }
        }
        const QString secondId = window.document()->state().id;
        QVERIFY(secondId != firstId);
        tabs->setCurrentIndex(0);
        QCOMPARE(window.document()->state().id, firstId);
        QCOMPARE(window.document()->state().layers.size(), 1);
        QCOMPARE(window.document()->state().layers[0].placement.origin, QPointF(3, 4));
        QVERIFY(window.document()->dirty());
        QVERIFY(window.document()->canUndo());
        tabs->setCurrentIndex(1);
        const auto imported = pickFile(window, "importAction", dir.filePath("照片 with spaces.png"));
        QVERIFY(imported.shown);
        QCOMPARE(imported.mode, QFileDialog::ExistingFiles);
        QTRY_COMPARE_WITH_TIMEOUT(window.document()->state().layers.size(), 2, 10000);
        QCOMPARE(tabs->count(), 2);
        QCOMPARE(window.document()->state().id, secondId);
        window.document()->undo();
        QCOMPARE(window.document()->state().layers.size(), 1);
        QVERIFY(pickFile(window, "openAction").shown);
        QCOMPARE(tabs->count(), 2);
        QCOMPARE(window.document()->state().id, secondId);
        const QString invalid = dir.filePath("invalid.png");
        QFile broken(invalid);
        QVERIFY(broken.open(QIODevice::WriteOnly));
        broken.write("not an image");
        broken.close();
        bool reported = false;
        QTimer error;
        connect(&error, &QTimer::timeout, this, [&] {
            if (auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                reported = true;
                dialog->accept();
            }
        });
        error.start(20);
        window.openProject(invalid);
        QTRY_VERIFY_WITH_TIMEOUT(reported, 10000);
        QCOMPARE(tabs->count(), 2);
        QCOMPARE(window.document()->state().id, secondId);
        QCOMPARE(window.document()->state().layers.size(), 1);
    }
    void openProjectPickerStillLoadsComp() {
        QTemporaryDir dir;
        const QString path = dir.filePath("项目 with spaces.comp");
        const auto state = fixture({40, 30});
        writeProject(path, state);
        MainWindow window;
        window.show();
        const auto picked = pickFile(window, "openProjectAction", path);
        QVERIFY(picked.shown);
        QCOMPARE(picked.mode, QFileDialog::Directory);
        QTRY_COMPARE_WITH_TIMEOUT(window.document()->state().id, state.id, 10000);
        QCOMPARE(window.document()->state().size, state.size);
        QCOMPARE(window.document()->state().layers[0].image, state.layers[0].image);
        QVERIFY(!window.document()->dirty());
        QVERIFY(pickFile(window, "openProjectAction").shown);
        QCOMPARE(window.findChild<QTabBar *>("documentTabs")->count(), 1);
        QCOMPARE(window.document()->state().id, state.id);
    }
    void openRawCreatesTabOnlyAfterDevelopment() {
        MainWindow window;
        window.show();
        window.openProject(QString(COMPOSITOR_FIXTURES) + "/layers-0.psd");
        QTRY_VERIFY_WITH_TIMEOUT(window.document()->hasDocument(), 10000);
        auto *tabs = window.findChild<QTabBar *>("documentTabs");
        const QString original = window.document()->state().id;
        bool accept = false;
        int dialogs = 0;
        QTimer modal;
        connect(&modal, &QTimer::timeout, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (dialog && dialog->objectName() == "rawDevelopDialog") {
                ++dialogs;
                if (accept)
                    dialog->accept();
                else
                    dialog->reject();
            }
        });
        modal.start(20);
        window.openProject(QString(COMPOSITOR_FIXTURES) + "/linear.dng");
        QTRY_COMPARE_WITH_TIMEOUT(dialogs, 1, 10000);
        QCOMPARE(tabs->count(), 1);
        QCOMPARE(window.document()->state().id, original);
        accept = true;
        window.openProject(QString(COMPOSITOR_FIXTURES) + "/linear.dng");
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 2, 10000);
        QCOMPARE(dialogs, 2);
        QCOMPARE(window.document()->state().layers.size(), 1);
        QVERIFY(window.document()->state().id != original);
        tabs->setCurrentIndex(0);
        QCOMPARE(window.document()->state().id, original);
        QCOMPARE(window.document()->state().layers.size(), 2);
    }
    void importPsdAndRawThroughWindow() {
        MainWindow window;
        window.show();
        window.openProject(QString(COMPOSITOR_FIXTURES) + "/layers-0.psd");
        QTRY_VERIFY_WITH_TIMEOUT(window.document()->hasDocument(), 10000);
        QCOMPARE(window.document()->state().layers.size(), 2);
        bool developed = false;
        QTimer modal;
        connect(&modal, &QTimer::timeout, this, [&] {
            auto *d = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (d && d->objectName() == "rawDevelopDialog") {
                developed = true;
                d->accept();
            }
        });
        modal.start(50);
        window.canvas()->filesDropped({QString(COMPOSITOR_FIXTURES) + "/linear.dng"});
        QTRY_COMPARE_WITH_TIMEOUT(window.document()->state().layers.size(), 3, 10000);
        QVERIFY(developed);
        window.document()->undo();
        QCOMPARE(window.document()->state().layers.size(), 2);
        window.document()->markSaved(window.document()->revision());
    }

    void inlineTextAcceptCancelAndShortcutsPersist() {
        MainWindow window;
        window.show();
        auto state = fixture({300, 180});
        QJsonObject style{{"content", "Red Blue"},
                          {"fontName", "Arial"},
                          {"fontSize", 24},
                          {"red", 1},
                          {"green", 0},
                          {"blue", 0},
                          {"alignment", "Left"},
                          {"tracking", 0},
                          {"leading", 0},
                          {"colorRuns", QJsonArray{QJsonObject{
                                            {"location", 4}, {"length", 4}, {"red", 0}, {"green", 0}, {"blue", 1}}}}};
        state.layers[0].metadata["text"] = style;
        state.layers[0].image = renderText(style);
        state.layers[0].placement.size = state.layers[0].image.size();
        window.document()->reset(state, true);
        window.canvas()->fit();
        window.canvas()->beginInlineText(state.active);
        auto *editor = window.canvas()->findChild<QTextEdit *>("inlineTextEditor");
        QVERIFY(editor);
        editor->moveCursor(QTextCursor::End);
        editor->insertPlainText("!");
        QTest::keyClick(editor, Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(window.document()->state().layers[0].metadata["text"].toObject()["content"].toString(),
                 QString("Red Blue!"));
        QVERIFY(!window.document()->state().layers[0].metadata["text"].toObject()["colorRuns"].toArray().isEmpty());
        window.document()->undo();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        window.canvas()->beginInlineText(state.active);
        editor = window.canvas()->findChild<QTextEdit *>("inlineTextEditor");
        QVERIFY(editor);
        editor->setPlainText("Canceled");
        QTest::keyClick(editor, Qt::Key_Escape);
        QCOMPARE(window.document()->state().layers[0].metadata["text"].toObject()["content"].toString(),
                 QString("Red Blue"));
        window.findChild<QAction *>("shortcutsAction")->trigger();
        auto *page = window.findChild<PreferencesDialog *>();
        QVERIFY(page);
        auto *key = page->findChild<QKeySequenceEdit *>("transformLayersActionShortcut");
        QVERIFY(key);
        key->setKeySequence(QKeySequence("Ctrl+Alt+T"));
        page->findChild<QPushButton *>("applyShortcuts")->click();
        page->close();
        QCOMPARE(window.findChild<QAction *>("transformLayersAction")->shortcut(), QKeySequence("Ctrl+Alt+T"));
        {
            MainWindow restored;
            QCOMPARE(restored.findChild<QAction *>("transformLayersAction")->shortcut(), QKeySequence("Ctrl+Alt+T"));
        }
        QSettings().remove("shortcuts/transformLayersAction");
        window.document()->markSaved(window.document()->revision());
    }
    void heicWithAlphaDecodes() {
        const QString fixture = qEnvironmentVariable("COMPOSITOR_HEIC_FIXTURE");
        if (fixture.isEmpty())
            QSKIP("Set COMPOSITOR_HEIC_FIXTURE to the libheif with-alpha-512x512.heic fixture.");
        auto image = readHeif(fixture);
        QCOMPARE(image.size(), QSize(512, 512));
        int low = 255, high = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                int a = image.pixelColor(x, y).alpha();
                low = std::min(low, a);
                high = std::max(high, a);
            }
        QVERIFY(low < 128 && high > 240);
    }

    void cameraRawPagesHaveTranslatedLabels() {
        MainWindow window;
        window.show();
        window.sample();
        for (const auto &entry : Localization::languages()) {
            window.localization()->setLanguage(entry.first);
            QTimer::singleShot(500, [&] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                QVERIFY(dialog);
                auto *sections = dialog->findChild<QComboBox *>("rawSection");
                QVERIFY(sections);
                QCOMPARE(sections->count(), 11);
                for (auto *label : dialog->findChildren<QLabel *>())
                    QVERIFY2(!label->text().startsWith("raw."), qPrintable(entry.first + " " + label->text()));
                const QString capture = qEnvironmentVariable("COMPOSITOR_TEST_CAPTURE_DIR");
                if (!capture.isEmpty()) {
                    QDir().mkpath(capture);
                    for (int i = 0; i < 11; ++i) {
                        sections->setCurrentIndex(i);
                        QCoreApplication::processEvents();
                        QVERIFY(dialog->grab().save(capture + "/camera-raw-" + entry.first + "-" + QString::number(i) +
                                                    ".png"));
                    }
                }
                dialog->reject();
            });
            window.findChild<QAction *>("filter.cameraRawAction")->trigger();
        }
        window.document()->markSaved(window.document()->revision());
    }
    void preferencesPreserveDocumentsAndPersist() {
        QSettings().remove("preferences");
        QSettings().setValue("language", "en");
        MainWindow window;
        window.show();
        window.sample();
        window.document()->edit([](State &s) { s.layers[0].name = "Keep this name"; });
        const auto before = window.document()->state();
        const auto revision = window.document()->revision();
        const auto pixels = render(before);
        window.findChild<QAction *>("preferencesAction")->trigger();
        auto *dialog = window.findChild<PreferencesDialog *>();
        QVERIFY(dialog);
        QVERIFY(dialog->isWindow());
        QVERIFY(dialog->isModal());
        QCOMPARE(QApplication::activeModalWidget(), dialog);
        QVERIFY(!window.findChild<QTabWidget *>("workspacePages"));
        QCOMPARE(window.findChild<QTabBar *>("documentTabs")->count(), 1);
        auto *language = window.findChild<QComboBox *>("languageSelector");
        QCOMPARE(language->count(), 9);
        for (const auto &entry : Localization::languages()) {
            language->setCurrentIndex(language->findData(entry.first));
            QCOMPARE(window.localization()->language(), entry.first);
            QCOMPARE(dialog->windowTitle(), window.localization()->text("preferences"));
            QCOMPARE(window.document()->revision(), revision);
            QCOMPARE(window.document()->state().layers[0].name, QString("Keep this name"));
            QVERIFY(window.document()->dirty());
        }
        language->setCurrentIndex(language->findData("ja"));
        window.findChild<QCheckBox *>("rulersPreference")->setChecked(true);
        window.findChild<QCheckBox *>("gridPreference")->setChecked(true);
        window.findChild<QCheckBox *>("snapPreference")->setChecked(false);
        window.findChild<QCheckBox *>("rememberWindowPreference")->setChecked(false);
        window.findChild<QCheckBox *>("autoReloadPreference")->setChecked(false);
        window.findChild<QSpinBox *>("gridSpacingPreference")->setValue(72);
        QCOMPARE(render(window.document()->state()), pixels);
        QCOMPARE(window.document()->revision(), revision);
        dialog->findChild<QPushButton *>("closePreferences")->click();
        QVERIFY(!dialog->isVisible());
        QVERIFY(!QApplication::activeModalWidget());
        QVERIFY(window.findChild<QAction *>("undoAction")->isEnabled());
        window.findChild<QAction *>("undoAction")->trigger();
        QVERIFY(window.document()->state().layers[0].name != "Keep this name");
        {
            MainWindow restored;
            QCOMPARE(restored.localization()->language(), QString("ja"));
            QCOMPARE(restored.localization()->selection(), QString("ja"));
            QVERIFY(restored.findChild<QAction *>("showRulersAction")->isChecked());
            QVERIFY(restored.findChild<QAction *>("showGridAction")->isChecked());
            QVERIFY(!restored.findChild<QAction *>("snapAction")->isChecked());
            QCOMPARE(Preferences::load().gridSpacing, 72);
            QVERIFY(!Preferences::load().rememberWindow);
            QVERIFY(!Preferences::load().autoReload);
            restored.findChild<QAction *>("preferencesAction")->trigger();
            restored.findChild<QComboBox *>("languageSelector")->setCurrentIndex(0);
            QCOMPARE(restored.localization()->selection(), QString("system"));
            QCOMPARE(QSettings().value("language").toString(), QString("system"));
            restored.findChild<QPushButton *>("resetPreferences")->click();
            QVERIFY(Preferences::load().rememberWindow);
            QVERIFY(Preferences::load().autoReload);
            // Resetting General does not reset Canvas preferences.
            QCOMPARE(Preferences::load().gridSpacing, 72);
        }
        QSettings().remove("preferences");
        QSettings().setValue("language", "en");
        window.document()->markSaved(window.document()->revision());
    }
    void preferencesNewCanvasDefaultsAndReset() {
        QSettings().remove("preferences");
        MainWindow window;
        window.show();
        window.findChild<QAction *>("preferencesAction")->trigger();
        auto *sections = window.findChild<QTabWidget *>("preferenceSections");
        sections->setCurrentIndex(1);
        auto *depth = window.findChild<QComboBox *>("defaultDepthPreference");
        auto *resolution = window.findChild<QComboBox *>("defaultResolutionPreference");
        depth->setCurrentIndex(depth->findData(16));
        resolution->setCurrentIndex(resolution->findData(300));
        QCOMPARE(window.findChild<QComboBox *>("welcomeBitDepth")->currentData().toInt(), 16);
        window.findChild<PreferencesDialog *>()->close();
        bool dialogChecked = false;
        QTimer::singleShot(40, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            QCOMPARE(dialog->findChild<QComboBox *>("canvasBitDepth")->currentData().toInt(), 16);
            QCOMPARE(dialog->findChild<QComboBox *>("canvasDpi")->currentData().toInt(), 300);
            dialog->findChild<QDoubleSpinBox *>("canvasWidth")->setValue(32);
            dialog->findChild<QDoubleSpinBox *>("canvasHeight")->setValue(24);
            dialogChecked = true;
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        window.findChild<QAction *>("newAction")->trigger();
        QVERIFY(dialogChecked);
        QVERIFY(!window.findChild<PreferencesDialog *>()->isVisible());
        QCOMPARE(window.document()->state().bitDepth, 16);
        QCOMPARE(window.document()->state().resolution, 300.);
        const auto revision = window.document()->revision();
        window.findChild<QAction *>("preferencesAction")->trigger();
        window.findChild<QPushButton *>("resetPreferences")->click();
        QCOMPARE(Preferences::load().depth, 8);
        QCOMPARE(Preferences::load().resolution, 72);
        QCOMPARE(window.document()->state().bitDepth, 16);
        QCOMPARE(window.document()->revision(), revision);
        QSettings().remove("preferences");
        window.document()->markSaved(window.document()->revision());
    }
    void preferencesShortcutsConflictAndRestore() {
        QSettings().remove("shortcuts");
        MainWindow window;
        window.findChild<QAction *>("shortcutsAction")->trigger();
        QCOMPARE(window.findChild<QTabWidget *>("preferenceSections")->currentIndex(), 2);
        auto *editor = window.findChild<QKeySequenceEdit *>("transformLayersActionShortcut");
        auto *action = window.findChild<QAction *>("transformLayersAction");
        auto *apply = window.findChild<QPushButton *>("applyShortcuts");
        const auto original = action->shortcut();
        for (const QString key : {QString("Ctrl+S"), QString("Ctrl+Y"), QString("Esc"), QString("Del")}) {
            editor->setKeySequence(QKeySequence(key));
            apply->click();
            QCOMPARE(action->shortcut(), original);
            QVERIFY(!QSettings().contains("shortcuts/transformLayersAction"));
            QVERIFY(window.findChild<QLabel *>("shortcutStatus")->text().contains(key));
        }
        editor->setKeySequence(QKeySequence("Ctrl+Alt+T"));
        auto *language = window.findChild<QComboBox *>("languageSelector");
        language->setCurrentIndex(language->findData("fr"));
        QCOMPARE(editor->keySequence(), QKeySequence("Ctrl+Alt+T"));
        QCOMPARE(action->shortcut(), original);
        apply->click();
        QCOMPARE(action->shortcut(), QKeySequence("Ctrl+Alt+T"));
        {
            MainWindow restored;
            QCOMPARE(restored.localization()->language(), QString("fr"));
            QCOMPARE(restored.findChild<QAction *>("transformLayersAction")->shortcut(), QKeySequence("Ctrl+Alt+T"));
        }
        window.findChild<QPushButton *>("resetPreferences")->click();
        QCOMPARE(action->shortcut(), QKeySequence("Ctrl+Alt+T"));
        apply->click();
        QCOMPARE(action->shortcut(), original);
        QVERIFY(!QSettings().contains("shortcuts/transformLayersAction"));
        QSettings().remove("shortcuts");
        QSettings().setValue("language", "en");
    }
    void preferencesDialogLifecycle() {
        QSettings().remove("shortcuts");
        MainWindow window;
        window.show();
        window.sample();
        QCoreApplication::processEvents();
        const auto canvasGeometry = window.canvas()->geometry();
        const auto revision = window.document()->revision();
        auto *preferencesAction = window.findChild<QAction *>("preferencesAction");
        QCOMPARE(preferencesAction->shortcut(), QKeySequence("Ctrl+,"));
        preferencesAction->trigger();
        auto *dialog = window.findChild<PreferencesDialog *>();
        QVERIFY(dialog);
        QCOMPARE(dialog->parentWidget(), &window);
        QCOMPARE(dialog->windowModality(), Qt::WindowModal);
        auto *editor = dialog->findChild<QKeySequenceEdit *>("transformLayersActionShortcut");
        auto *action = window.findChild<QAction *>("transformLayersAction");
        const auto original = action->shortcut();
        auto *sections = dialog->findChild<QTabWidget *>("preferenceSections");
        sections->setCurrentIndex(2);
        editor->setKeySequence(QKeySequence("Ctrl+Alt+T"));
        preferencesAction->trigger();
        QCOMPARE(window.findChildren<PreferencesDialog *>().size(), 1);
        QCOMPARE(editor->keySequence(), QKeySequence("Ctrl+Alt+T"));
        QCOMPARE(window.canvas()->geometry(), canvasGeometry);
        QCOMPARE(window.document()->revision(), revision);
        dialog->findChild<QPushButton *>("closePreferences")->click();
        QVERIFY(!dialog->isVisible());
        QCOMPARE(action->shortcut(), original);
        preferencesAction->trigger();
        QCOMPARE(editor->keySequence(), original);
        editor->setKeySequence(QKeySequence("Ctrl+Alt+T"));
        QTest::keyClick(dialog->findChild<QPushButton *>("closePreferences"), Qt::Key_Escape);
        QVERIFY(!dialog->isVisible());
        preferencesAction->trigger();
        QCOMPARE(editor->keySequence(), original);
        editor->setKeySequence(QKeySequence("Ctrl+Alt+T"));
        dialog->findChild<QPushButton *>("applyShortcuts")->click();
        QVERIFY(dialog->isVisible());
        dialog->close();
        QVERIFY(!dialog->isVisible());
        preferencesAction->trigger();
        QCOMPARE(editor->keySequence(), QKeySequence("Ctrl+Alt+T"));
        editor->setKeySequence(QKeySequence("Ctrl+Alt+U"));
        dialog->close();
        preferencesAction->trigger();
        QCOMPARE(editor->keySequence(), QKeySequence("Ctrl+Alt+T"));
        dialog->close();
        QVERIFY(!QApplication::activeModalWidget());
        QVERIFY(window.canvas()->isVisible());
        QCOMPARE(window.canvas()->geometry(), canvasGeometry);
        QCOMPARE(window.document()->revision(), revision);
        QSettings().remove("shortcuts");
        window.document()->markSaved(window.document()->revision());
    }
    void preferencesCaptureLayouts() {
        const QString output = qEnvironmentVariable("COMPOSITOR_PREFERENCES_CAPTURES");
        MainWindow window;
        window.resize(1100, 800);
        window.show();
        window.findChild<QAction *>("preferencesAction")->trigger();
        auto *dialog = window.findChild<PreferencesDialog *>();
        auto *sections = dialog->findChild<QTabWidget *>("preferenceSections");
        for (const auto &entry : Localization::languages()) {
            window.localization()->setLanguage(entry.first);
            for (const QSize size : {QSize(820, 640), QSize(680, 480)}) {
                dialog->resize(size);
                for (int section = 0; section < 3; ++section) {
                    sections->setCurrentIndex(section);
                    QCoreApplication::processEvents();
                    auto *shortcut = dialog->findChild<QKeySequenceEdit *>("aboutActionShortcut");
                    QCOMPARE(shortcut->findChild<QLineEdit *>()->placeholderText(),
                             window.localization()->text("pressShortcut"));
                    QCOMPARE(dialog->windowTitle(), window.localization()->text("preferences"));
                    QCOMPARE(window.size(), QSize(1100, 800));
                    QCOMPARE(dialog->size(), size);
                    auto *close = dialog->findChild<QPushButton *>("closePreferences");
                    QVERIFY(close->isVisible());
                    QVERIFY(dialog->rect().contains(QRect(close->mapTo(dialog, QPoint()), close->size())));
                    if (!output.isEmpty()) {
                        QDir().mkpath(output);
                        QVERIFY(dialog->grab().save(output + "/preferences-" + entry.first + "-" +
                                                    QString::number(section) + "-" + QString::number(size.width()) +
                                                    ".png"));
                    }
                }
            }
        }
        dialog->close();
        QSettings().setValue("language", "en");
    }
    void standardDialogsFollowLanguage() {
        Localization l;
        const QList<QPair<QString, QString>> words{
            {"de", "Abbrechen"}, {"fr", "Annuler"}, {"es", "Cancelar"}, {"ja", "キャンセル"}, {"ko", "취소"}};
        for (const auto &entry : words) {
            l.setLanguage(entry.first);
            QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
            QCOMPARE(buttons.button(QDialogButtonBox::Cancel)->text().remove('&'), entry.second);
        }
        l.setLanguage("en");
    }
    void localizationCompleteness() {
        QFile en(":/translations/en.json");
        QVERIFY(en.open(QIODevice::ReadOnly));
        const auto english = QJsonDocument::fromJson(en.readAll()).object();
        for (const auto &lang : QStringList{"zh-CN", "zh-TW", "ja", "ko", "de", "fr", "es"}) {
            QFile f(":/translations/" + lang + ".json");
            QVERIFY(f.open(QIODevice::ReadOnly));
            const auto translated = QJsonDocument::fromJson(f.readAll()).object();
            QCOMPARE(translated.keys(), english.keys());
            auto tokens = [](const QString &value) {
                QStringList result;
                auto matches = QRegularExpression("\\{[^{}]+\\}|\\*\\.[a-z0-9]+").globalMatch(value);
                while (matches.hasNext())
                    result.append(matches.next().captured());
                result.sort();
                return result;
            };
            for (const auto &key : english.keys()) {
                QVERIFY2(!translated[key].toString().isEmpty(), qPrintable(lang + " " + key));
                QCOMPARE(tokens(translated[key].toString()), tokens(english[key].toString()));
            }
        }
        QCOMPARE(Localization::normalize("zh_Hant_HK"), QString("zh-TW"));
        QCOMPARE(Localization::normalize("zh-Hans"), QString("zh-CN"));
        QCOMPARE(Localization::normalize("fr-FR"), QString("fr"));
        QCOMPARE(Localization::normalize("DE_at"), QString("de"));
        QCOMPARE(Localization::normalize("es-MX"), QString("es"));
        QCOMPARE(Localization::normalize("ja_JP"), QString("ja"));
        QCOMPARE(Localization::normalize("ko-KR"), QString("ko"));
        QCOMPARE(Localization::normalize("unsupported"), QString("en"));
        Localization l;
        l.setLanguage("zh-CN");
        QCOMPARE(l.text("save"), QString("保存"));
        l.setLanguage("zh-TW");
        QCOMPARE(l.text("save"), QString("儲存"));
        l.setLanguage("en");
        QCOMPARE(l.text("save"), QString("Save"));
    }
    void nativeBrushAndLanguageUi() {
        MainWindow window;
        window.show();
        auto s = fixture({200, 200});
        s.layers[0].image.fill(Qt::transparent);
        window.document()->reset(s, true);
        window.canvas()->fit();
        QCoreApplication::processEvents();
        auto *canvas = window.canvas();
        canvas->setTool(Tool::Brush);
        canvas->setColor(Qt::red);
        canvas->setBrushSize(20);
        const QPoint start = canvas->viewPoint({60, 60}).toPoint(), end = canvas->viewPoint({100, 60}).toPoint();
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(canvas, end);
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, end);
        QVERIFY(window.document()->dirty());
        QVERIFY(window.document()->state().layers[0].image.pixelColor(60, 60).red() > 200);
        window.document()->undo();
        QCOMPARE(window.document()->state().layers[0].image.pixelColor(60, 60).alpha(), 0);
        window.localization()->setLanguage("zh-CN");
        QCOMPARE(window.findChild<QAction *>("saveAction")->text(), QString("保存"));
        QCOMPARE(window.document()->state().id, s.id);
        auto masked = window.document()->state();
        masked.layers[0].mask = QImage(1, 1, QImage::Format_Grayscale8);
        masked.layers[0].mask.fill(Qt::white);
        masked.layers[0].maskPlacement = masked.layers[0].placement;
        window.document()->reset(masked, true);
        canvas->setMaskTarget(true);
        canvas->setTool(Tool::Eraser);
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, start);
        QCOMPARE(window.document()->state().layers[0].mask.size(), QSize(200, 200));
        QVERIFY(window.document()->state().layers[0].mask.pixelColor(60, 60).red() < 20);
        QCOMPARE(window.document()->state().layers[0].mask.pixelColor(190, 190).red(), 255);
        window.document()->undo();
        QTest::keyClick(canvas, Qt::Key_Delete);
        QCOMPARE(window.document()->state().layers.size(), 0);
        window.document()->undo();
        window.document()->markSaved(window.document()->revision());
    }
};
int main(int argc, char **argv) {
    Q_INIT_RESOURCE(translations);
    Q_INIT_RESOURCE(icons);
    Q_INIT_RESOURCE(controls);
    QApplication app(argc, argv);
    QImageReader::setAllocationLimit(1024);
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QCoreApplication::setOrganizationName("CompositorTests");
    QCoreApplication::setApplicationName("Tests");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QTemporaryDir settings;
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    Tests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "Tests.moc"
