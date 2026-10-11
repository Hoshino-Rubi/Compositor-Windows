#include "Pixels.h"
#include "MainWindow.h"
#include "Filters.h"
#include "Renderer.h"
#include "Selection.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QCheckBox>
#include <QLineEdit>
#include <QColorDialog>
#include <QPainter>
#include <QScrollArea>
#include <QSignalBlocker>
namespace comp {
void MainWindow::filterDialog(const QString &kind, bool adjustment, QString editLayer) {
    if (kind == "Camera Raw") {
        cameraRawDialog();
        return;
    }
    if (!document_.hasDocument())
        return;
    const QHash<QString, QString> titles{{"Hue/Saturation", "filter.hsv"},
                                         {"Levels", "filter.levels"},
                                         {"Curves", "filter.curves"},
                                         {"Exposure", "filter.exposure"},
                                         {"Black & White", "filter.grayscale"},
                                         {"Invert", "filter.invert"},
                                         {"Gaussian Blur", "filter.blur"},
                                         {"Add Noise", "filter.noise"},
                                         {"Color Balance", "filter.colorBalance"},
                                         {"Gradient Map", "filter.gradientMap"},
                                         {"Grain", "filter.grain"},
                                         {"Camera Raw", "filter.cameraRaw"},
                                         {"Motion Blur", "filter.motionBlur"},
                                         {"Scanlines", "filter.scanlines"},
                                         {"Vignette", "filter.vignette"}, {"Bloom / Glow", "filter.bloom"},
                                         {"Dither", "filter.dither"}, {"Tonal Contrast", "filter.tonalContrast"},
                                         {"Lens Correction", "filter.lensCorrection"}};
    const int active = document_.state().index(document_.state().active);
    if (active < 0 && !adjustment)
        return;
    const int editIndex = document_.state().index(editLayer);
    const QJsonObject original =
        editIndex >= 0 ? document_.state().layers[editIndex].metadata["adjustment"].toObject() : QJsonObject();
    QDialog dialog(this);
    dialog.setObjectName("filterDialog");
    dialog.setWindowTitle(t(titles.value(kind)));
    dialog.resize(690, 430);
    auto *outer = new QVBoxLayout(&dialog);
    auto *row = new QHBoxLayout;
    auto *form = new QFormLayout;
    QHash<QString, QDoubleSpinBox *> controls;
    auto field = [&](QString key, double value, double low, double high, QString label = {}) {
        auto *box = new QDoubleSpinBox;
        box->setObjectName("filter_" + key);
        box->setRange(low, high);
        box->setDecimals(key == "gamma" || key == "exposure" || key == "offset" ? 2 : 1);
        box->setValue(value);
        box->setKeyboardTracking(false);
        box->setMinimumWidth(95);
        controls[key] = box;
        form->addRow(t(label.isEmpty() ? key : label), box);
    };
    QComboBox ditherStyle, ditherColors, pixelShape;
    QCheckBox lightOnDark(t("lightOnDark"));
    QLineEdit characters;
    QPushButton vignetteColor(t("color")), darkColor(t("dark")), lightColor(t("white"));
    QColor vignette = Qt::black, dark = Qt::black, light = Qt::white;
    QComboBox channel;
    channel.addItems({"RGB", t("red"), t("green"), t("blue")});
    QComboBox hsvRange;
    const QStringList hsvNames{"Master", "Reds", "Yellows", "Greens", "Cyans", "Blues", "Magentas"};
    for (const auto &name : hsvNames)
        hsvRange.addItem(t("hsv." + name), name);
    QJsonObject hsv = original["hsvSettings"].toObject();
    QJsonObject adjustments;
    auto stored = hsv["adjustments"];
    if (stored.isObject())
        adjustments = stored.toObject();
    else {
        auto a = stored.toArray();
        for (int i = 0; i + 1 < a.size(); i += 2)
            adjustments[a[i].toString()] = a[i + 1];
    }
    if (adjustments.isEmpty())
        adjustments["Master"] = QJsonObject{{"hue", original["hue"].toDouble()},
                                            {"saturation", original["saturation"].toDouble()},
                                            {"lightness", original["lightness"].toDouble()}};
    QString activeRange = "Master";
    auto decodeDictionary = [](QJsonValue value) {
        if (value.isObject())
            return value.toObject();
        QJsonObject o;
        auto a = value.toArray();
        for (int i = 0; i + 1 < a.size(); i += 2)
            o[a[i].toString()] = a[i + 1];
        return o;
    };
    QJsonObject hueBands = decodeDictionary(hsv["bands"]);
    const QList<QJsonArray> bandDefaults{{0, 0, 360, 360},    {315, 345, 15, 45},   {15, 45, 75, 105},
                                         {75, 105, 135, 165}, {135, 165, 195, 225}, {195, 225, 255, 285},
                                         {255, 285, 315, 345}};
    for (int i = 0; i < hsvNames.size(); ++i)
        if (!hueBands.contains(hsvNames[i]))
            hueBands[hsvNames[i]] = QJsonObject{{"falloffStart", bandDefaults[i][0]},
                                                {"rangeStart", bandDefaults[i][1]},
                                                {"rangeEnd", bandDefaults[i][2]},
                                                {"falloffEnd", bandDefaults[i][3]}};
    QCheckBox colorize(t("colorize")), invertRange(t("invertRange"));
    colorize.setChecked(hsv["colorize"].toBool());
    invertRange.setChecked(hsv["invertRange"].toBool());
    if (kind == "Hue/Saturation") {
        form->addRow(t("colorRange"), &hsvRange);
        form->addRow(&colorize);
        form->addRow(&invertRange);
        field("hue", original["hue"].toDouble(), -360, 360);
        field("saturation", original["saturation"].toDouble(), -100, 100);
        field("lightness", adjustments["Master"].toObject()["lightness"].toDouble(), -100, 100);
        controls["hue"]->setValue(adjustments["Master"].toObject()["hue"].toDouble());
        controls["saturation"]->setValue(adjustments["Master"].toObject()["saturation"].toDouble());
        for (auto key : {"falloffStart", "rangeStart", "rangeEnd", "falloffEnd"}) {
            field(key, hueBands["Master"].toObject()[key].toDouble(), 0, 360, "hsv." + QString(key));
            controls[key]->setEnabled(false);
        }
    } else if (kind == "Exposure") {
        auto v = original["exposureSettings"].toObject();
        field("exposure", v["exposure"].toDouble(), -20, 20);
        field("offset", v["offset"].toDouble(), -.5, .5);
        field("gamma", v["gamma"].toDouble(1), .01, 9.99);
    } else if (kind == "Levels") {
        auto ranges = original["levels"].toObject()["ranges"].toArray();
        auto v = ranges.isEmpty() ? QJsonObject() : ranges[0].toObject();
        field("black", v["black"].toDouble(), 0, 254, "inputBlack");
        field("gamma", v["gamma"].toDouble(1), .1, 9.99);
        field("white", v["white"].toDouble(255), 1, 255, "inputWhite");
        field("outputBlack", v["outputBlack"].toDouble(), 0, 255);
        field("outputWhite", v["outputWhite"].toDouble(255), 0, 255);
    } else if (kind == "Curves") {
        form->addRow(t("color"), &channel);
        field("black", 0, 0, 255, "outputBlack");
        field("midpoint", 128, 0, 255);
        field("white", 255, 0, 255, "outputWhite");
    } else if (kind == "Gaussian Blur")
        field("blurRadius", original["blurRadius"].toDouble(10), .1, 250, "radius");
    else if (kind == "Motion Blur") {
        field("motionDistance", original["motionDistance"].toDouble(10), 1, 2000, "distance");
        field("motionAngle", original["motionAngle"].toDouble(), -90, 90, "rotation");
    } else if (kind == "Add Noise")
        field("noiseAmount", original["noiseAmount"].toDouble(10), .1, 400, "amount");
    else if (kind == "Color Balance") {
        auto v = original["colorBalanceSettings"].toObject();
        field("midCyanRed", v["midCyanRed"].toDouble(), -100, 100, "red");
        field("midMagentaGreen", v["midMagentaGreen"].toDouble(), -100, 100, "green");
        field("midYellowBlue", v["midYellowBlue"].toDouble(), -100, 100, "blue");
    } else if (kind == "Black & White") {
        auto v = original["blackWhiteSettings"].toObject();
        field("reds", v["reds"].toDouble(40), -200, 300, "red");
        field("yellows", v["yellows"].toDouble(60), -200, 300, "yellow");
        field("greens", v["greens"].toDouble(40), -200, 300, "green");
        field("cyans", v["cyans"].toDouble(60), -200, 300, "cyan");
        field("blues", v["blues"].toDouble(20), -200, 300, "blue");
        field("magentas", v["magentas"].toDouble(80), -200, 300, "magenta");
    } else if (kind == "Grain") {
        auto v = original["grainSettings"].toObject();
        field("amount", v["amount"].toDouble(25), 0, 100);
        field("size", v["size"].toDouble(1.5), .5, 20);
        field("roughness", v["roughness"].toDouble(50), 0, 100);
    } else if (kind == "Vignette") {
        field("vignetteAmount", 35, 0, 100, "amount");
        field("vignetteMidpoint", 50, 0, 100, "midpoint");
        field("vignetteRoundness", 100, -100, 100, "roundness");
        field("vignetteFeather", 60, 0, 100, "feather");
        field("vignetteHighlights", 25, 0, 100, "highlights");
        form->addRow(t("color"), &vignetteColor);
    } else if (kind == "Bloom / Glow") {
        field("bloomAmount", 40, 0, 100, "amount");
        field("bloomRadius", 24, 1, 150, "radius");
    } else if (kind == "Tonal Contrast") {
        field("tonalAmount", 50, 0, 100, "amount");
        field("tonalRadius", 16, 1, 150, "radius");
        field("tonalShadows", 40, -100, 100, "shadows");
        field("tonalMidtones", 60, -100, 100, "midtones");
        field("tonalHighlights", 30, -100, 100, "highlights");
    } else if (kind == "Lens Correction") {
        field("distortion", 0, -100, 100, "distortion");
    } else if (kind == "Dither") {
        for (const auto &key : QStringList{"dither.atkinson", "dither.floyd", "dither.bayer2", "dither.bayer4",
                                            "dither.bayer8", "dither.dots", "dither.lines", "dither.diamonds",
                                            "dither.patterns", "dither.ascii"})
            ditherStyle.addItem(t(key));
        form->addRow(t("style"), &ditherStyle);
        field("pixelSize", 2, 1, 32);
        field("cellSize", 8, 4, 64);
        field("textSize", 14, 6, 64);
        field("levels", 2, 2, 8);
        field("angle", 45, -180, 180, "rotation");
        field("diffusion", 100, 0, 100);
        field("density", 0, -100, 100);
        field("contrast", 0, -100, 100);
        ditherColors.addItems({t("scan.monochrome"), t("scan.twoColors"), t("scan.original")});
        form->addRow(t("color"), &ditherColors);
        pixelShape.addItems({t("square"), t("dot")});
        form->addRow(t("shape"), &pixelShape);
        lightOnDark.setChecked(true);
        form->addRow(&lightOnDark);
        characters.setText(" .:-=+*#%@");
        form->addRow(t("characters"), &characters);
        form->addRow(t("dark"), &darkColor);
        form->addRow(t("white"), &lightColor);
    } else if (kind == "Scanlines") {
        field("lineSpacing", 4, 2, 32, "scan.lineSpacing");
        field("thickness", 70, 5, 100, "scan.thickness");
        field("dots", 0, 0, 100, "scan.dots");
        field("wobble", 0, 0, 64, "scan.wobble");
        field("displace", 0, -100, 100, "scan.displace");
        field("smoothness", 50, 0, 100, "scan.smoothness");
        field("threshold", 0, 0, 100, "scan.threshold");
        field("split", 0, 0, 16, "scan.split");
        field("density", 0, -100, 100, "scan.density");
        field("contrast", 0, -100, 100, "contrast");
        field("blackLevel", 0, 0, 100, "scan.blackLevel");
        field("glow", 0, 0, 100, "raw.glow");
        channel.clear();
        channel.addItems({t("scan.monochrome"), t("scan.original"), t("scan.twoColors")});
        form->addRow(t("color"), &channel);
    } else if (kind == "Camera Raw") {
        for (const auto &key : QStringList{"temperature", "tint", "exposure", "contrast", "highlights", "shadows",
                                           "whites", "blacks", "vibrance", "saturation"})
            field(key, 0, key == "exposure" ? -5 : -100, key == "exposure" ? 5 : 100);
    } else
        form->addRow(new QLabel(t("filterNote")));
    auto *formWidget = new QWidget;
    formWidget->setLayout(form);
    auto *formScroll = new QScrollArea;
    formScroll->setWidgetResizable(true);
    formScroll->setWidget(formWidget);
    formScroll->setMinimumWidth(285);
    row->addWidget(formScroll);
    auto *preview = new QLabel;
    preview->setObjectName("filterPreview");
    preview->setMinimumSize(330, 300);
    preview->setAlignment(Qt::AlignCenter);
    preview->setStyleSheet("background:#191919;border:1px solid #444;");
    row->addWidget(preview, 1);
    outer->addLayout(row);
    QCheckBox live(t("preview"));
    live.setChecked(true);
    outer->addWidget(&live);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (kind == "Hue/Saturation")
        connect(&hsvRange, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [&](int i) {
            QJsonObject a;
            for (auto key : {"hue", "saturation", "lightness"})
                a[key] = controls[key]->value();
            adjustments[activeRange] = a;
            QJsonObject band;
            for (auto key : {"falloffStart", "rangeStart", "rangeEnd", "falloffEnd"})
                band[key] = controls[key]->value();
            hueBands[activeRange] = band;
            activeRange = hsvNames[i];
            a = adjustments[activeRange].toObject();
            for (auto key : {"hue", "saturation", "lightness"}) {
                QSignalBlocker block(controls[key]);
                controls[key]->setValue(a[key].toDouble());
            }
            auto nextBand = hueBands[activeRange].toObject();
            for (auto key : {"falloffStart", "rangeStart", "rangeEnd", "falloffEnd"}) {
                QSignalBlocker block(controls[key]);
                controls[key]->setValue(nextBand[key].toDouble());
                controls[key]->setEnabled(i != 0);
            }
        });
    auto read = [&]() {
        QJsonObject v;
        for (auto it = controls.begin(); it != controls.end(); ++it)
            v[it.key()] = it.value()->value();
        if (kind == "Hue/Saturation") {
            QJsonObject a, band;
            for (auto key : {"hue", "saturation", "lightness"})
                a[key] = v[key];
            for (auto key : {"falloffStart", "rangeStart", "rangeEnd", "falloffEnd"})
                band[key] = v[key];
            adjustments[activeRange] = a;
            hueBands[activeRange] = band;
            QJsonArray bandArray;
            for (auto it = hueBands.begin(); it != hueBands.end(); ++it) {
                bandArray.append(it.key());
                bandArray.append(it.value());
            }
            hsv["bands"] = bandArray;
            QJsonArray packed;
            for (auto it = adjustments.begin(); it != adjustments.end(); ++it) {
                packed.append(it.key());
                packed.append(it.value());
            }
            hsv["range"] = activeRange;
            hsv["adjustments"] = packed;
            hsv["colorize"] = colorize.isChecked();
            hsv["invertRange"] = invertRange.isChecked();
            if (!hsv.contains("bands")) {
                QJsonArray bands;
                const QList<QJsonArray> limits{{0, 0, 360, 360},    {315, 345, 15, 45},   {15, 45, 75, 105},
                                               {75, 105, 135, 165}, {135, 165, 195, 225}, {195, 225, 255, 285},
                                               {255, 285, 315, 345}};
                for (int i = 0; i < hsvNames.size(); ++i) {
                    bands.append(hsvNames[i]);
                    bands.append(QJsonObject{{"falloffStart", limits[i][0]},
                                             {"rangeStart", limits[i][1]},
                                             {"rangeEnd", limits[i][2]},
                                             {"falloffEnd", limits[i][3]}});
                }
                hsv["bands"] = bands;
            }
        }
        QJsonObject settings = original;
        settings["kind"] = kind;
        if (kind == "Hue/Saturation")
            settings["hsvSettings"] = hsv;
        if (kind == "Levels") {
            v["white"] = std::max(v["white"].toDouble(), v["black"].toDouble() + 1);
            auto ranges = settings["levels"].toObject()["ranges"].toArray();
            if (ranges.size() != 4)
                ranges = QJsonArray{
                    v,
                    QJsonObject{{"black", 0}, {"white", 255}, {"gamma", 1}, {"outputBlack", 0}, {"outputWhite", 255}},
                    QJsonObject{{"black", 0}, {"white", 255}, {"gamma", 1}, {"outputBlack", 0}, {"outputWhite", 255}},
                    QJsonObject{{"black", 0}, {"white", 255}, {"gamma", 1}, {"outputBlack", 0}, {"outputWhite", 255}}};
            ranges[0] = v;
            settings["levels"] = QJsonObject{{"channel", "RGB"}, {"ranges", ranges}};
        } else if (kind == "Curves") {
            QJsonArray identity{QJsonObject{{"x", 0}, {"y", 0}}, QJsonObject{{"x", 255}, {"y", 255}}};
            QJsonArray channels{identity, identity, identity, identity};
            channels[channel.currentIndex()] =
                QJsonArray{QJsonObject{{"x", 0}, {"y", v["black"]}}, QJsonObject{{"x", 128}, {"y", v["midpoint"]}},
                           QJsonObject{{"x", 255}, {"y", v["white"]}}};
            settings["curves"] = QJsonObject{{"channel", "RGB"}, {"channels", channels}};
        } else if (kind == "Exposure")
            settings["exposureSettings"] = v;
        else if (kind == "Color Balance") {
            auto balance = settings["colorBalanceSettings"].toObject();
            for (const auto &key :
                 QStringList{"shadowCyanRed", "shadowMagentaGreen", "shadowYellowBlue", "midCyanRed", "midMagentaGreen",
                             "midYellowBlue", "highlightCyanRed", "highlightMagentaGreen", "highlightYellowBlue"})
                if (!balance.contains(key))
                    balance[key] = 0;
            for (auto it = v.begin(); it != v.end(); ++it)
                balance[it.key()] = it.value();
            balance["preserveLuminosity"] = true;
            settings["colorBalanceSettings"] = balance;
        } else if (kind == "Black & White") {
            v["tint"] = false;
            v["tintHue"] = 40;
            v["tintSaturation"] = 20;
            settings["blackWhiteSettings"] = v;
        } else if (kind == "Grain") {
            v["seed"] = 0;
            settings["grainSettings"] = v;
        } else if (kind == "Gradient Map") {
            auto color = [](QColor c) {
                return QJsonObject{{"red", c.redF()}, {"green", c.greenF()}, {"blue", c.blueF()}};
            };
            settings["gradientMapSettings"] = QJsonObject{{"shadows", color(canvas_->color())},
                                                          {"highlights", color(backgroundColorValue_)},
                                                          {"reversed", false}};
        } else
            for (auto it = v.begin(); it != v.end(); ++it)
                settings[it.key()] = it.value();
        if (kind == "Vignette")
            settings["vignetteColor"] = vignette.name();
        if (kind == "Dither") {
            settings["style"] = ditherStyle.currentIndex();
            settings["colors"] = ditherColors.currentIndex();
            settings["pixelShape"] = pixelShape.currentIndex();
            settings["lightOnDark"] = lightOnDark.isChecked();
            settings["characters"] = characters.text();
            settings["dark"] = dark.name();
            settings["light"] = light.name();
        }
        if (kind == "Scanlines") {
            settings["colors"] = channel.currentIndex();
            settings["dark"] = canvas_->color().name();
            settings["light"] = backgroundColorValue_.name();
        }
        if (adjustment || !editLayer.isEmpty()) {
            if (!settings.contains("hue"))
                settings["hue"] = 0;
            if (!settings.contains("saturation"))
                settings["saturation"] = 0;
            if (!settings.contains("lightness"))
                settings["lightness"] = 0;
            if (!settings.contains("colorize"))
                settings["colorize"] = false;
            QJsonObject range{{"black", 0}, {"white", 255}, {"gamma", 1}, {"outputBlack", 0}, {"outputWhite", 255}};
            if (!settings.contains("levels"))
                settings["levels"] =
                    QJsonObject{{"channel", "RGB"}, {"ranges", QJsonArray{range, range, range, range}}};
            QJsonArray pts{QJsonObject{{"x", 0}, {"y", 0}}, QJsonObject{{"x", 255}, {"y", 255}}};
            if (!settings.contains("curves"))
                settings["curves"] = QJsonObject{{"channel", "RGB"}, {"channels", QJsonArray{pts, pts, pts, pts}}};
        }
        return settings;
    };
    const State base = document_.state();
    const QSize target = base.size.scaled(400, 400, Qt::KeepAspectRatio);
    QImage source =
        active >= 0 ? (mask_->isChecked() ? base.layers[active].mask : base.layers[active].image) : QImage();
    const QSize sourceSize = source.size();
    if (!source.isNull())
        source = source.scaled(500, 500, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    auto update = [&] {
        try {
            State candidate = base;
            if (live.isChecked()) {
                auto settings = read();
                if (adjustment || !editLayer.isEmpty()) {
                    if (editIndex >= 0)
                        candidate.layers[editIndex].metadata["adjustment"] = settings;
                    else {
                        Layer l;
                        l.id = newId();
                        l.name = "preview";
                        l.placement.size = base.size;
                        l.metadata["adjustment"] = settings;
                        if (active >= 0)
                            l.parent = base.layers[active].parent;
                        candidate.layers.insert(active < 0 ? int(candidate.layers.size()) : active + 1, l);
                    }
                } else if (active >= 0 && !source.isNull()) {
                    if (sourceSize.width() > 0 && kind == "Bloom / Glow")
                        settings["bloomRadius"] = settings["bloomRadius"].toDouble() * source.width() / sourceSize.width();
                    if (sourceSize.width() > 0 && kind == "Tonal Contrast")
                        settings["tonalRadius"] = settings["tonalRadius"].toDouble() * source.width() / sourceSize.width();
                    if (kind == "Gaussian Blur" && sourceSize.width() > 0)
                        settings["blurRadius"] =
                            settings["blurRadius"].toDouble() * source.width() / sourceSize.width();
                    auto output = filtered(source, settings);
                    if (!base.selectionMask.isNull()) {
                        constrainSelection(base,
                                           mask_->isChecked() && !base.layers[active].maskLinked
                                               ? base.layers[active].maskPlacement
                                               : base.layers[active].placement,
                                           source, output);
                    } else if (base.selectionActive) {
                        auto clipped = source;
                        QPainter p(&clipped);
                        const auto placement = mask_->isChecked() && !base.layers[active].maskLinked
                                                   ? base.layers[active].maskPlacement
                                                   : base.layers[active].placement;
                        p.setClipPath(placement.matrix(source.size()).inverted().map(base.selection));
                        p.setCompositionMode(QPainter::CompositionMode_Source);
                        p.drawImage(0, 0, output);
                        p.end();
                        output = clipped;
                    }
                    if (mask_->isChecked())
                        candidate.layers[active].mask = output;
                    else
                        candidate.layers[active].image = output;
                }
            }
            preview->setPixmap(QPixmap::fromImage(comp::render(candidate, target)));
        } catch (const Error &error) {
            preview->setText(t(error.key));
        }
    };
    QTimer debounce;
    debounce.setSingleShot(true);
    debounce.setInterval(80);
    connect(&debounce, &QTimer::timeout, &dialog, update);
    for (auto *box : controls)
        connect(box, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, [&] { debounce.start(); });
    connect(&channel, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [&] { debounce.start(); });
    connect(&hsvRange, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [&] { debounce.start(); });
    connect(&colorize, &QCheckBox::toggled, &dialog, [&] { debounce.start(); });
    connect(&invertRange, &QCheckBox::toggled, &dialog, [&] { debounce.start(); });
    connect(&ditherStyle, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [&] { debounce.start(); });
    connect(&ditherColors, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [&] { debounce.start(); });
    connect(&pixelShape, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [&] { debounce.start(); });
    connect(&lightOnDark, &QCheckBox::toggled, &dialog, [&] { debounce.start(); });
    connect(&characters, &QLineEdit::textChanged, &dialog, [&] { debounce.start(); });
    for (auto [button, value] : {std::pair{&vignetteColor, &vignette}, std::pair{&darkColor, &dark},
                                 std::pair{&lightColor, &light}})
        connect(button, &QPushButton::clicked, &dialog, [&, value] {
            QColor next = QColorDialog::getColor(*value, &dialog);
            if (next.isValid()) { *value = next; debounce.start(); }
        });
    connect(&live, &QCheckBox::toggled, &dialog, [&] { update(); });
    update();
    if (dialog.exec() != QDialog::Accepted)
        return;
    const auto settings = read();
    invoke([&] {
        if (adjustment || !editLayer.isEmpty())
            document_.edit([&](State &s) {
                if (editIndex >= 0) {
                    s.layers[editIndex].metadata["adjustment"] = settings;
                    return;
                }
                Layer l;
                l.id = newId();
                l.name = t(titles.value(kind));
                l.placement.size = s.size;
                l.metadata["adjustment"] = settings;
                if (active >= 0)
                    l.parent = s.layers[active].parent;
                if (s.selectionActive) {
                    l.mask = QImage(s.size, maskFormat(s.bitDepth));
                    l.mask.fill(Qt::black);
                    QPainter p(&l.mask);
                    p.fillPath(s.selection, Qt::white);
                    p.end();
                    if (!s.selectionMask.isNull())
                        l.mask = s.selectionMask.copy();
                    l.maskPlacement = l.placement;
                }
                int position = active < 0 ? int(s.layers.size()) : active + 1;
                s.layers.insert(position, l);
                s.active = l.id;
            });
        else {
            document_.applyFilter(settings, mask_->isChecked());
            lastFilter_ = settings;
            refreshSelection();
        }
    });
}
} // namespace comp
