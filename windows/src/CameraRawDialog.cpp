#include "GpuPreview.h"
#include "MainWindow.h"
#include "CameraRaw.h"
#include "Importers.h"
#include "Selection.h"
#include <QtConcurrent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QTabWidget>
#include <QGroupBox>
#include <QPainter>
#include <QMouseEvent>
#include <QFileDialog>
#include <QJsonDocument>
#include <QSaveFile>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QMenu>
#include <QApplication>
#include <array>
namespace comp {
class CurveEditor final : public QWidget {
  public:
    QJsonArray points{QJsonObject{{"x", 0}, {"y", 0}}, QJsonObject{{"x", 1}, {"y", 1}}};
    std::function<void()> changed;
    int selected = -1;
    explicit CurveEditor(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumSize(230, 175);
        setMouseTracking(true);
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), QColor("#191919"));
        p.setPen(QColor("#444"));
        for (int i = 1; i < 4; ++i) {
            p.drawLine(i * width() / 4, 0, i * width() / 4, height());
            p.drawLine(0, i * height() / 4, width(), i * height() / 4);
        }
        QPainterPath path;
        for (int x = 0; x < width(); ++x) {
            QPointF point(x, (1 - curveValue(double(x) / width(), points)) * height());
            if (!x)
                path.moveTo(point);
            else
                path.lineTo(point);
        }
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor("#95bdff"), 2));
        p.drawPath(path);
        p.setBrush(Qt::white);
        for (const auto &v : points) {
            auto pt = v.toObject();
            p.drawEllipse(QPointF(pt["x"].toDouble() * width(), (1 - pt["y"].toDouble()) * height()), 4, 4);
        }
    }
    void mousePressEvent(QMouseEvent *event) override {
        double x = std::clamp(event->position().x() / width(), 0., 1.),
               y = std::clamp(1 - event->position().y() / height(), 0., 1.);
        selected = -1;
        for (int i = 0; i < points.size(); ++i) {
            auto p = points[i].toObject();
            if (QLineF(event->position(), QPointF(p["x"].toDouble() * width(), (1 - p["y"].toDouble()) * height()))
                    .length() < 10) {
                selected = i;
                break;
            }
        }
        if (event->button() == Qt::RightButton) {
            if (selected > 0 && selected < points.size() - 1) {
                points.removeAt(selected);
                if (changed)
                    changed();
                update();
            }
            return;
        }
        if (selected < 0 && points.size() < 32) {
            int i = 1;
            while (i < points.size() && points[i].toObject()["x"].toDouble() < x)
                ++i;
            selected = std::min(i, int(points.size()) - 1);
            points.insert(selected, QJsonObject{{"x", x}, {"y", y}});
        }
        if (changed)
            changed();
        update();
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        if (!(event->buttons() & Qt::LeftButton) || selected < 0)
            return;
        auto pt = points[selected].toObject();
        double x = event->position().x() / width();
        if (selected == 0)
            x = 0;
        else if (selected == points.size() - 1)
            x = 1;
        else
            x = std::clamp(x, points[selected - 1].toObject()["x"].toDouble() + .005,
                           points[selected + 1].toObject()["x"].toDouble() - .005);
        pt["x"] = x;
        pt["y"] = std::clamp(1 - event->position().y() / height(), 0., 1.);
        points[selected] = pt;
        if (changed)
            changed();
        update();
    }
};
class RawScope final : public QWidget {
  public:
    QImage image;
    bool vectorscope = false;
    QString histogramText, vectorText;
    explicit RawScope(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumHeight(115);
        setMaximumHeight(130);
        setContextMenuPolicy(Qt::CustomContextMenu);
        connect(this, &QWidget::customContextMenuRequested, this, [this](QPoint at) {
            QMenu menu;
            auto *hist = menu.addAction(histogramText), *vector = menu.addAction(vectorText);
            auto *selected = menu.exec(mapToGlobal(at));
            if (selected == hist)
                vectorscope = false;
            if (selected == vector)
                vectorscope = true;
            update();
        });
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), QColor("#191919"));
        if (image.isNull())
            return;
        auto sample = image.scaled(200, 160, Qt::KeepAspectRatio).convertToFormat(QImage::Format_RGB32);
        if (!vectorscope) {
            std::array<std::array<int, 256>, 3> bins{};
            int peak = 1;
            for (int y = 0; y < sample.height(); ++y)
                for (int x = 0; x < sample.width(); ++x) {
                    auto pixel = sample.pixel(x, y);
                    peak = std::max({peak, ++bins[0][qRed(pixel)], ++bins[1][qGreen(pixel)], ++bins[2][qBlue(pixel)]});
                }
            p.setCompositionMode(QPainter::CompositionMode_Plus);
            QColor colors[]{QColor(170, 35, 35, 160), QColor(35, 170, 70, 160), QColor(40, 80, 220, 160)};
            for (int c = 0; c < 3; ++c) {
                QPainterPath path;
                path.moveTo(0, height());
                for (int i = 0; i < 256; ++i)
                    path.lineTo(i * double(width()) / 255, height() - bins[c][i] * double(height() - 5) / peak);
                path.lineTo(width(), height());
                path.closeSubpath();
                p.fillPath(path, colors[c]);
            }
        } else {
            QPointF center(width() / 2., height() / 2.);
            double radius = std::min(width(), height()) * .45;
            p.setPen(QColor("#444"));
            p.drawEllipse(center, radius, radius);
            p.drawLine(center - QPointF(radius, 0), center + QPointF(radius, 0));
            p.drawLine(center - QPointF(0, radius), center + QPointF(0, radius));
            for (int y = 0; y < sample.height(); ++y)
                for (int x = 0; x < sample.width(); ++x) {
                    auto color = sample.pixelColor(x, y);
                    double a = color.hsvHueF() * 6.28318530718, s = color.hsvSaturationF();
                    p.setPen(QColor(color.red(), color.green(), color.blue(), 100));
                    p.drawPoint(center + QPointF(cos(a) * s * radius, -sin(a) * s * radius));
                }
        }
    }
};
class RawPreview final : public QLabel {
  public:
    std::function<void(QPointF)> clicked;
    std::function<void(QPointF, QPointF)> guide;
    bool guideMode = false;
    std::function<void(QPointF)> hovered;
    QPointF start;
    QList<QLineF> guides;
    using QLabel::QLabel;

