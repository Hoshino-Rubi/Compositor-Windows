#include "CameraRaw.h"
#include "Hdr.h"
#include "Filters.h"
#include "SpatialFilters.h"
#include "Cmyk.h"
#include "Export.h"
#include "Renderer.h"
#include "RetouchStroke.h"
#include "LayerClipboard.h"
#include "LayerSearch.h"
#include <QLineEdit>
#include <QDialogButtonBox>
#include "Selection.h"
#include <lcms2.h>
#include <QDataStream>
#include <QPainter>
#include <QImageReader>
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
    QImage image(96, 80,
                 depth == 16 ? QImage::Format_RGBA64_Premultiplied : QImage::Format_RGBA8888_Premultiplied);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const auto color =
                QColor::fromRgba64(10001 + x * 73, 14003 + y * 59, 23009 + (x + y) * 17, 65535);
            image.setPixelColor(x, y, color);
        }
    return image;
}
State document(int depth) {
    State s;
    s.id = newId();
    s.bitDepth = depth;
    Layer l;
    l.id = newId();
    l.name = "Layer";
    l.image = pixels(depth);
    s.size = l.image.size();
    l.placement.size = s.size;
    s.layers.append(l);
    s.active = l.id;
    return s;
}
void manifest(const QString &directory, const std::function<void(QJsonObject &)> &change) {
    QFile f(directory + "/manifest.json");
    if (!f.open(QIODevice::ReadOnly))
        qFatal("Missing fixture manifest");
    auto json = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    change(json);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        qFatal("Cannot rewrite fixture");
    f.write(QJsonDocument(json).toJson());
}
int patch(QImage &image, const QImage &mask, int depth) {
    return depth == 16
               ? wide::patch_fill(reinterpret_cast<quint16 *>(image.bits()), image.bytesPerLine() / 2,
                                  mask.constBits(), mask.bytesPerLine(), image.width(), image.height(), 123)
               : patch_fill(image.bits(), image.bytesPerLine(), mask.constBits(), mask.bytesPerLine(),
                            image.width(), image.height(), 123);
}
bool rejects(const QString &key, const std::function<void()> &operation) {
    try {
        operation();
    } catch (const Error &e) {
        return e.key == key;
    }
    return false;
}
} // namespace

QByteArray syntheticCmyk() {
    cmsHPROFILE profile = cmsCreateProfilePlaceholder(nullptr), rgb = cmsCreate_sRGBProfile(),
                lab = cmsCreateLab4Profile(nullptr);
    cmsSetProfileVersion(profile, 2.1);
    cmsSetDeviceClass(profile, cmsSigOutputClass);
    cmsSetColorSpace(profile, cmsSigCmykData);
    cmsSetPCS(profile, cmsSigLabData);
    cmsWriteTag(profile, cmsSigMediaWhitePointTag, cmsD50_XYZ());
    cmsMLU *text = cmsMLUalloc(nullptr, 1);
    cmsMLUsetASCII(text, "en", "US", "Compositor synthetic CMYK test profile (not for printing)");
    cmsWriteTag(profile, cmsSigProfileDescriptionTag, text);
    cmsMLUfree(text);
    auto forward = cmsCreateTransform(rgb, TYPE_RGB_16, lab, TYPE_Lab_16, INTENT_RELATIVE_COLORIMETRIC, 0);
    auto reverse = cmsCreateTransform(lab, TYPE_Lab_16, rgb, TYPE_RGB_16, INTENT_RELATIVE_COLORIMETRIC, 0);
    auto *a = cmsPipelineAlloc(nullptr, 4, 3), *b = cmsPipelineAlloc(nullptr, 3, 4);
    auto *ac = cmsStageAllocCLut16bit(nullptr, 9, 4, 3, nullptr),
         *bc = cmsStageAllocCLut16bit(nullptr, 17, 3, 4, nullptr);
    cmsStageSampleCLut16bit(
        ac,
        [](const cmsUInt16Number in[], cmsUInt16Number out[], void *data) -> cmsInt32Number {
            cmsUInt16Number rgb[3];
            for (int c = 0; c < 3; ++c)
                rgb[c] = cmsUInt16Number((65535 - in[c]) * (65535. - in[3]) / 65535.);
            cmsDoTransform(data, rgb, out, 1);
            return 1;
        },
        forward, 0);
    cmsStageSampleCLut16bit(
        bc,
        [](const cmsUInt16Number in[], cmsUInt16Number out[], void *data) -> cmsInt32Number {
            cmsUInt16Number rgb[3];
            cmsDoTransform(data, in, rgb, 1);
            const int k = 65535 - std::max({int(rgb[0]), int(rgb[1]), int(rgb[2])});
            out[3] = cmsUInt16Number(k);
            for (int c = 0; c < 3; ++c)
                out[c] = cmsUInt16Number(k == 65535 ? 0 : (65535 - rgb[c] - k) * 65535. / (65535 - k));
            return 1;
        },
        reverse, 0);
    cmsPipelineInsertStage(a, cmsAT_END, cmsStageAllocToneCurves(nullptr, 4, nullptr));
    cmsPipelineInsertStage(a, cmsAT_END, ac);
    cmsPipelineInsertStage(a, cmsAT_END, cmsStageAllocToneCurves(nullptr, 3, nullptr));
    cmsPipelineInsertStage(b, cmsAT_END, cmsStageAllocToneCurves(nullptr, 3, nullptr));
    cmsPipelineInsertStage(b, cmsAT_END, bc);
    cmsPipelineInsertStage(b, cmsAT_END, cmsStageAllocToneCurves(nullptr, 4, nullptr));
    if (!cmsWriteTag(profile, cmsSigAToB0Tag, a) || !cmsWriteTag(profile, cmsSigBToA0Tag, b))
        qFatal("Cannot construct test profile");
    cmsUInt32Number size = 0;
    if (!cmsSaveProfileToMem(profile, nullptr, &size))
        qFatal("Cannot encode test profile");
    QByteArray data(size, 0);
    if (!cmsSaveProfileToMem(profile, data.data(), &size))
        qFatal("Cannot encode test profile");
    cmsPipelineFree(a);
    cmsPipelineFree(b);
    cmsDeleteTransform(forward);
    cmsDeleteTransform(reverse);
    cmsCloseProfile(profile);
    cmsCloseProfile(rgb);
    cmsCloseProfile(lab);
    return data;
}
State hdrDocument() {
    State s = document(16);
    s.layers[0].image = QImage(s.size, QImage::Format_RGBA32FPx4_Premultiplied);
    s.layers[0].image.setColorSpace(QColorSpace::SRgb);
    for (int y = 0; y < s.size.height(); ++y) {
        auto *row = reinterpret_cast<FloatPixel *>(s.layers[0].image.scanLine(y));
        for (int x = 0; x < s.size.width(); ++x)
            row[x] = {4.125f + x * .001f, .375f, 1.5f, 1};
    }
    return s;
}

