#include "MainWindow.h"
#include "Effects.h"
#include "Renderer.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QColorDialog>
#include <QSet>
namespace comp {
void MainWindow::effectsDialog() {
    const int index = document_.state().index(document_.state().active);
    if (index < 0)
        return;
    auto settings = document_.state().layers[index].metadata["effects"].toObject();
    QDialog dialog(this);
    dialog.setObjectName("effectsDialog");
    dialog.setWindowTitle(t("layerEffects"));
    auto *outer = new QVBoxLayout(&dialog);
    auto *row = new QHBoxLayout;
    auto *form = new QFormLayout;
    QComboBox kind;
    const QStringList keys{"stroke", "shadow", "colorOverlay", "innerShadow", "outerGlow", "innerGlow"};
    for (const auto &key : keys)
        kind.addItem(t("effect." + key), key);
    form->addRow(t("effect"), &kind);
    QCheckBox enabled(t("enabled")), inside(t("inside"));
    form->addRow(&enabled);
    QDoubleSpinBox size, distance, angle, opacity;
    size.setRange(0, 500);
    distance.setRange(0, 5000);
    angle.setRange(-360, 360);
    opacity.setRange(0, 100);
    opacity.setSuffix("%");
    form->addRow(t("size"), &size);
    form->addRow(t("distance"), &distance);
    form->addRow(t("rotation"), &angle);
    form->addRow(t("opacity"), &opacity);
    form->addRow(&inside);
    QColor color = Qt::black;
    QPushButton swatch(t("color"));
    form->addRow(&swatch);
    row->addLayout(form);
    QLabel preview;
    preview.setMinimumSize(340, 300);
    preview.setAlignment(Qt::AlignCenter);
    row->addWidget(&preview);
    outer->addLayout(row);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QString current = keys.first();
    bool loading = false;
    auto store = [&] {
        if (loading)
            return;
        if (!enabled.isChecked()) {
            settings.remove(current);
            return;
        }
        QJsonObject value{{"enabled", true},
                          {"red", color.redF()},
                          {"green", color.greenF()},
                          {"blue", color.blueF()},
                          {"opacity", opacity.value() / 100.}};
        if (current == "stroke" || current == "outerGlow" || current == "innerGlow")
            value["size"] = size.value();
        if (current == "stroke")
            value["inside"] = inside.isChecked();
        if (current == "shadow" || current == "innerShadow") {
            value["blur"] = size.value();
            value["distance"] = distance.value();
            value["angle"] = angle.value();
        }
        settings[current] = value;
    };
    State snapshot = document_.state();
    snapshot.layers[index].metadata.remove("effects");
    const QSize target = snapshot.size.scaled(340, 300, Qt::KeepAspectRatio);
    QImage source;
    if (snapshot.layers[index].group) {
        auto ids = snapshot.descendants(snapshot.layers[index].id);
        QSet<QString> included;
        for (int i : ids)
            included.insert(snapshot.layers[i].id);
        snapshot.layers.removeIf([&](const Layer &l) { return !included.contains(l.id); });
        for (auto &l : snapshot.layers)
            if (!included.contains(l.parent))
                l.parent.clear();
        source = comp::render(snapshot, target);
    } else
        source = layerSurface(snapshot, index, target);
    auto update = [&] {
        store();
        try {
            preview.setPixmap(
                QPixmap::fromImage(applyEffects(source, settings, double(target.width()) / snapshot.size.width())));
        } catch (const Error &e) {
            preview.setText(t(e.key));
        }
    };
    auto load = [&] {
        loading = true;
        const auto value = settings[current].toObject();
        enabled.setChecked(!value.isEmpty());
        const bool glow = current == "outerGlow" || current == "innerGlow",
                   shadow = current == "shadow" || current == "innerShadow";
        color = QColor::fromRgbF(value["red"].toDouble(glow ? 1 : 0), value["green"].toDouble(glow ? 1 : 0),
                                 value["blue"].toDouble(glow ? 1 : 0));
        size.setValue(value[shadow ? "blur" : "size"].toDouble(current == "stroke" ? 4 : 20));
        distance.setValue(value["distance"].toDouble(20));
        angle.setValue(value["angle"].toDouble(90));
        opacity.setValue(value["opacity"].toDouble(shadow ? .5 : 1) * 100);
        inside.setChecked(value["inside"].toBool());
        inside.setVisible(current == "stroke");
        distance.setEnabled(shadow);
        angle.setEnabled(shadow);
        size.setEnabled(current != "colorOverlay");
        swatch.setStyleSheet("background:" + color.name() + ";color:" + (color.lightness() < 128 ? "white" : "black") +
                             ";");
        loading = false;
        update();
    };
    connect(&kind, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [&] {
        store();
        current = kind.currentData().toString();
        load();
    });
    for (auto *box : {&size, &distance, &angle, &opacity})
        connect(box, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, [&] {
            if (!loading)
                update();
        });
    connect(&enabled, &QCheckBox::toggled, &dialog, [&] {
        if (!loading)
            update();
    });
    connect(&inside, &QCheckBox::toggled, &dialog, [&] {
        if (!loading)
            update();
    });
    connect(&swatch, &QPushButton::clicked, &dialog, [&] {
        auto value = QColorDialog::getColor(color, &dialog, t("color"));
        if (value.isValid()) {
            color = value;
            swatch.setStyleSheet("background:" + color.name() + ";");
            update();
        }
    });
    load();
    if (dialog.exec() == QDialog::Accepted)
        invoke([&] {
            document_.edit([&](State &s) {
                if (settings.isEmpty())
                    s.layers[index].metadata.remove("effects");
                else
                    s.layers[index].metadata["effects"] = settings;
            });
        });
}
} // namespace comp
