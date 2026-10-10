#include "MainWindow.h"
#include "Text.h"
#include <QDialog>
#include <QApplication>
#include <QDialogButtonBox>
#include <QTextEdit>
#include <QFontComboBox>
#include <QColorDialog>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QPainter>
#include <QTextDocument>
#include <QTextBlockFormat>
#include <QTextCursor>
namespace comp {
void MainWindow::textDialog(QPointF point, QString id) {
    if (busy_)
        return;
    const int index = document_.state().index(id);
    const QJsonObject original =
        index >= 0 ? document_.state().layers[index].metadata["text"].toObject() : QJsonObject();
    QDialog dialog(this);
    dialog.setObjectName("textDialog");
    dialog.setWindowTitle(t(index >= 0 ? "editText" : "textTitle"));
    auto *outer = new QVBoxLayout(&dialog);
    QTextEdit text;
    text.setObjectName("textContent");
    text.setAcceptRichText(false);
    QJsonObject initial = original;
    if (initial.isEmpty()) {
        initial = {{"content", t("textPlaceholder")},
                   {"fontName", QApplication::font().family()},
                   {"fontSize", 72},
                   {"red", canvas_->color().redF()},
                   {"green", canvas_->color().greenF()},
                   {"blue", canvas_->color().blueF()},
                   {"alignment", "Left"},
                   {"tracking", 0},
                   {"leading", 0}};
    }
    loadText(*text.document(), initial);
    text.setMinimumSize(480, 180);
    outer->addWidget(&text);
    auto *form = new QFormLayout;
    QFontComboBox font;
    font.setCurrentFont(QFont(original["fontName"].toString(QApplication::font().family())));
    QSpinBox size;
    size.setObjectName("textFontSize");
    size.setRange(1, 2000);
    size.setValue(original["fontSize"].toInt(72));
    connect(&font, &QFontComboBox::currentFontChanged, &dialog, [&](QFont face) {
        QTextCharFormat fmt;
        fmt.setFontFamilies({face.family()});
        auto cursor = text.textCursor();
        if (!cursor.hasSelection())
            cursor.select(QTextCursor::Document);
        cursor.mergeCharFormat(fmt);
    });
    QDoubleSpinBox leading, tracking;
    leading.setRange(0, 5000);
    leading.setValue(original["leading"].toDouble());
    tracking.setRange(-100, 1000);
    tracking.setValue(original["tracking"].toDouble());
    QComboBox alignment;
    alignment.addItem(t("left"), "Left");
    alignment.addItem(t("center"), "Center");
    alignment.addItem(t("right"), "Right");
    alignment.setCurrentIndex(std::max(0, alignment.findData(original["alignment"].toString("Left"))));
    QColor color = index >= 0 ? QColor::fromRgbF(original["red"].toDouble(), original["green"].toDouble(),
                                                 original["blue"].toDouble())
                              : canvas_->color();
    QPushButton swatch(t("fontColor"));
    auto sync = [&] {
        swatch.setStyleSheet("background:" + color.name() + ";color:" + (color.lightness() < 128 ? "white" : "black") +
                             ";");
    };
    sync();
    connect(&swatch, &QPushButton::clicked, &dialog, [&] {
        auto selected = QColorDialog::getColor(color, &dialog, t("fontColor"));
        if (selected.isValid()) {
            if (text.textCursor().hasSelection()) {
                QTextCharFormat fmt;
                fmt.setForeground(selected);
                text.textCursor().mergeCharFormat(fmt);
            } else {
                color = selected;
                QTextCursor cursor(text.document());
                cursor.select(QTextCursor::Document);
                QTextCharFormat fmt;
                fmt.setForeground(color);
                cursor.mergeCharFormat(fmt);
            }
            sync();
        }
    });
    form->addRow(t("font"), &font);
    form->addRow(t("fontSize"), &size);
    form->addRow(t("alignment"), &alignment);
    form->addRow(t("leading"), &leading);
    form->addRow(t("tracking"), &tracking);
    form->addRow(&swatch);
    outer->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted || text.toPlainText().trimmed().isEmpty())
        return;
    invoke([&] {
        QFont face = font.currentFont();
        face.setPixelSize(size.value());
        face.setLetterSpacing(QFont::AbsoluteSpacing, tracking.value());
        QTextDocument layout;
        layout.setDefaultFont(face);
        layout.setDocumentMargin(12);
        layout.setPlainText(text.toPlainText());
        QTextCursor cursor(&layout);
        cursor.select(QTextCursor::Document);
        QTextCharFormat ink;
        ink.setForeground(color);
        cursor.mergeCharFormat(ink);
        QTextBlockFormat block;
        block.setAlignment(alignment.currentIndex() == 1   ? Qt::AlignHCenter
                           : alignment.currentIndex() == 2 ? Qt::AlignRight
                                                           : Qt::AlignLeft);
        if (leading.value() > 0)
            block.setLineHeight(leading.value(), QTextBlockFormat::FixedHeight);
        cursor.mergeBlockFormat(block);
        layout.adjustSize();
        QSize bounds = layout.size().toSize();
        bounds.setWidth(std::max(24, bounds.width()));
        bounds.setHeight(std::max(24, bounds.height()));
        if (bounds.width() > MaxSide || bounds.height() > MaxSide ||
            qint64(bounds.width()) * bounds.height() + document_.state().sourcePixels() > MaxSourcePixels)
            throw Error("tooLarge");
        QImage image(bounds, QImage::Format_ARGB32_Premultiplied);
        if (image.isNull())
            throw Error("tooLarge");
        image.fill(Qt::transparent);
        QPainter painter(&image);
        layout.drawContents(&painter);
        painter.end();
        QJsonObject style = initial;
        const QJsonObject edited{{"content", text.toPlainText()},
                                 {"fontName", face.family()},
                                 {"fontSize", size.value()},
                                 {"red", color.redF()},
                                 {"green", color.greenF()},
                                 {"blue", color.blueF()},
                                 {"alignment", alignment.currentData().toString()},
                                 {"leading", leading.value()},
                                 {"tracking", tracking.value()}};
        for (auto it = edited.begin(); it != edited.end(); ++it)
            style[it.key()] = it.value();
        style = textStyle(*text.document(), style);
        image = renderText(style);
        bounds = image.size();
        document_.edit([&](State &s) {
            if (index >= 0) {
                auto &l = s.layers[index];
                const QSize old = l.image.size();
                l.image = image;
                l.originalPng.clear();
                l.name = text.toPlainText().split('\n').first().left(60);
                l.placement.size = QSizeF(bounds.width() * l.placement.size.width() / std::max(1, old.width()),
                                          bounds.height() * l.placement.size.height() / std::max(1, old.height()));
                l.metadata["text"] = style;
            } else {
                Layer l;
                l.id = newId();
                l.name = text.toPlainText().split('\n').first().left(60);
                l.image = image;
                l.placement.size = bounds;
                l.placement.origin = point;
                l.metadata["text"] = style;
                const int i = s.index(s.active);
                if (i >= 0)
                    l.parent = s.layers[i].group ? s.layers[i].id : s.layers[i].parent;
                s.layers.append(l);
                s.active = l.id;
            }
        });
    });
}
} // namespace comp