class GpuUpstreamTests : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        Q_INIT_RESOURCE(translations);
        Q_INIT_RESOURCE(icons);
        Q_INIT_RESOURCE(controls);
        QCoreApplication::setOrganizationName("CompositorTests");
        QCoreApplication::setApplicationName("UpstreamTests");
    }
    void v16FilterLayersRoundTripAndFallBackCorrectly() {
        for (int depth : {8, 16}) for (const auto kind : {"Progressive Blur", "Bloom / Glow", "RGB Split"}) {
            State input = document(depth);
            Layer layer; layer.id = newId(); layer.name = kind; layer.placement.size = input.size;
            auto settings = defaultFilterLayerSettings(input.size);
            settings["rgbSplitDistance"] = 3.25; settings["rgbSplitBlur"] = 1.5;
            layer.metadata["adjustment"] = QJsonObject{{"kind", kind}, {"filterLayerSettings", settings}};
            input.layers.append(layer);
            validateState(input);
            const auto expected = render(input);
            QVERIFY(!expected.isNull());
            QCOMPARE(wideImage(expected), depth == 16);
            QCOMPARE(renderPreview(input), expected);
            QTemporaryDir dir;
            const auto path = dir.path() + "/filter.comp";
            writeProject(path, input, {}, true);
            const auto loaded = readProject(path).state;
            QCOMPARE(loaded.metadata["version"].toInt(), 16);
            QCOMPARE(loaded.layers.last().metadata["adjustment"].toObject(), layer.metadata["adjustment"].toObject());
            QCOMPARE(render(loaded), expected);
            const auto target = QSize(48, 40);
            const auto mapped = mappedFilterLayer(layer.metadata["adjustment"].toObject(), input.size, target);
            QCOMPARE(mapped["rgbSplitDistance"].toDouble(), 1.625);
            QCOMPARE(mapped["progressiveEndY"].toDouble(), 1.);
            manifest(path, [](QJsonObject &j) { j["version"] = 15; });
            QVERIFY(rejects("invalidProject", [&] { readProject(path); }));
        }
    }
    void legacyMasksAndUpstreamLineVersionsAreDistinct() {
        auto state = document(16);
        state.layers[0].mask = QImage(state.size, QImage::Format_Grayscale16);
        state.layers[0].mask.fill(QColor::fromRgba64(12345, 12345, 12345, 65535));
        state.layers[0].maskPlacement = state.layers[0].placement;
        state.layers[0].metadata["shape"] = QJsonObject{{"kind", "Line"}, {"lineStyle", "Dotted"}, {"lineCap", "Square"}};
        QTemporaryDir dir; const auto path = dir.path() + "/line.comp";
        writeProject(path, state);
        QCOMPARE(readProject(path).state.metadata["version"].toInt(), 17);
        manifest(path, [](QJsonObject &j) { j["version"] = 15; });
        QCOMPARE(readProject(path).state.layers[0].mask, state.layers[0].mask);
        writeProject(path, state, {}, true);
        const auto compatible = readProject(path).state;
        QCOMPARE(compatible.metadata["version"].toInt(), 15);
        QCOMPARE(compatible.layers[0].mask, state.layers[0].mask);
        QFile png(path + "/images/" + state.layers[0].id + ".mask.png"); QVERIFY(png.open(QIODevice::ReadOnly));
        QCOMPARE(quint8(png.readAll()[24]), quint8(8));
        manifest(path, [](QJsonObject &j) { j["version"] = 14; });
        QVERIFY(rejects("invalidProject", [&] { readProject(path); }));
    }
    void spectralSplitRetainsPrecisionAndHdr() {
        for (int depth : {8, 16}) {
            auto source = pixels(depth);
            QCOMPARE(rgbSplit(source, {{"rgbSplitDistance", 0}, {"rgbSplitBlur", 0}}), source);
            const auto split = rgbSplit(source, {{"rgbSplitDistance", 5}, {"rgbSplitBlur", 2}});
            QCOMPARE(split.size(), source.size()); QCOMPARE(wideImage(split), depth == 16);
            QVERIFY(split.pixelColor(30, 30) != source.pixelColor(30, 30));
            QCOMPARE(split.pixelColor(30, 30).alpha(), 255);
            const auto radial = rgbSplit(source, {{"rgbSplitRadial", true}, {"rgbSplitReach", 20}});
            QVERIFY(radial != split);
            if (depth == 16) QVERIFY(pixel64(split, 30, 30).red() % 257 != 0);
        }
        auto hdr = hdrDocument().layers[0].image;
        auto split = rgbSplit(hdr, {{"rgbSplitDistance", 4}});
        QVERIFY(hdrImage(split)); QVERIFY(reinterpret_cast<const FloatPixel *>(split.constScanLine(20))[20].r > 4);
    }
    void packedPngKeepsRgbWhenAlphaIsZero() {
        auto state = document(8); state.layers[0].name = " Red "; state.layers[0].visible = false;
        state.layers[0].image.fill(Qt::white);
        QCOMPARE(defaultChannelSource(state, 0), state.layers[0].id);
        QCOMPARE(defaultChannelSource(state, 3), QString("@white"));
        const auto packed = renderChannelPack(state, QSize(12, 8), {state.layers[0].id, "@white", "@black", "@black"});
        const auto encoded = pngBytes(packed);
        auto decoded = QImage::fromData(encoded, "png").convertToFormat(QImage::Format_RGBA8888);
        QVERIFY(!decoded.isNull());
        const auto *pixel = decoded.constScanLine(3) + 4 * 4;
        QCOMPARE(pixel[0], uchar(255)); QCOMPARE(pixel[1], uchar(255)); QCOMPARE(pixel[2], uchar(0)); QCOMPARE(pixel[3], uchar(0));
    }
    void splitChannelsKeepsWideSamplesAndUndo() {
        auto state = document(16);
        Document doc; doc.reset(state); doc.splitChannels();
        QCOMPARE(doc.state().layers.size(), 5); QVERIFY(doc.state().layers[0].group);
        const auto &red = doc.state().layers.last(); QCOMPARE(red.name, QString("Red"));
        QCOMPARE(pixel64(red.image, 10, 10).red(), pixel64(state.layers[0].image, 10, 10).red());
        QCOMPARE(pixel64(red.image, 10, 10).green(), pixel64(red.image, 10, 10).red());
        doc.undo(); QCOMPARE(doc.state().layers.size(), 1); QCOMPARE(doc.state().layers[0].image, state.layers[0].image);
    }
    void splitChannelsRefusesToBreakClipping() {
        auto state = document(16);
        Layer dependent = state.layers[0]; dependent.id = newId(); dependent.maskSource = state.active;
        state.layers.append(dependent);
        Document doc; doc.reset(state); const auto revision = doc.revision();
        QVERIFY(rejects("splitClippingSource", [&] { doc.splitChannels(); }));
        QCOMPARE(doc.revision(), revision); QCOMPARE(doc.state().layers.size(), 2);
        QCOMPARE(doc.state().layers.last().maskSource, state.active);
    }
    void layerSearchPreservesHierarchyAndFolderContents() {
        auto state = document(8);
        Layer group; group.id = newId(); group.name = "Photos"; group.group = true; group.placement.size = state.size;
        state.layers[0].parent = group.id; state.layers[0].name = "Mountain"; state.layers.prepend(group);
        Layer other = state.layers.last(); other.id = newId(); other.parent.clear(); other.name = "Caption";
        other.metadata["text"] = QJsonObject{{"content", "Text"}}; state.layers.append(other);
        QCOMPARE(matchingLayers(state, "MOUN", "all", {}), (QSet<QString>{group.id, state.layers[1].id}));
        QCOMPARE(matchingLayers(state, "photo", "all", {}), (QSet<QString>{group.id, state.layers[1].id}));
        QCOMPARE(matchingLayers(state, "", "text", {}), QSet<QString>{other.id});
        QCOMPARE(matchingLayers(state, "", "selected", {group.id}), (QSet<QString>{group.id, state.layers[1].id}));
    }
    void onePixelLinesAndDotsRemainOpaque() {
        for (const auto style : {"Solid", "Dotted", "Dashed", "Long Dash"}) for (const auto cap : {"Square", "Round"}) {
            QImage image(40, 12, QImage::Format_RGBA64_Premultiplied); image.fill(Qt::transparent);
            QPainter painter(&image); painter.setRenderHint(QPainter::Antialiasing);
            strokeShapeLine(painter, linePixelPoint({3, 5}, 1), linePixelPoint({35, 5}, 1), 1, style, cap, Qt::white);
            painter.end();
            int ink = 0;
            for (int x = 0; x < 40; ++x) { if (image.pixelColor(x, 5).alpha() > 180) ++ink; QCOMPARE(image.pixelColor(x, 3).alpha(), 0); }
            QVERIFY2(ink > 8, style);
            if (QString(style) == "Solid") QVERIFY(ink >= 32);
        }
    }
    void photoshopFileImportsSeparateArtboards() {
        const auto result = readPhotoshop(QFINDTESTDATA("fixtures/artboards.psd"));
        QCOMPARE(result.artboards.size(), 2);
        QCOMPARE(result.artboards[0].first, QString("Phone"));
        QCOMPARE(result.artboards[0].second.size, QSize(80, 60));
        QCOMPARE(result.artboards[1].first, QString("Tablet"));
        QCOMPARE(result.artboards[1].second.size, QSize(60, 50));
        QCOMPARE(result.artboards[1].second.layers[0].placement.origin, QPointF(10, 10));
        QCOMPARE(result.artboards[0].second.layers.size(), 1);
        QCOMPARE(result.artboards[1].second.layers.size(), 1);
    }
    void artboardDocumentsOffsetLayersAndMasks() {
        auto source = document(16); source.size = {240, 100};
        Layer board; board.id = newId(); board.group = true; board.name = "Phone"; board.placement.size = source.size;
        board.metadata["windowsPSDArtboard"] = QJsonArray{120, 10, 220, 90};
        source.layers[0].parent = board.id; source.layers[0].placement.origin = {135, 20};
        source.layers[0].maskPlacement.origin = {130, 18}; source.layers.prepend(board);
        Layer loose; loose.id = newId(); loose.name = "Loose"; loose.placement.size = source.size; source.layers.append(loose);
        const auto boards = photoshopArtboards(source); QCOMPARE(boards.size(), 1);
        QCOMPARE(boards[0].first, QString("Phone")); QCOMPARE(boards[0].second.size, QSize(100, 80));
        QCOMPARE(boards[0].second.layers.size(), 1); const auto layer = boards[0].second.layers[0];
        QCOMPARE(layer.placement.origin, QPointF(15, 10)); QCOMPARE(layer.maskPlacement.origin, QPointF(10, 8));
        QVERIFY(layer.parent.isEmpty()); QVERIFY(wideImage(layer.image));
    }
    void newAdjustmentControlsCreateEditableLayers() {
        MainWindow window; window.document()->reset(document(16));
        for (const auto key : {"progressiveBlur", "bloom", "rgbSplit"}) {
            auto *action = window.findChild<QAction *>(QString("adjustment.") + key + "Action"); QVERIFY(action);
            bool opened = false;
            QTimer::singleShot(0, &window, [&] {
                auto *dialog = window.findChild<QDialog *>("filterDialog");
                if (dialog) { opened = true; dialog->accept(); }
            });
            action->trigger(); QVERIFY(opened);
            const auto a = window.document()->state().layers.last().metadata["adjustment"].toObject();
            QVERIFY(a["filterLayerSettings"].isObject()); validateAdjustment(a);
        }
        auto *search = window.findChild<QLineEdit *>("layerSearch"); QVERIFY(search);
        search->setText("nonexistent-layer");
        auto *tree = window.findChild<QTreeWidget *>("layerList"); QVERIFY(tree);
        for (int i = 0; i < tree->topLevelItemCount(); ++i) QVERIFY(tree->topLevelItem(i)->isHidden());
        search->clear();
        for (int i = 0; i < tree->topLevelItemCount(); ++i) QVERIFY(!tree->topLevelItem(i)->isHidden());
        window.canvas()->resize(1200, 900); window.canvas()->fit(false); QVERIFY(window.canvas()->zoom() <= 1.);
    }
    void upstreamPngProjectsInferDepthAndPreserveOriginalBytes() {
        for (int depth : {8, 16})
            for (int version : {12, 13, 14}) {
                QTemporaryDir dir;
                QVERIFY(dir.isValid());
                const auto input = document(depth);
                const auto path = dir.path() + "/input.comp";
                writeProject(path, input);
                manifest(path, [=](QJsonObject &j) {
                    j["version"] = version;
                    j.remove("bitDepth");
                });
                const auto loaded = readProject(path);
                QCOMPARE(loaded.state.bitDepth, depth);
                QCOMPARE(loaded.state.layers[0].image,
                         input.layers[0].image.convertToFormat(pixelFormat(depth)));
                QVERIFY(!loaded.state.layers[0].originalPng.isEmpty());
                const auto copy = dir.path() + "/copy.comp";
                writeProject(copy, loaded.state);
                const auto reopened = readProject(copy);
                QCOMPARE(reopened.state.layers[0].originalPng, loaded.state.layers[0].originalPng);
                QCOMPARE(reopened.state.layers[0].image, loaded.state.layers[0].image);
            }
    }
    void missingHdrAssetsAndFutureProjectsAreRejectedWhileSmoothingLoads() {
        QTemporaryDir dir;
        const auto path = dir.path() + "/file.comp";
        const auto input = document(16);
        writeProject(path, input);
        manifest(path, [](QJsonObject &j) {
            j["version"] = 13;
            auto layers = j["layers"].toArray();
            auto l = layers[0].toObject();
            l["imageFile"] = l["id"].toString() + ".exr";
            layers[0] = l;
            j["layers"] = layers;
        });
        QVERIFY(rejects("missingAsset", [&] { readProject(path); }));
        const auto other = dir.path() + "/shape.comp";
        writeProject(other, input);
        manifest(other, [](QJsonObject &j) {
            j["version"] = 14;
            auto layers = j["layers"].toArray();
            auto l = layers[0].toObject();
            l["shape"] = QJsonObject{{"kind", "Rectangle"}, {"smoothing", true}};
            layers[0] = l;
            j["layers"] = layers;
        });
        QVERIFY(readProject(other).state.layers[0].metadata["shape"].toObject()["smoothing"].toBool());
        manifest(other, [](QJsonObject &j) { j["version"] = ProjectManifest::readable + 1; });
        QVERIFY(rejects("unsupportedVersion", [&] { readProject(other); }));
    }
    void patchMatchPreservesOutsideAndSixteenBitSamples() {
        for (int depth : {8, 16}) {
            QImage image = pixels(depth);
            const QRect hole(40, 30, 12, 10);
            for (int y = hole.top(); y <= hole.bottom(); ++y)
                for (int x = hole.left(); x <= hole.right(); ++x)
                    image.setPixelColor(x, y, Qt::red);
            const auto before = image;
            QImage mask(image.size(), QImage::Format_Grayscale8);
            mask.fill(Qt::black);
            for (int y = hole.top(); y <= hole.bottom(); ++y)
                for (int x = hole.left(); x <= hole.right(); ++x)
                    mask.scanLine(y)[x] = 255;
            QCOMPARE(patch(image, mask, depth), 1);
            QVERIFY(image != before);
            bool nativePrecision = false;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x) {
                    if (!hole.contains(x, y))
                        QCOMPARE(image.pixelColor(x, y), before.pixelColor(x, y));
                    else if (depth == 16)
                        nativePrecision |= image.pixelColor(x, y).rgba64().red() % 257 != 0;
                }
            if (depth == 16)
                QVERIFY(nativePrecision);
            auto repeat = before;
            QCOMPARE(patch(repeat, mask, depth), 1);
            QCOMPARE(repeat, image);
            mask.fill(Qt::white);
            repeat = before;
            QCOMPARE(patch(repeat, mask, depth), 0);
            QCOMPARE(repeat, before);
        }
    }
    void contentAwareHealingUsesPatchMatchWithCoverageAndOpacity() {
        for (int depth : {8, 16}) {
            const auto source = pixels(depth);
            auto filled = source, actual = source;
            QImage mask(source.size(), QImage::Format_Grayscale8);
            mask.fill(Qt::black);
            for (int y = 30; y < 40; ++y)
                for (int x = 40; x < 52; ++x)
                    mask.scanLine(y)[x] = 128;
            QCOMPARE(patch(filled, mask, depth), 1);
            const float opacity = .65f;
            const int status = depth == 16
                                   ? wide::spot_heal(reinterpret_cast<quint16 *>(actual.bits()),
                                                     mask.constBits(), source.width(), source.height(),
                                                     actual.bytesPerLine() / 2, opacity, 0, 123)
                                   : spot_heal(actual.bits(), mask.constBits(), source.width(),
                                               source.height(), actual.bytesPerLine(), opacity, 0, 123);
            QCOMPARE(status, 0);
            for (int y = 0; y < actual.height(); ++y)
                for (int x = 0; x < actual.width(); ++x) {
                    if (!mask.constScanLine(y)[x]) {
                        QCOMPARE(actual.pixelColor(x, y), source.pixelColor(x, y));
                        continue;
                    }
                    const int maximum = depth == 16 ? 65535 : 255;
                    const double amount = mask.constScanLine(y)[x] / 255.0 * opacity;
                    const auto a = source.pixelColor(x, y), b = filled.pixelColor(x, y),
                               c = actual.pixelColor(x, y);
                    QCOMPARE(qRound(c.redF() * maximum),
                             qRound((a.redF() + (b.redF() - a.redF()) * amount) * maximum));
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
            for (int y = 0; y < source.height(); ++y)
                for (int x = 0; x < source.width(); ++x) {
                    QCOMPARE(expected.pixelColor(x, y).alpha(), source.pixelColor(x, y).alpha());
                    if (depth == 16)
                        nativePrecision |= expected.pixelColor(x, y).rgba64().red() % 257 != 0;
                }
            if (depth == 16)
                QVERIFY(nativePrecision);
            QVERIFY(cameraRaw(source, settings, .05) != expected);
            auto disabled = settings;
            disabled["enableLight"] = false;
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
                for (auto &v : grid) {
                    seed = seed * 6364136223846793005ull + 1442695040888963407ull;
                    v = double(seed >> 11) / 9007199254740992.;
                }
                for (int y = 0; y < height; ++y)
                    for (int x = 0; x < width; ++x) {
                        const double fx = double(x) / cell, fy = double(y) / cell;
                        const int x0 = int(fx), y0 = int(fy);
                        const double tx = fx - x0, ty = fy - y0;
                        const double a = grid[y0 * gw + x0] * (1 - tx) + grid[y0 * gw + x0 + 1] * tx;
                        const double b =
                            grid[(y0 + 1) * gw + x0] * (1 - tx) + grid[(y0 + 1) * gw + x0 + 1] * tx;
                        out[y * width + x] += amplitude * (a * (1 - ty) + b * ty);
                    }
                total += amplitude;
                amplitude *= .6;
            }
            for (auto &v : out)
                v /= total;
            return out;
        };
        for (int width : {600, 1200}) {
            const int height = width * 2 / 3;
            const auto clouds = field(width, height, 1), lay = field(width, height, 2);
            QImage image(width, height, QImage::Format_RGBA8888_Premultiplied);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) {
                    const int i = y * width + x;
                    const double horizon = height * (.5 + .25 * (lay[x] - .5));
                    const double sky = 140 + 80 * (1 - y / horizon) + 60 * (clouds[i] - .5);
                    const double ground = 4 + 70 * std::pow(clouds[i], 2.2) + 20 * (lay[i] - .5);
                    const uchar v = uchar(std::clamp(qRound(y < horizon ? sky : ground), 0, 255));
                    auto *p = image.scanLine(y) + x * 4;
                    p[0] = p[1] = p[2] = v;
                    p[3] = 255;
                }
            for (int amount : {100, 50, -100}) {
                const auto out = cameraRaw(image, {{"shadows", amount}})
                                     .convertToFormat(QImage::Format_RGBA8888_Premultiplied);
                auto mean = [&](int first, int last) {
                    double sum = 0;
                    for (int y = first; y < last; ++y)
                        for (int x = 0; x < width; ++x)
                            sum += out.constScanLine(y)[x * 4 + 1];
                    return sum / ((last - first) * width);
                };
                const double ground = mean(height * 4 / 5, height), sky = mean(0, height / 5);
                const double expectedGround = width == 1200 ? (amount == 100  ? 59.0
                                                               : amount == 50 ? 34.9
                                                                              : 4.2)
                                                            : (amount == 100  ? 51.0
                                                               : amount == 50 ? 32.2
                                                                              : 5.5);
                const double expectedSky = width == 1200 ? (amount == 100  ? 205.9
                                                            : amount == 50 ? 205.3
                                                                           : 203.6)
                                                         : (amount == 100  ? 205.8
                                                            : amount == 50 ? 205.2
                                                                           : 203.6);
                qInfo() << "UPSTREAM_SHADOWS" << width << amount << ground << sky;
                QVERIFY(std::abs(ground - expectedGround) <= (width == 600 && amount == 100 ? 10 : 4));
                QVERIFY(std::abs(sky - expectedSky) <= 2);
            }
        }
    }
    void sectionResetLeavesOtherSectionsAndRestoresVisibility() {
        QJsonObject original{{"exposure", 2},
                             {"shadows", 70},
                             {"temperature", 31},
                             {"enableLight", false},
                             {"detail", QJsonObject{{"noiseLuminance", 45}}},
                             {"mixer", QJsonObject{{"points", QJsonArray{QJsonObject{{"hue", 30}}}}}}};
        const auto reset = resetCameraRawSection(original, "light");
        QVERIFY(!reset.contains("exposure"));
        QVERIFY(!reset.contains("shadows"));
        QVERIFY(reset["enableLight"].toBool());
        QCOMPARE(reset["temperature"], original["temperature"]);
        QCOMPARE(reset["detail"], original["detail"]);
        QCOMPARE(reset["mixer"], original["mixer"]);
        QCOMPARE(resetCameraRawSection(original, "unknown"), original);
        QVERIFY(!resetCameraRawSection(original, "pointColor")["mixer"].toObject().contains("points"));
    }
    void cameraRawSectionResetIsWiredToTheDialog() {
        MainWindow window;
        const auto before = document(16);
        window.document()->reset(before, true);
        bool checked = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            auto *exposure = dialog->findChild<QDoubleSpinBox *>("raw_exposure");
            auto *temperature = dialog->findChild<QDoubleSpinBox *>("raw_temperature");
            auto *reset = dialog->findChild<QPushButton *>("rawReset_light");
            if (exposure && temperature && reset) {
                exposure->setValue(2);
                temperature->setValue(31);
                reset->click();
                checked = exposure->value() == 0 && temperature->value() == 31;
            }
            dialog->reject();
        });
        auto *action = window.findChild<QAction *>("filter.cameraRawAction");
        QVERIFY(action);
        action->trigger();
        QVERIFY(checked);
        QCOMPARE(window.document()->state().layers[0].image, before.layers[0].image);
    }
    void imageFolderImportIsSortedNonrecursiveAndSkipsProjects() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        root.mkdir("nested");
        root.mkdir("project.comp");
        for (const auto &name : {"B.PNG", "a.hif", "C.dng", "ignored.txt", "nested/inside.png"}) {
            QFile f(root.filePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("fixture");
        }
        const auto found = imageFilesInFolder(dir.path());
        QCOMPARE(found.size(), 3);
        QCOMPARE(QFileInfo(found[0]).fileName(), QString("a.hif"));
        QCOMPARE(QFileInfo(found[1]).fileName(), QString("B.PNG"));
        QVERIFY(imageFilesInFolder(root.filePath("project.comp")).isEmpty());
    }
    void smoothCornersMatchGeometryAndPersist() {
        const auto ordinary = roundedShapePath(QRectF(0, 0, 160, 100), 20, false),
                   smooth = roundedShapePath(QRectF(0, 0, 160, 100), 20, true);
        QVERIFY(ordinary != smooth);
        QCOMPARE(smooth.boundingRect(), QRectF(0, 0, 160, 100));
        QVERIFY(smooth.contains(QPointF(80, 50)));
        QVERIFY(!smooth.contains(QPointF(0, 0)));
        QCOMPARE(roundedShapePath(QRectF(0, 0, 160, 100), 0, true),
                 roundedShapePath(QRectF(0, 0, 160, 100), 0, false));
        auto s = document(16);
        s.layers[0].metadata["shape"] =
            QJsonObject{{"kind", "Rectangle"}, {"cornerRadius", 20}, {"smoothing", true}};
        QTemporaryDir dir;
        writeProject(dir.path() + "/smooth.comp", s);
        QVERIFY(readProject(dir.path() + "/smooth.comp")
                    .state.layers[0]
                    .metadata["shape"]
                    .toObject()["smoothing"]
                    .toBool());
    }
    void blurPreservesExactAlphaWithoutDarkFringe() {
        for (int depth : {8, 16}) {
            QImage image(64, 48, pixelFormat(depth));
            image.fill(Qt::transparent);
            for (int y = 10; y < 38; ++y)
                for (int x = 12; x < 52; ++x)
                    image.setPixelColor(x, y, QColor::fromRgba64(31007, 49001, 59003, 32767));
            auto out = filtered(
                image, {{"kind", "Gaussian Blur"}, {"blurRadius", 6}, {"preserveTransparency", true}});
            QCOMPARE(out.format(), image.format());
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    QCOMPARE(out.pixelColor(x, y).alpha(), image.pixelColor(x, y).alpha());
            QVERIFY(std::abs(out.pixelColor(12, 12).blueF() - image.pixelColor(12, 12).blueF()) < .01);
        }
    }
    void blurBoundsKeepPlacementAndCanvasEdges() {
        auto s = document(16);
        auto l = s.layers[0];
        l.placement.rotation = 31;
        l.placement.flipX = true;
        l.placement.origin = {80, 50};
        const auto before = l.placement.matrix(l.image.size());
        expandLayerPixels(l, QRect(-10, -8, 130, 110));
        const auto after = l.placement.matrix(l.image.size());
        QVERIFY(QLineF(before.map(QPointF(23, 17)), after.map(QPointF(33, 25))).length() < 1e-7);
        auto full = s.layers[0];
        prepareBlurBounds(full, s.size, {{"kind", "Gaussian Blur"}, {"blurRadius", 5}});
        QCOMPARE(full.image.size(), s.size);
        auto small = s.layers[0];
        small.placement.origin = {10, 10};
        prepareBlurBounds(small, QSize(240, 200), {{"kind", "Gaussian Blur"}, {"blurRadius", 5}});
        QVERIFY(small.image.width() > s.size.width());
    }
    void progressiveBlurEndpointsAndRadialPreserveDepth() {
        for (int depth : {8, 16}) {
            auto image = pixels(depth);
            QCOMPARE(progressiveBlur(image, {{"progressiveFrom", 0}, {"progressiveTo", 0}}), image);
            auto out = progressiveBlur(
                image, {{"progressiveFrom", 0}, {"progressiveTo", 15}, {"progressiveRadial", true}});
            QCOMPARE(out.format(), image.format());
            QCOMPARE(out.size(), image.size());
            QVERIFY(out != image);
        }
        auto s = hdrDocument();
        auto out = progressiveBlur(s.layers[0].image, {{"progressiveFrom", 2}, {"progressiveTo", 10}});
        QVERIFY(hdrImage(out));
        QVERIFY(reinterpret_cast<const FloatPixel *>(out.constScanLine(20))[30].r > 4);
    }
    void perLayerDepthDoesNotChangeOtherLayersAndSurvivesUndo() {
        auto s = document(16);
        auto copy = s.layers[0];
        copy.id = newId();
        copy.name = "Second";
        s.layers.append(copy);
        Document doc;
        doc.reset(s);
        doc.setLayerBitDepth(8);
        QVERIFY(!wideImage(doc.state().layers[0].image));
        QCOMPARE(doc.state().layers[1].image, s.layers[1].image);
        QCOMPARE(doc.state().bitDepth, 16);
        doc.applyFilter({{"kind", "Invert"}});
        QVERIFY(!wideImage(doc.state().layers[0].image));
        doc.undo();
        doc.undo();
        QCOMPARE(doc.state().layers[0].image, s.layers[0].image);
        doc.setLayerBitDepth(8);
        QTemporaryDir dir;
        writeProject(dir.path() + "/mixed.comp", doc.state());
        auto loaded = readProject(dir.path() + "/mixed.comp").state;
        QVERIFY(!wideImage(loaded.layers[0].image));
        QVERIFY(wideImage(loaded.layers[1].image));
    }
    void contentFillExtendsLayerAndUndoRestoresOriginal() {
        auto s = document(16);
        s.size = QSize(140, 100);
        Document doc;
        doc.reset(s);
        QPainterPath selection;
        selection.addRect(90, 20, 18, 35);
        doc.setSelection(selection);
        doc.contentFill();
        QVERIFY(doc.state().layers[0].image.width() >= 108);
        QVERIFY(doc.state().layers[0].image.pixelColor(103, 30).alpha() > 0);
        QCOMPARE(doc.state().layers[0].image.pixelColor(4, 4), s.layers[0].image.pixelColor(4, 4));
        doc.undo();
        QCOMPARE(doc.state().layers[0].image, s.layers[0].image);
    }
    void hdrExrProjectAndClipboardRoundTripKeepHighlights() {
        const auto s = hdrDocument();
        const auto bytes = exrBytes(s.layers[0].image);
        QVERIFY(bytes.size() > 64);
        auto decoded = readExr(bytes);
        QVERIFY(hdrImage(decoded));
        QVERIFY(std::abs(reinterpret_cast<const FloatPixel *>(decoded.constScanLine(10))[10].r - 4.135f) <
                .002f);
        QTemporaryDir dir;
        const auto path = dir.path() + "/hdr.comp";
        writeProject(path, s);
        auto loaded = readProject(path);
        QVERIFY(hasHdr(loaded.state));
        writeProject(dir.path() + "/copy.comp", loaded.state);
        QCOMPARE(readProject(dir.path() + "/copy.comp").state.layers[0].originalPng,
                 loaded.state.layers[0].originalPng);
        Document target;
        target.reset(document(8));
        QVERIFY(pasteLayers(target, encodeLayers(s, {s.active})));
        QVERIFY(hasHdr(target.state()));
    }
    void hdrRenderingExposureAndRetouchDoNotClipAtWhite() {
        const auto s = hdrDocument();
        auto rendered = render(s);
        QVERIFY(hdrImage(rendered));
        QVERIFY(reinterpret_cast<const FloatPixel *>(rendered.constScanLine(10))[10].r > 4);
        auto state = s;
        Layer adjustment;
        adjustment.id = newId();
        adjustment.name = "Exposure";
        adjustment.placement.size = s.size;
        adjustment.metadata["adjustment"] =
            QJsonObject{{"kind", "Exposure"}, {"exposureSettings", QJsonObject{{"exposure", -3}}}};
        state.layers.append(adjustment);
        auto exposed = render(state);
        const auto value = reinterpret_cast<const FloatPixel *>(exposed.constScanLine(10))[10].r;
        QVERIFY(value > .50f && value < .53f);
        auto image = s.layers[0].image;
        RetouchStroke stroke(image, 12, .5, .7, false);
        stroke.append(image, {{30, 30}, {40, 35}});
        QVERIFY(hdrImage(image));
        QVERIFY(reinterpret_cast<const FloatPixel *>(image.constScanLine(34))[38].r > 4);
        auto blur = filtered(image, {{"kind", "Gaussian Blur"}, {"blurRadius", 6}});
        QVERIFY(hdrImage(blur));
        QVERIFY(reinterpret_cast<const FloatPixel *>(blur.constScanLine(34))[38].r > 4);
    }
    void radianceImportAndMalformedExrFailSafely() {
        QByteArray file = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n";
        file += QByteArray::fromHex("8040208380402083");
        auto image = readRadiance(file);
        QVERIFY(hdrImage(image));
        QCOMPARE(image.size(), QSize(2, 1));
        QVERIFY(reinterpret_cast<const FloatPixel *>(image.constScanLine(0))[0].r > 1);
        QVERIFY_EXCEPTION_THROWN(readExr(QByteArray("bad")), Error);
        QVERIFY_EXCEPTION_THROWN(readRadiance(file.left(file.size() - 1)), Error);
    }
    void cmykProfileConversionProofAndTaggedExports() {
        const auto profile = syntheticCmyk();
        validateCmykProfile(profile);
        QVERIFY(cmykProfileName(profile).contains("synthetic"));
        QImage image(24, 18, QImage::Format_RGB32);
        image.fill(QColor(91, 123, 167));
        const auto inks = rgbToCmyk(image, profile);
        QCOMPARE(inks.format(), QImage::Format_CMYK8888);
        const auto rgb = cmykToRgb(inks, profile);
        QVERIFY(rgb.pixelColor(0, 0).alpha() == 255);
        QVERIFY(cmykProof(image, profile).size() == image.size());
        QVERIFY(cmykProof(image, profile, true).size() == image.size());
        QTemporaryDir dir;
        for (const auto &format : {QByteArray("jpeg"), QByteArray("tiff"), QByteArray("pdf")}) {
            const auto data = encodeCmyk(image, profile, format, 300, 95);
            QVERIFY(data.size() > 100);
            QVERIFY(data.contains(format == "pdf" ? "/N 4" : format == "jpeg" ? "ICC_PROFILE" : "acsp"));
            QFile f(dir.path() + "/test." + format);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(data);
            f.close();
            if (format != "pdf") {
                QImageReader reader(f.fileName());
                QCOMPARE(reader.size(), image.size());
                QVERIFY(!reader.read().isNull());
            }
        }
        QVERIFY_EXCEPTION_THROWN(validateCmykProfile(QByteArray("invalid")), Error);
    }
    void tiffAndHdrExportAreReadable() {
        QTemporaryDir dir;
        auto s = document(16);
        exportDocument(s, dir.path() + "/16.tiff", "tiff", s.size);
        QImageReader tiff(dir.path() + "/16.tiff");
        auto image = tiff.read();
        QVERIFY(!image.isNull());
        QVERIFY(wideImage(image));
        s = hdrDocument();
        exportDocument(s, dir.path() + "/hdr.exr", "exr", s.size);
        QFile exr(dir.path() + "/hdr.exr");
        QVERIFY(exr.open(QIODevice::ReadOnly));
        auto hdr = readExr(exr.readAll());
        QVERIFY(reinterpret_cast<const FloatPixel *>(hdr.constScanLine(10))[10].r > 4);
    }
    void rawLensIdentityAndUnsupportedProfiles() {
        const QByteArray xml = "<lensdatabase><lens><calibration><distortion model=\"poly3\" focal=\"50\" "
                               "k1=\"0\"/></calibration></lens></lensdatabase>";
        const auto image = pixels(16).convertToFormat(QImage::Format_RGBA64);
        QCOMPARE(correctRawLens(image, xml, 50), image);
        QVERIFY_EXCEPTION_THROWN(correctRawLens(image, "<bad>", 50), Error);
        auto changed = correctRawLens(image, QByteArray(xml).replace("k1=\"0\"", "k1=\"0.12\""), 50);
        QCOMPARE(changed.size(), image.size());
        QVERIFY(changed != image);
    }
    void newFeatureActionsAndCancelledProgressiveDialog() {
        MainWindow window;
        window.localization()->setLanguage("en");
        const auto s = document(16);
        window.document()->reset(s);
        QCoreApplication::processEvents();
        for (auto key : {"filter.progressiveBlurAction", "layerDepth8Action", "layerDepth16Action",
                         "loadCmykProfileAction", "cmykProofAction", "cmykGamutAction"})
            QVERIFY2(window.findChild<QAction *>(key), key);
        bool opened = false;
        QTimer::singleShot(60, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (dialog) {
                opened = dialog->objectName() == "progressiveBlurDialog";
                const auto capture = qEnvironmentVariable("COMPOSITOR_CAPTURE_TEST_UI");
                if (!capture.isEmpty())
                    dialog->grab().save(capture);
                dialog->reject();
            }
        });
        window.findChild<QAction *>("filter.progressiveBlurAction")->trigger();
        QVERIFY(opened);
        QCOMPARE(window.document()->state().layers[0].image, s.layers[0].image);
        QVERIFY(!window.document()->canUndo());
    }

    void cmykPsdCompositeAndLayersUseEmbeddedProfile() {
        const auto profile = syntheticCmyk();
        QTemporaryDir directory;
        for (bool layered : {false, true}) {
            QByteArray resources;
            QDataStream resource(&resources, QIODevice::WriteOnly);
            resource.setByteOrder(QDataStream::BigEndian);
            resource.writeRawData("8BIM", 4);
            resource << quint16(1039) << quint16(0) << quint32(profile.size());
            resource.writeRawData(profile.constData(), profile.size());
            if (profile.size() % 2)
                resource << quint8(0);
            QByteArray sections;
            if (layered) {
                QByteArray extras;
                QDataStream ex(&extras, QIODevice::WriteOnly);
                ex.setByteOrder(QDataStream::BigEndian);
                ex << quint32(0) << quint32(0);
                ex.writeRawData("\x04"
                                "Test\0\0\0",
                                8);
                QByteArray info;
                QDataStream layer(&info, QIODevice::WriteOnly);
                layer.setByteOrder(QDataStream::BigEndian);
                layer << qint16(1) << qint32(0) << qint32(0) << qint32(3) << qint32(4) << quint16(5);
                for (int id : {0, 1, 2, 3, -1})
                    layer << qint16(id) << quint32(14);
                layer.writeRawData("8BIMnorm", 8);
                layer << quint8(255) << quint8(0) << quint8(0) << quint8(0) << quint32(extras.size());
                layer.writeRawData(extras.constData(), extras.size());
                for (int value : {205, 155, 105, 55, 90}) {
                    layer << quint16(0);
                    layer.writeRawData(QByteArray(12, char(value)).constData(), 12);
                }
                QDataStream section(&sections, QIODevice::WriteOnly);
                section.setByteOrder(QDataStream::BigEndian);
                section << quint32(info.size());
                section.writeRawData(info.constData(), info.size());
                section << quint32(0);
            }
            QByteArray bytes;
            QDataStream file(&bytes, QIODevice::WriteOnly);
            file.setByteOrder(QDataStream::BigEndian);
            file.writeRawData("8BPS", 4);
            file << quint16(1);
            file.writeRawData(QByteArray(6, 0).constData(), 6);
            file << quint16(5) << quint32(3) << quint32(4) << quint16(8) << quint16(4) << quint32(0)
                 << quint32(resources.size());
            file.writeRawData(resources.constData(), resources.size());
            file << quint32(sections.size());
            file.writeRawData(sections.constData(), sections.size());
            file << quint16(0);
            for (int value : {205, 155, 105, 55, 90})
                file.writeRawData(QByteArray(12, char(value)).constData(), 12);
            const QString path = directory.path() + (layered ? "/layers.psd" : "/composite.psd");
            QFile output(path);
            QVERIFY(output.open(QIODevice::WriteOnly));
            output.write(bytes);
            output.close();
            const auto imported = readPhotoshop(path);
            QCOMPARE(imported.state.layers.size(), 1);
            const auto &image = imported.state.layers[0].image;
            QCOMPARE(image.size(), QSize(4, 3));
            QCOMPARE(image.pixelColor(0, 0).alpha(), 90);
            QCOMPARE(
                QByteArray::fromBase64(imported.state.metadata["windowsCMYKProfile"].toString().toLatin1()),
                profile);
            QImage inks(1, 1, QImage::Format_CMYK8888);
            for (int c = 0; c < 4; ++c)
                inks.bits()[c] = quint8((c + 1) * 50);
            const auto expected = cmykToRgb(inks, profile).pixelColor(0, 0);
            QVERIFY(std::abs(image.pixelColor(0, 0).red() - expected.red()) < 3);
            QVERIFY(std::abs(image.pixelColor(0, 0).blue() - expected.blue()) < 3);
        }
    }
    void expansionRetainsLinkedMaskRegistration() {
        auto layer=document(16).layers[0];layer.mask=QImage(layer.image.size(),QImage::Format_Grayscale16);layer.mask.fill(Qt::white);
        layer.mask.setPixelColor(4,5,Qt::black);expandLayerPixels(layer,QRect(-7,-9,120,110));
        QVERIFY(layer.maskLinked);QCOMPARE(layer.mask.size(),layer.image.size());QCOMPARE(layer.mask.pixelColor(11,14).red(),0);
        QCOMPARE(layer.mask.pixelColor(0,0).red(),255);
    }
    void hdrCameraRawClippingUsesReferenceParameters() {
        const auto source = hdrDocument().layers[0].image;
        QJsonObject settings{{"kind", "Camera Raw"}, {"exposure", .3}};
        QCOMPARE(filteredAccelerated(source, settings, 1, 1), cameraRaw(source, settings, 1, 1));
    }
    void hdrSelectionsAndMotionKeepHighlightsAndOutsidePixels() {
        auto s = hdrDocument();
        QImage changed = filtered(
            s.layers[0].image, {{"kind", "Exposure"}, {"exposureSettings", QJsonObject{{"exposure", -2}}}});
        s.selectionActive = true;
        s.selection.addRect(20, 20, 20, 20);
        s.selectionMask = QImage(s.size, QImage::Format_Grayscale16);
        s.selectionMask.fill(Qt::black);
        {
            QPainter painter(&s.selectionMask);
            painter.fillRect(QRect(20, 20, 20, 20), Qt::white);
        }
        constrainSelection(s, s.layers[0].placement, s.layers[0].image, changed);
        QVERIFY(hdrImage(changed));
        QCOMPARE(reinterpret_cast<const FloatPixel *>(changed.constScanLine(2))[2].r,
                 reinterpret_cast<const FloatPixel *>(s.layers[0].image.constScanLine(2))[2].r);
        QVERIFY(reinterpret_cast<const FloatPixel *>(changed.constScanLine(25))[25].r < 2);
        auto motion = motionBlurClamped(
            s.layers[0].image, {{"kind", "Motion Blur"}, {"motionDistance", 12}, {"motionAngle", 35}});
        QVERIFY(hdrImage(motion));
        QVERIFY(reinterpret_cast<const FloatPixel *>(motion.constScanLine(25))[25].r > 4);
        QImage opaque(45, 35, QImage::Format_RGBA64_Premultiplied);
        opaque.fill(Qt::white);
        auto edge =
            motionBlurClamped(opaque, {{"kind", "Motion Blur"}, {"motionDistance", 15}, {"motionAngle", 35}});
        QVERIFY(edge.pixelColor(0, 0).alphaF() > .98);
        QVERIFY(edge.pixelColor(44, 34).alphaF() > .98);
    }
    void rawDecodeControlsKeepSixteenBitData() {
        const QString path = QFINDTESTDATA("fixtures/linear.dng");
        QVERIFY(!path.isEmpty());
        auto first = decodeRaw(path);
        QVERIFY(!first.encoded.isEmpty());
        auto decoded = decodeRawBytes(first.encoded, 35, true);
        QCOMPARE(decoded.linear.size(), first.linear.size());
        QVERIFY(wideImage(decoded.linear));
        QVERIFY(wideImage(developRaw(decoded)));
    }
};
QTEST_MAIN(GpuUpstreamTests)
#include "GpuUpstreamTests.moc"
