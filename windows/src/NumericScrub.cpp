#include "NumericScrub.h"
#include <QApplication>
#include <QAbstractSpinBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QSlider>
#include <QLabel>
#include <QLayout>
#include <QFormLayout>
#include <QBoxLayout>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPointer>
#include <cmath>
#include <algorithm>
namespace comp {
namespace {
bool numeric(QWidget *widget) {
    return qobject_cast<QSpinBox *>(widget) || qobject_cast<QDoubleSpinBox *>(widget) ||
           qobject_cast<QSlider *>(widget);
}
class Scrub final : public QObject {
    QPointer<QWidget> field_;
    QPointer<QLabel> label_;
    QPointF previous_;
    double initial_ = 0, value_ = 0;
    bool active_ = false, moved_ = false;
    double value() const {
        if (auto *box = qobject_cast<QDoubleSpinBox *>(field_)) return box->value();
        if (auto *box = qobject_cast<QSpinBox *>(field_)) return box->value();
        if (auto *slider = qobject_cast<QSlider *>(field_)) return slider->value();
        return 0;
    }
    double step() const {
        if (auto *box = qobject_cast<QDoubleSpinBox *>(field_)) return box->singleStep();
        if (auto *box = qobject_cast<QSpinBox *>(field_)) return box->singleStep();
        if (auto *slider = qobject_cast<QSlider *>(field_)) return slider->singleStep();
        return 1;
    }
    void set(double v) {
        if (auto *box = qobject_cast<QDoubleSpinBox *>(field_)) {
            value_ = std::clamp(v, box->minimum(), box->maximum()); box->setValue(value_);
        } else if (auto *integer = qobject_cast<QSpinBox *>(field_)) {
            value_ = std::clamp(v, double(integer->minimum()), double(integer->maximum()));
            integer->setValue(int(std::round(value_)));
        } else if (auto *slider = qobject_cast<QSlider *>(field_)) {
            value_ = std::clamp(v, double(slider->minimum()), double(slider->maximum()));
            slider->setValue(int(std::round(value_)));
        }
    }
    void finish(bool cancel) {
        if (!active_) return;
        active_ = false;
        qApp->removeEventFilter(this);
        if (cancel) set(initial_);
        if (label_) label_->releaseMouse();
        if (field_ && moved_ && qobject_cast<QAbstractSpinBox *>(field_))
            QMetaObject::invokeMethod(field_, "editingFinished", Qt::DirectConnection);
    }
  public:
    Scrub(QLabel *label, QWidget *field) : QObject(label), field_(field), label_(label) {
        label->setProperty("numericScrub", true);
        label->setBuddy(field);
        label->setCursor(Qt::SizeHorCursor);
        label->installEventFilter(this);
    }
    bool eventFilter(QObject *object, QEvent *event) override {
        if (active_ && event->type() == QEvent::KeyPress &&
            static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            finish(true); return true;
        }
        if (active_ && (event->type() == QEvent::ApplicationDeactivate ||
                       (object == label_ && event->type() == QEvent::Hide))) finish(false);
        if (object != label_ || !field_ || !field_->isEnabled()) return false;
        if (auto *box = qobject_cast<QAbstractSpinBox *>(field_); box && box->isReadOnly()) return false;
        if (event->type() == QEvent::MouseButtonPress) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() != Qt::LeftButton) return false;
            active_ = true;
            qApp->installEventFilter(this);
            moved_ = false; previous_ = mouse->globalPosition();
            field_->setFocus(Qt::MouseFocusReason);
            initial_ = value_ = value();
            label_->grabMouse();
            return true;
        }
        if (active_ && event->type() == QEvent::MouseMove) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            const auto delta = mouse->globalPosition().x() - previous_.x();
            if (!moved_ && std::abs(delta) < 3) return true;
            moved_ = true;
            const double speed = mouse->modifiers().testFlag(Qt::ShiftModifier) ? .1 :
                                 mouse->modifiers().testFlag(Qt::ControlModifier) ? 10 : 1;
            set(value_ + delta * step() * speed * .25);
            previous_ = mouse->globalPosition();
            return true;
        }
        if (active_ && event->type() == QEvent::MouseButtonRelease &&
            static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
            finish(false); return true;
        }
        return false;
    }
};
void bind(QLabel *label, QWidget *field) {
    if (label && numeric(field) && !label->property("numericScrub").toBool())
        new Scrub(label, field);
}
void scanLayout(QLayout *layout) {
    if (!layout) return;
    if (auto *form = qobject_cast<QFormLayout *>(layout)) {
        for (int i = 0; i < form->rowCount(); ++i) {
            auto *label = form->itemAt(i, QFormLayout::LabelRole);
            auto *field = form->itemAt(i, QFormLayout::FieldRole);
            if (label && field) bind(qobject_cast<QLabel *>(label->widget()), field->widget());
        }
    }
    for (int i = 0; i < layout->count(); ++i) {
        auto *item = layout->itemAt(i);
        if (i + 1 < layout->count())
            bind(qobject_cast<QLabel *>(item->widget()), layout->itemAt(i + 1)->widget());
        if (item->layout()) scanLayout(item->layout());
    }
}
void scan(QWidget *widget) {
    scanLayout(widget->layout());
    for (auto *child : widget->findChildren<QWidget *>()) {
        scanLayout(child->layout());
        if (auto *label = qobject_cast<QLabel *>(child)) bind(label, label->buddy());
    }
}
class Binder final : public QObject {
    QPointer<QWidget> root_;
  public:
    explicit Binder(QWidget *root) : QObject(root), root_(root) { qApp->installEventFilter(this); scan(root); }
    bool eventFilter(QObject *object, QEvent *event) override {
        if (event->type() == QEvent::Show) {
            auto *widget = qobject_cast<QWidget *>(object);
            if (widget && root_ && (widget == root_ || root_->isAncestorOf(widget))) scan(widget);
        }
        return false;
    }
};
}
void installNumericScrubbing(QWidget *root) {
    if (!root->property("numericScrubInstalled").toBool()) {
        root->setProperty("numericScrubInstalled", true);
        new Binder(root);
    }
}
}