  protected:
    void paintEvent(QPaintEvent *event) override {
        QLabel::paintEvent(event);
        auto pix = pixmap();
        if (pix.isNull())
            return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor("#70baff"), 2));
        auto size = pix.deviceIndependentSize();
        QPointF offset((width() - size.width()) / 2, (height() - size.height()) / 2);
        auto view = [&](QPointF point) {
            return offset + QPointF(point.x() * size.width(), point.y() * size.height());
        };
        for (const auto &line : guides) {
            p.drawLine(view(line.p1()), view(line.p2()));
            p.drawEllipse(view(line.p1()), 4, 4);
            p.drawEllipse(view(line.p2()), 4, 4);
        }
    }
    QPointF normalized(QPointF pt) const {
        auto pix = pixmap();
        if (pix.isNull())
            return {};
        QSizeF size = pix.deviceIndependentSize();
        QPointF offset((width() - size.width()) / 2, (height() - size.height()) / 2);
        return {(pt.x() - offset.x()) / size.width(), (pt.y() - offset.y()) / size.height()};
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        if (hovered)
            hovered(normalized(event->position()));
    }
    void mousePressEvent(QMouseEvent *event) override {
        start = normalized(event->position());
        if (!guideMode && clicked)
            clicked(start);
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (guideMode && guide)
            guide(start, normalized(event->position()));
    }
};
static void jsonSet(QJsonObject &root, const QString &path, QJsonValue value) {
    int dot = path.indexOf('.');
    if (dot < 0) {
        root[path] = value;
        return;
    }
    QString first = path.left(dot);
    auto child = root[first].toObject();
    jsonSet(child, path.mid(dot + 1), value);
    root[first] = child;
}
static QJsonValue jsonGet(QJsonObject root, const QString &path) {
    auto parts = path.split('.');
    for (int i = 0; i < parts.size() - 1; ++i)
        root = root[parts[i]].toObject();
    return root[parts.last()];
}
void MainWindow::cameraRawDialog() {
    const int index = document_.state().index(document_.state().active);
    if (index < 0 || document_.state().layers[index].image.isNull())
        return;
    const State original = document_.state();
    const QImage full = original.layers[index].image;
    const QImage source = full.scaled(1000, 850, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QJsonObject settings;
    QDialog dialog(this);
    dialog.setObjectName("cameraRawDialog");
    dialog.setWindowTitle(t("cameraRaw"));
    dialog.resize(1200, 800);
    auto *outer = new QVBoxLayout(&dialog);
    auto *body = new QHBoxLayout;
    auto *preview = new RawPreview;
    preview->setObjectName("cameraRawPreview");
    preview->setMinimumSize(440, 380);
    preview->setAlignment(Qt::AlignCenter);
    preview->setStyleSheet("background:#161616;");
    body->addWidget(preview, 1);
    auto *tabs = new QTabWidget;
    tabs->setObjectName("rawPages");
    tabs->setUsesScrollButtons(true);
    tabs->setMinimumWidth(440);
    tabs->setMaximumWidth(510);
    tabs->setStyleSheet("QTabBar::tab{min-width:0;padding:7px 10px;}");
    auto *right = new QVBoxLayout;
    auto *scope = new RawScope;
    scope->setObjectName("rawScope");
    scope->histogramText = t("raw.histogram");
    scope->vectorText = t("raw.vectorscope");
    right->addWidget(scope);
    auto *readout = new QLabel("R —   G —   B —");
    right->addWidget(readout);
    auto *sectionChoice = new QComboBox;
    sectionChoice->setObjectName("rawSection");
    right->addWidget(sectionChoice);
    tabs->tabBar()->hide();
    connect(sectionChoice, qOverload<int>(&QComboBox::currentIndexChanged), tabs, &QTabWidget::setCurrentIndex);
    connect(tabs, &QTabWidget::currentChanged, sectionChoice, &QComboBox::setCurrentIndex);
    right->addWidget(tabs, 1);
    body->addLayout(right);

    outer->addLayout(body, 1);
    QHash<QString, QDoubleSpinBox *> numbers;
    QHash<QString, QSlider *> sliders;
    QHash<QString, QCheckBox *> toggles;
    QHash<QString, QComboBox *> choices;
    QHash<QString, double> defaults;
    QTimer debounce;
    debounce.setSingleShot(true);
    debounce.setInterval(80);
    QFutureWatcher<QImage> watcher;
    quint64 generation = 0, running = 0;
    bool waiting = false, closing = false;
    QImage rendered = source;
    int temporaryClipping = 0;
    preview->setMouseTracking(true);
    preview->hovered = [&, readout](QPointF pt) {
        QPoint at(qFloor(pt.x() * rendered.width()), qFloor(pt.y() * rendered.height()));
        if (rendered.rect().contains(at)) {
            auto color = rendered.pixelColor(at);
            readout->setText(QString("R %1   G %2   B %3").arg(color.red()).arg(color.green()).arg(color.blue()));
        }
    };
    auto schedule = [&] {
        ++generation;
        waiting = true;
        debounce.start();
    };
    QHash<QString, QPushButton *> sectionResets;
    auto page = [&](QString key) {
        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        auto *widget = new QWidget;
        auto *form = new QFormLayout(widget);
        form->setContentsMargins(12, 12, 12, 12);
        form->setSpacing(10);
        auto *sectionReset = new QPushButton(t("reset"));
        sectionReset->setObjectName("rawReset_" + key);
        sectionResets[key] = sectionReset;
        form->addRow(sectionReset);
        scroll->setWidget(widget);
        tabs->addTab(scroll, t("raw." + key));
        sectionChoice->addItem(t("raw." + key));
        return form;
    };
    auto number = [&](QFormLayout *form, QString path, double low, double high, double value = 0, QString label = {}) {
        auto *box = new QDoubleSpinBox;
        box->setObjectName("raw_" + path);
        box->setRange(low, high);
        box->setDecimals((high - low) < 50 ? 2 : 1);
        box->setValue(value);
        box->setKeyboardTracking(false);
        box->setFixedWidth(95);
        auto *slider = new QSlider(Qt::Horizontal);
        slider->setRange(0, 1000);
        slider->setValue(qRound((value - low) / (high - low) * 1000));
        auto *row = new QHBoxLayout;
        row->addWidget(slider, 1);
        row->addWidget(box);
        form->addRow(t(label.isEmpty() ? "raw." + path : label), row);
        if (path == "calibration.process")
            box->setDecimals(0);
        numbers[path] = box;
        sliders[path] = slider;
        defaults[path] = value;
        jsonSet(settings, path, value);
        connect(slider, &QSlider::valueChanged, &dialog,
                [&, box, low, high](int v) { box->setValue(low + v * (high - low) / 1000); });
        connect(box, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, [&, path, slider, low, high](double v) {
            QSignalBlocker block(slider);
            slider->setValue(qRound((v - low) / (high - low) * 1000));
            jsonSet(settings, path, v);
            if (QApplication::keyboardModifiers().testFlag(Qt::AltModifier)) {
                if (QStringList{"exposure", "highlights", "whites"}.contains(path))
                    temporaryClipping = 1;
                else if (QStringList{"shadows", "blacks"}.contains(path))
                    temporaryClipping = 2;
            }
            schedule();
        });
        connect(slider, &QSlider::sliderReleased, &dialog, [&] {
            temporaryClipping = 0;
            schedule();
        });
        return box;
    };
    auto check = [&](QFormLayout *form, QString path, bool value = false, QString label = {}) {
        auto *box = new QCheckBox(t(label.isEmpty() ? "raw." + path : label));
        box->setChecked(value);
        form->addRow(box);
        toggles[path] = box;
        jsonSet(settings, path, value);
        connect(box, &QCheckBox::toggled, &dialog, [&, path](bool on) {
            jsonSet(settings, path, on);
            schedule();
        });
        return box;
    };
    auto choice = [&](QFormLayout *form, QString path, QStringList labels, int value = 0) {
        auto *box = new QComboBox;
        for (const auto &label : labels)
            box->addItem(t("raw." + label));
        box->setCurrentIndex(value);
        form->addRow(t("raw." + path), box);
        choices[path] = box;
        jsonSet(settings, path, value);
        connect(box, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [&, path](int v) {
            jsonSet(settings, path, v);
            schedule();
        });
        return box;
    };
    auto *light = page("light");
    check(light, "enableLight", true, "enabled");
    for (auto key : {"exposure", "contrast", "highlights", "shadows", "whites", "blacks"})
        number(light, key, QString(key) == "exposure" ? -5 : -100, QString(key) == "exposure" ? 5 : 100, 0, key);
    auto *autoLight = new QPushButton(t("raw.auto"));
    light->addRow(autoLight);
    connect(autoLight, &QPushButton::clicked, &dialog, [&] {
        double average = 0;
        auto small = source.scaled(100, 100, Qt::KeepAspectRatio);
        for (int y = 0; y < small.height(); ++y)
            for (int x = 0; x < small.width(); ++x)
                average += qGray(small.pixel(x, y)) / 255.;
        average /= std::max(1, small.width() * small.height());
        numbers["exposure"]->setValue(std::clamp(log2(.45 / std::max(.01, average)), -5., 5.));
    });
    auto *color = page("color");
    check(color, "enableColor", true, "enabled");
    for (auto key : {"temperature", "tint", "vibrance", "saturation"})
        number(color, key, -100, 100, 0, key);
    auto *autoColor = new QPushButton(t("raw.autoWhiteBalance"));
    color->addRow(autoColor);
    connect(autoColor, &QPushButton::clicked, &dialog, [&] {
        auto balance = autoWhiteBalance(source);
        for (auto it = balance.begin(); it != balance.end(); ++it)
            numbers[it.key()]->setValue(it.value().toDouble());
    });
    QLabel *pickHint = new QLabel(t("raw.pickWhiteBalance"));
    pickHint->setWordWrap(true);
    color->addRow(pickHint);
    auto *curve = page("curve");
    check(curve, "enableCurve", true, "enabled");
    for (auto key : {"shadows", "darks", "lights", "highlights"})
        number(curve, "curve." + QString(key), -100, 100);
    number(curve, "curve.shadowSplit", 5, 90, 25);
    number(curve, "curve.darkSplit", 7, 95, 50);
    number(curve, "curve.lightSplit", 9, 98, 75);
    number(curve, "curve.refineSaturation", -100, 100);
    QComboBox channel;
    channel.addItems({"RGB", t("red"), t("green"), t("blue")});
    curve->addRow(t("color"), &channel);
    auto *curveEditor = new CurveEditor;
    curve->addRow(curveEditor);
    QString curveKey = "rgb";
    const QStringList curveKeys{"rgb", "red", "green", "blue"};
    for (const auto &key : curveKeys)
        jsonSet(settings, "curve." + key, curveEditor->points);
    curveEditor->changed = [&] {
        jsonSet(settings, "curve." + curveKey, curveEditor->points);
        schedule();
    };
    connect(&channel, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [&](int i) {
        curveKey = curveKeys[i];
        curveEditor->points = jsonGet(settings, "curve." + curveKey).toArray();
        curveEditor->update();
    });
    auto *mixer = page("mixer");
    check(mixer, "enableMixer", true, "enabled");
    QStringList families{"reds", "oranges", "yellows", "greens", "aquas", "blues", "purples", "magentas"};
    QVector<QDoubleSpinBox *> mixBoxes;
    for (int i = 0; i < 8; ++i) {
        auto *group = new QGroupBox(t("raw." + families[i]));
        auto *form = new QFormLayout(group);
        for (auto component : {"hue", "saturation", "luminance"}) {
            auto *box = number(form, "mixerControl." + QString(component) + QString::number(i), -100, 100, 0,
                               QString(component) == "luminance" ? "lightness" : component);
            mixBoxes.append(box);
            connect(box, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, [&, i, component](double v) {
                auto object = settings["mixer"].toObject();
                auto values = object[component].toArray();
                while (values.size() < 8)
                    values.append(0);
                values[i] = v;
                object[component] = values;
                settings["mixer"] = object;
            });
        }
        mixer->addRow(group);
    }
    auto *points = page("pointColor");
    QComboBox pointChoice;
    for (int i = 0; i < 8; ++i)
        pointChoice.addItem(QString::number(i + 1));
    points->addRow(t("raw.pointColor"), &pointChoice);
    auto *pickPoint = check(points, "pickPoint", false, "raw.pickPoint");
    QStringList pointFields{"hue",      "saturation",      "luminance",
                            "hueShift", "saturationShift", "luminanceShift",
                            "hueRange", "saturationRange", "luminanceRange"};
    for (auto key : pointFields) {
        double max = key == "hue" ? 360 : key == "hueRange" ? 180 : key.endsWith("Shift") ? 100 : 1;
        double min = key.endsWith("Shift") ? -100 : 0;
        double def = key == "hueRange" ? 30 : key.endsWith("Range") ? .4 : 0;
        number(points, "point." + key, min, max, def);
    }
    check(points, "visualizePoint", false, "raw.visualizePoint");
    auto syncPoint = [&] {
        auto object = settings["mixer"].toObject();
        auto list = object["points"].toArray();
        while (list.size() <= pointChoice.currentIndex())
            list.append(QJsonObject());
        list[pointChoice.currentIndex()] = settings["point"].toObject();
        object["points"] = list;
        settings["mixer"] = object;
    };
    for (auto key : pointFields)
        connect(numbers["point." + key], qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog,
                [&] { syncPoint(); });
    connect(&pointChoice, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [&](int i) {
        auto list = settings["mixer"].toObject()["points"].toArray();
        auto p = i < list.size() ? list[i].toObject() : QJsonObject();
        for (auto key : pointFields) {
            auto *box = numbers["point." + key];
            QSignalBlocker b(box);
            box->setValue(p[key].toDouble(defaults["point." + key]));
            auto *slider = sliders["point." + key];
            QSignalBlocker block(slider);
            slider->setValue(qRound((box->value() - box->minimum()) / (box->maximum() - box->minimum()) * 1000));
            jsonSet(settings, "point." + key, box->value());
        }
    });
    auto *grading = page("grading");
    check(grading, "enableGrading", true, "enabled");
    for (auto wheel : {"shadows", "midtones", "highlights", "global"}) {
        auto *group = new QGroupBox(t("raw." + QString(wheel)));
        auto *form = new QFormLayout(group);
        number(form, "grading." + QString(wheel) + ".hue", 0, 360, 0, "hue");
        number(form, "grading." + QString(wheel) + ".saturation", 0, 100, 0, "saturation");
        number(form, "grading." + QString(wheel) + ".luminance", -100, 100, 0, "lightness");
        grading->addRow(group);
    }
    number(grading, "grading.blending", 0, 100, 50);
    number(grading, "grading.balance", -100, 100);
    auto *effects = page("effects");
    check(effects, "enableEffects", true, "enabled");
    for (auto key : {"texture", "clarity", "dehaze"})
        number(effects, key, -100, 100);
    number(effects, "glow", 0, 100);
    choice(effects, "glowStyle", {"diffusion", "bloom", "halation"});
    for (auto key : {"glowRange", "glowSpread", "glowWarmth"})
        number(effects, key, -100, 100);
    number(effects, "vignetteAmount", -100, 100);
    choice(effects, "vignetteStyle", {"highlightPriority", "colorPriority", "paintOverlay"});
    number(effects, "vignetteMidpoint", 0, 100, 50);
    number(effects, "vignetteRoundness", -100, 100);
    number(effects, "vignetteFeather", 0, 100, 50);
    number(effects, "vignetteHighlights", 0, 100);
    number(effects, "grainAmount", 0, 100);
    number(effects, "grainSize", 0, 100, 25);
    number(effects, "grainRoughness", 0, 100, 50);
    auto *detail = page("detail");
    check(detail, "enableDetail", true, "enabled");
    number(detail, "detail.sharpenAmount", 0, 150);
    number(detail, "detail.sharpenRadius", 0, 100, 10);
    number(detail, "detail.sharpenDetail", 0, 100, 25);
    number(detail, "detail.sharpenMasking", 0, 100);
    number(detail, "detail.noiseLuminance", 0, 100);
    number(detail, "detail.noiseLuminanceDetail", 0, 100, 50);
    number(detail, "detail.noiseLuminanceContrast", 0, 100);
    number(detail, "detail.noiseColor", 0, 100);
    number(detail, "detail.noiseColorDetail", 0, 100, 50);
    number(detail, "detail.noiseColorSmoothness", 0, 100, 50);
    check(detail, "sharpenMask", false, "raw.sharpenMask");
    auto *optics = page("optics");
    check(optics, "enableOptics", true, "enabled");
    check(optics, "optics.removeChromaticAberration");
    check(optics, "optics.enableLensProfile");
    number(optics, "optics.profileDistortion", 0, 100, 100);
    number(optics, "optics.profileVignetting", 0, 100, 100);
    number(optics, "optics.distortion", -100, 100);
    number(optics, "optics.purpleAmount", 0, 100);
    number(optics, "optics.purpleHueLow", 0, 360, 270);
    number(optics, "optics.purpleHueHigh", 0, 360, 310);
    number(optics, "optics.greenAmount", 0, 100);
    number(optics, "optics.greenHueLow", 0, 360, 60);
    number(optics, "optics.greenHueHigh", 0, 360, 120);
    number(optics, "optics.vignetteAmount", -100, 100);
    number(optics, "optics.vignetteMidpoint", 0, 100, 50);
    auto *geo = page("geometry");
    check(geo, "enableGeometry", true, "enabled");
    auto *guided = check(geo, "geometry.upright");
    check(geo, "geometry.rectilinear");
    for (auto key : {"vertical", "horizontal", "rotate", "aspect", "scale", "offsetX", "offsetY"})
        number(geo, "geometry." + QString(key), QString(key) == "rotate" ? -45 : -100,
               QString(key) == "rotate" ? 45 : 100);
    check(geo, "geometry.constrainCrop");
    auto *clearGuides = new QPushButton(t("clearGuides"));
    geo->addRow(clearGuides);
    connect(clearGuides, &QPushButton::clicked, &dialog, [&] {
        jsonSet(settings, "geometry.guides", QJsonArray());
        preview->guides.clear();
        preview->update();
        schedule();
    });
    connect(guided, &QCheckBox::toggled, &dialog, [&](bool b) { preview->guideMode = b; });
    preview->guide = [&](QPointF a, QPointF b) {
        if (a.x() < 0 || a.x() > 1 || a.y() < 0 || a.y() > 1 || b.x() < 0 || b.x() > 1 || b.y() < 0 || b.y() > 1 ||
            QLineF(a, b).length() < .01)
            return;
        auto g = jsonGet(settings, "geometry.guides").toArray();
        if (g.size() >= 2)
            g.removeFirst();
        g.append(QJsonArray{a.x(), 1 - a.y(), b.x(), 1 - b.y()});
        jsonSet(settings, "geometry.guides", g);
        if (preview->guides.size() >= 2)
            preview->guides.removeFirst();
        preview->guides.append(QLineF(a, b));
        preview->update();
        schedule();
    };
    auto *calibration = page("calibration");
    check(calibration, "enableCalibration", true, "enabled");
    number(calibration, "calibration.process", 1, 6, 6);
    for (auto key :
         {"shadowTint", "redHue", "redSaturation", "greenHue", "greenSaturation", "blueHue", "blueSaturation"})
        number(calibration, "calibration." + QString(key), -100, 100);
    preview->clicked = [&](QPointF pt) {
        QPoint p(qFloor(pt.x() * source.width()), qFloor(pt.y() * source.height()));
        if (!source.rect().contains(p))
            return;
        if (pickPoint->isChecked()) {
            auto c = source.pixelColor(p);
            numbers["point.hue"]->setValue(std::max(0., double(c.hslHueF())) * 360);
            numbers["point.saturation"]->setValue(c.hslSaturationF());
            numbers["point.luminance"]->setValue(c.lightnessF());
            syncPoint();
        } else if (tabs->currentIndex() == 1) {
            auto balance =
                autoWhiteBalance(source.copy(QRect(p - QPoint(2, 2), QSize(5, 5)).intersected(source.rect())));
            for (auto it = balance.begin(); it != balance.end(); ++it)
                numbers[it.key()]->setValue(it.value().toDouble());
        }
    };
    auto *footer = new QHBoxLayout;
    QCheckBox previewToggle(t("preview")), highlight(t("raw.highlightClip")), shadow(t("raw.shadowClip"));
    previewToggle.setChecked(true);
    footer->addWidget(&previewToggle);
    footer->addWidget(&highlight);
    footer->addWidget(&shadow);
    QPushButton reset(t("reset")), load(t("raw.loadPreset")), save(t("raw.savePreset"));
    footer->addWidget(&reset);
    footer->addWidget(&load);
    footer->addWidget(&save);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    footer->addWidget(buttons);
    outer->addLayout(footer);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(&previewToggle, &QCheckBox::toggled, &dialog, [&] { schedule(); });
    connect(&highlight, &QCheckBox::toggled, &dialog, [&](bool b) {
        settings["highlightClip"] = b;
        schedule();
    });
    connect(&shadow, &QCheckBox::toggled, &dialog, [&](bool b) {
        settings["shadowClip"] = b;
        schedule();
    });
    auto restoreControls = [&] {
        settings = normalizedCameraRaw(settings);
        for (auto it = numbers.begin(); it != numbers.end(); ++it) {
            double value = jsonGet(settings, it.key()).toDouble(defaults[it.key()]);
            if (it.key().startsWith("mixerControl.")) {
                auto key = it.key().mid(13);
                int i = key.right(1).toInt();
                auto a = settings["mixer"].toObject()[key.chopped(1)].toArray();
                value = i < a.size() ? a[i].toDouble() : 0;
            }
            if (it.key().startsWith("point.")) {
                auto a = settings["mixer"].toObject()["points"].toArray();
                value = pointChoice.currentIndex() < a.size()
                            ? a[pointChoice.currentIndex()].toObject()[it.key().mid(6)].toDouble(defaults[it.key()])
                            : defaults[it.key()];
            }
            QSignalBlocker b(it.value());
            it.value()->setValue(value);
            auto *slider = sliders[it.key()];
            QSignalBlocker bs(slider);
            slider->setValue(qRound((it.value()->value() - it.value()->minimum()) /
                                    (it.value()->maximum() - it.value()->minimum()) * 1000));
            jsonSet(settings, it.key(), it.value()->value());
        }
        for (auto it = toggles.begin(); it != toggles.end(); ++it) {
            QSignalBlocker b(it.value());
            it.value()->setChecked(jsonGet(settings, it.key()).toBool(it.key().startsWith("enable")));
        }
        for (auto it = choices.begin(); it != choices.end(); ++it) {
            QSignalBlocker b(it.value());
            it.value()->setCurrentIndex(jsonGet(settings, it.key()).toInt());
        }
        curveEditor->points = jsonGet(settings, "curve." + curveKey).toArray();
        if (curveEditor->points.size() < 2)
            curveEditor->points = {QJsonObject{{"x", 0}, {"y", 0}}, QJsonObject{{"x", 1}, {"y", 1}}};
        curveEditor->update();
        preview->guideMode = guided->isChecked();
        preview->guides.clear();
        for (const auto &v : jsonGet(settings, "geometry.guides").toArray()) {
            auto a = v.toArray();
            preview->guides.append(
                QLineF(QPointF(a[0].toDouble(), 1 - a[1].toDouble()), QPointF(a[2].toDouble(), 1 - a[3].toDouble())));
        }
        preview->update();
        highlight.setChecked(settings["highlightClip"].toBool());
        shadow.setChecked(settings["shadowClip"].toBool());
        schedule();
    };
    for (auto it = sectionResets.cbegin(); it != sectionResets.cend(); ++it) {
        connect(it.value(), &QPushButton::clicked, &dialog, [&, section = it.key()] {
            settings = resetCameraRawSection(settings, section);
            if (section == "mixer" || section == "pointColor") {
                QSignalBlocker block(&pointChoice); pointChoice.setCurrentIndex(0);
            }
            restoreControls();
        });
    }
    const QJsonObject initial = settings;
    connect(&reset, &QPushButton::clicked, &dialog, [&] {
        settings = initial;
        restoreControls();
    });
    connect(&save, &QPushButton::clicked, &dialog, [&] {
        auto path = QFileDialog::getSaveFileName(&dialog, t("raw.savePreset"), {}, "JSON (*.json)");
        if (path.isEmpty())
            return;
        QSaveFile file(path);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QJsonDocument(settings).toJson());
            file.commit();
        }
    });
    connect(&load, &QPushButton::clicked, &dialog, [&] {
        auto path = QFileDialog::getOpenFileName(&dialog, t("raw.loadPreset"), {}, "JSON (*.json)");
        QFile file(path);
        if (path.isEmpty() || !file.open(QIODevice::ReadOnly))
            return;
        if (file.size() > 1024 * 1024)
            return;
        auto value = QJsonDocument::fromJson(file.readAll());
        if (value.isObject()) {
            settings = value.object();
            restoreControls();
        }
    });
    std::function<void()> start = [&] {
        if (closing || watcher.isRunning())
            return;
        waiting = false;
        running = generation;
        auto snapshot = settings;
        int clip = temporaryClipping;
        snapshot["visualizePoint"] = toggles["visualizePoint"]->isChecked() ? pointChoice.currentIndex() : -1;
        bool enabled = previewToggle.isChecked();
        watcher.setFuture(
            QtConcurrent::run([source, snapshot, enabled, clip, scale = double(source.width()) / full.width()] {
                auto parameters = snapshot; parameters["kind"] = "Camera Raw";
                return enabled ? filteredAccelerated(source, parameters, scale, clip) : source;
            }));
    };
    connect(&debounce, &QTimer::timeout, &dialog, start);
    connect(&watcher, &QFutureWatcher<QImage>::finished, &dialog, [&] {
        try {
            if (running == generation) {
                rendered = watcher.result();
                scope->image = rendered;
                scope->update();
                preview->setPixmap(QPixmap::fromImage(rendered).scaled(preview->size(), Qt::KeepAspectRatio,
                                                                       Qt::SmoothTransformation));
            }
        } catch (...) {
            preview->setText(t("imageError"));
        }
        if (waiting && !closing)
            start();
    });
    preview->setPixmap(
        QPixmap::fromImage(source).scaled(preview->minimumSize(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    scope->image = source;
    schedule();
    const bool accept = dialog.exec() == QDialog::Accepted;
    closing = true;
    debounce.stop();
    watcher.waitForFinished();
    if (!accept)
        return;
    settings.remove("highlightClip");
    settings.remove("shadowClip");
    settings.remove("visualizePoint");
    settings.remove("sharpenMask");
    auto output = std::make_shared<QImage>();
    runIO([output, full, settings] { auto parameters = settings; parameters["kind"] = "Camera Raw"; *output = filteredAccelerated(full, parameters); },
          [this, output, index, original, settings] {
              invoke([&] {
                  document_.edit([&](State &s) {
                      auto &l = s.layers[index];
                      auto after = *output;
                      constrainSelection(original, l.placement, l.image, after);
                      l.image = after;
                      l.originalPng.clear();
                      l.metadata.remove("text");
                      l.metadata.remove("shape");
                  });
                  lastFilter_ = settings;
                  lastFilter_["kind"] = "Camera Raw";
                  refreshSelection();
              });
          });
}
} // namespace comp
