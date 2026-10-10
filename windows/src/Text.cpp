#include "Text.h"
#include <QFont>
#include <QPainter>
#include <QTextCursor>
#include <QTextBlock>
#include <QTextFragment>
namespace comp {
void loadText(QTextDocument &document, const QJsonObject &style) {
    QFont font(style["fontName"].toString("Arial"));
    font.setPixelSize(style["fontSize"].toInt(72));
    font.setLetterSpacing(QFont::AbsoluteSpacing, style["tracking"].toDouble());
    document.setDefaultFont(font);
    document.setDocumentMargin(12);
    document.setPlainText(style["content"].toString());
    QTextCursor cursor(&document);
    cursor.select(QTextCursor::Document);
    QTextCharFormat color;
    color.setForeground(QColor::fromRgbF(style["red"].toDouble(), style["green"].toDouble(), style["blue"].toDouble()));
    cursor.mergeCharFormat(color);
    QTextBlockFormat block;
    block.setAlignment(style["alignment"] == "Center"  ? Qt::AlignHCenter
                       : style["alignment"] == "Right" ? Qt::AlignRight
                                                       : Qt::AlignLeft);
    if (style["leading"].toDouble() > 0)
        block.setLineHeight(style["leading"].toDouble(), QTextBlockFormat::FixedHeight);
    cursor.mergeBlockFormat(block);
    auto runs = [&](const QJsonArray &array, bool colors) {
        for (const auto &v : array) {
            auto r = v.toObject();
            int start = r["location"].toInt(), length = r["length"].toInt();
            if (start < 0 || length < 1 || start + length > document.toPlainText().size())
                continue;
            QTextCursor c(&document);
            c.setPosition(start);
            c.setPosition(start + length, QTextCursor::KeepAnchor);
            QTextCharFormat f;
            if (colors)
                f.setForeground(QColor::fromRgbF(r["red"].toDouble(), r["green"].toDouble(), r["blue"].toDouble()));
            else
                f.setFontFamilies({r["fontName"].toString()});
            c.mergeCharFormat(f);
        }
    };
    runs(style["colorRuns"].toArray(), true);
    runs(style["fontRuns"].toArray(), false);
    auto box = style["boxSize"].toArray();
    if (box.size() == 2)
        document.setTextWidth(box[0].toDouble());
    else
        document.adjustSize();
}
QJsonObject textStyle(const QTextDocument &document, QJsonObject base) {
    base["content"] = document.toPlainText();
    QJsonArray colors, fonts;
    const auto baseColor = QColor::fromRgbF(base["red"].toDouble(), base["green"].toDouble(), base["blue"].toDouble());
    for (auto block = document.begin(); block != document.end(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            auto f = it.fragment();
            if (!f.isValid())
                continue;
            auto format = f.charFormat();
            QColor color = format.foreground().color();
            const int start = f.position(), length = f.length();
            if (color.isValid() && color != baseColor)
                colors.append(QJsonObject{{"location", start},
                                          {"length", length},
                                          {"red", color.redF()},
                                          {"green", color.greenF()},
                                          {"blue", color.blueF()}});
            QString face = format.fontFamilies().toStringList().value(0, document.defaultFont().family());
            if (!face.isEmpty() && face != base["fontName"].toString())
                fonts.append(QJsonObject{{"location", start}, {"length", length}, {"fontName", face}});
        }
    if (colors.isEmpty())
        base.remove("colorRuns");
    else
        base["colorRuns"] = colors;
    if (fonts.isEmpty())
        base.remove("fontRuns");
    else
        base["fontRuns"] = fonts;
    return base;
}
QImage renderText(const QJsonObject &style) {
    QTextDocument document;
    loadText(document, style);
    QSize size = document.size().toSize();
    auto box = style["boxSize"].toArray();
    if (box.size() == 2)
        size = {qRound(box[0].toDouble()), qRound(box[1].toDouble())};
    size.setWidth(std::max(24, size.width()));
    size.setHeight(std::max(24, size.height()));
    if (size.width() > MaxSide || size.height() > MaxSide || qint64(size.width()) * size.height() > MaxSourcePixels)
        throw Error("tooLarge");
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull())
        throw Error("tooLarge");
    image.fill(Qt::transparent);
    QPainter p(&image);
    document.drawContents(&p);
    return image;
}
} // namespace comp
