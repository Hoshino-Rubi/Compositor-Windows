#include "MainWindow.h"
#include "Filters.h"
#include "SpatialFilters.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QMouseEvent>
#include <QPainter>
namespace comp {
class ProgressivePreview final : public QWidget {
  public:
    QImage image;
    QPointF start{.25, .5}, end{.75, .5};
    bool radial = false;
    std::function<void()> changed;
    QRectF content() const {
        const QSize size = image.size().scaled(this->size(), Qt::KeepAspectRatio);
        return QRectF(QPointF((width() - size.width()) / 2., (height() - size.height()) / 2.), size);
    }

  protected:
    bool draggingStart = false;
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), QColor("#181818"));
        const auto box = content();
        p.drawImage(box, image);
        p.setRenderHint(QPainter::Antialiasing);
        auto point = [&](QPointF v) {
            return box.topLeft() + QPointF(v.x() * box.width(), v.y() * box.height());
        };
        const auto a = point(start), b = point(end);
        p.setPen(QPen(Qt::black, 3));
        p.drawLine(a, b);
        p.setPen(QPen(Qt::white, 1));
        p.drawLine(a, b);
        if (radial) {
            const double radius = QLineF(a, b).length();
            p.drawEllipse(a, radius, radius);
        }
        p.setBrush(Qt::white);
        p.setPen(QPen(Qt::black, 1));
        p.drawEllipse(a, 5, 5);
        p.drawEllipse(b, 5, 5);
    }
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() != Qt::LeftButton)
            return;
        const auto box = content();
        QPointF v((event->position().x() - box.x()) / box.width(),
                  (event->position().y() - box.y()) / box.height());
        draggingStart = QLineF(v, start).length() < QLineF(v, end).length();
        mouseMoveEvent(event);
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        if (!event->buttons().testFlag(Qt::LeftButton))
            return;
        const auto box = content();
        QPointF v(std::clamp((event->position().x() - box.x()) / box.width(), 0., 1.),
                  std::clamp((event->position().y() - box.y()) / box.height(), 0., 1.));
        (draggingStart ? start : end) = v;
        update();
        if (changed)
            changed();
    }
};
void MainWindow::progressiveBlurDialog() {
    const int index = document_.state().index(document_.state().active);
    if (index < 0 || document_.state().layers[index].image.isNull())
        return;
    QDialog dialog(this);
    dialog.setObjectName("progressiveBlurDialog");
    dialog.setWindowTitle(t("filter.progressiveBlur"));
    dialog.resize(950, 650);
    auto *outer = new QVBoxLayout(&dialog);
    auto *row = new QHBoxLayout;
    ProgressivePreview preview;
    preview.setMinimumSize(600, 400);
    row->addWidget(&preview, 1);
    const auto &layer = document_.state().layers[index];
    const auto full = mask_->isChecked() ? layer.mask : layer.image;
    if (full.isNull()) return;
    const auto source = full.width() > 800 || full.height() > 600
                            ? full.scaled(800, 600, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                            : full;
    preview.image = source;
    auto *form = new QFormLayout;
    QDoubleSpinBox from, to;
    from.setRange(0, 250);
    to.setRange(0, 250);
    to.setValue(20);
    from.setObjectName("progressiveFrom");
    to.setObjectName("progressiveTo");
    form->addRow(t("progressiveFrom"), &from);
    form->addRow(t("progressiveTo"), &to);
    QCheckBox radial(t("progressiveRadial")), invert(t("progressiveInvert")), live(t("preview"));
    live.setChecked(true);
    form->addRow(&radial);
    form->addRow(&invert);
    form->addRow(&live);
    row->addLayout(form);
    outer->addLayout(row, 1);
    QLabel hint(t("progressiveHint"));
    hint.setWordWrap(true);
    hint.setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    outer->addWidget(&hint);
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    outer->addWidget(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto settings = [&] {
        return QJsonObject{{"kind", "Progressive Blur"},
                           {"progressiveFrom", from.value()},
                           {"progressiveTo", to.value()},
                           {"progressiveStartX", preview.start.x()},
                           {"progressiveStartY", preview.start.y()},
                           {"progressiveEndX", preview.end.x()},
                           {"progressiveEndY", preview.end.y()},
                           {"progressiveRadial", radial.isChecked()},
                           {"progressiveInvert", invert.isChecked()}};
    };
    QTimer timer;
    timer.setSingleShot(true);
    timer.setInterval(60);
    connect(&timer, &QTimer::timeout, &dialog, [&] {
        try {
            auto s = settings();
            const double scale = double(source.width()) / full.width();
            s["progressiveFrom"] = from.value() * scale;
            s["progressiveTo"] = to.value() * scale;
            preview.image = live.isChecked() ? progressiveBlur(source, s) : source;
            preview.radial = radial.isChecked();
            preview.update();
        } catch (const Error &e) {
            showError(e.key, e.detail);
        }
    });
    preview.changed = [&] { timer.start(); };
    for (auto *box : {&from, &to})
        connect(box, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, [&] { timer.start(); });
    for (auto *box : {&radial, &invert, &live})
        connect(box, &QCheckBox::toggled, &dialog, [&] { timer.start(); });
    timer.start();
    if (dialog.exec() != QDialog::Accepted)
        return;
    timer.stop();
    const auto result = settings();
    invoke([&] {
        document_.applyFilter(result, mask_->isChecked());
        lastFilter_ = result;
    });
}
} // namespace comp
