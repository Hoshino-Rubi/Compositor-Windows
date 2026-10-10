#include "MainWindow.h"
#include "Renderer.h"
#include "Filters.h"
#include "Icons.h"
#include "Effects.h"
#include "Importers.h"
#include "CameraRaw.h"
#include "Selection.h"
#include "Warp.h"
#include "Text.h"
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
} // namespace
class Tests : public QObject {
    Q_OBJECT
  private slots:
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
        rewrite(future, [](QJsonObject &j) { j["version"] = 12; });
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
        for (const auto &language : QStringList{"en", "zh-CN", "zh-TW"}) {
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
        window.openProject(QString(COMPOSITOR_FIXTURES) + "/linear.dng");
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
        bool accepted = false;
        QTimer rejectErrors;
        connect(&rejectErrors, &QTimer::timeout, this, [] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                box->reject();
        });
        rejectErrors.start(300);
        QTimer::singleShot(40, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto *key = dialog->findChild<QKeySequenceEdit *>("transformLayersActionShortcut");
            QVERIFY(key);
            key->setKeySequence(QKeySequence("Ctrl+Alt+T"));
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            accepted = dialog->result() == QDialog::Accepted;
        });
        window.findChild<QAction *>("shortcutsAction")->trigger();
        QVERIFY(accepted);
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
        window.localization()->setLanguage("zh-CN");
        QTimer::singleShot(500, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto *sections = dialog->findChild<QComboBox *>("rawSection");
            QVERIFY(sections);
            QCOMPARE(sections->count(), 11);
            for (auto *label : dialog->findChildren<QLabel *>())
                QVERIFY2(!label->text().startsWith("raw."), qPrintable(label->text()));
            QString capture = qEnvironmentVariable("COMPOSITOR_TEST_CAPTURE_DIR");
            if (!capture.isEmpty()) {
                QDir().mkpath(capture);
                for (int i = 0; i < 11; ++i) {
                    sections->setCurrentIndex(i);
                    QCoreApplication::processEvents();
                    dialog->grab().save(capture + QString("/camera-raw-page-%1.png").arg(i));
                }
            }
            dialog->reject();
        });
        window.findChild<QAction *>("filter.cameraRawAction")->trigger();
        window.document()->markSaved(window.document()->revision());
    }
    void localizationCompleteness() {
        QFile en(":/translations/en.json");
        QVERIFY(en.open(QIODevice::ReadOnly));
        const auto english = QJsonDocument::fromJson(en.readAll()).object();
        for (const auto &lang : QStringList{"zh-CN", "zh-TW"}) {
            QFile f(":/translations/" + lang + ".json");
            QVERIFY(f.open(QIODevice::ReadOnly));
            const auto translated = QJsonDocument::fromJson(f.readAll()).object();
            QCOMPARE(translated.keys(), english.keys());
            for (const auto &key : english.keys())
                QVERIFY2(!translated[key].toString().isEmpty(), qPrintable(key));
        }
        QCOMPARE(Localization::normalize("zh_Hant_HK"), QString("zh-TW"));
        QCOMPARE(Localization::normalize("zh-Hans"), QString("zh-CN"));
        QCOMPARE(Localization::normalize("fr-FR"), QString("en"));
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
    QCoreApplication::setOrganizationName("CompositorTests");
    QCoreApplication::setApplicationName("Tests");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QTemporaryDir settings;
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    Tests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "Tests.moc"
