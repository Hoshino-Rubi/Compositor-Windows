#include "Icons.h"
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
namespace comp {
class VectorIcon final : public QIconEngine {
    QString name_;

  public:
    explicit VectorIcon(QString name) : name_(std::move(name)) {}
    QIconEngine *clone() const override { return new VectorIcon(name_); }
    void paint(QPainter *p, const QRect &rect, QIcon::Mode mode, QIcon::State) override {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const double size = qMin(rect.width(), rect.height());
        p->translate(rect.center().x() - size / 2, rect.center().y() - size / 2);
        p->scale(size / 24., size / 24.);
        QColor c = mode == QIcon::Disabled ? QColor("#636363") : QColor("#dedede");
        p->setPen(QPen(c, 1.65, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p->setBrush(Qt::NoBrush);
        auto line = [&](double x, double y, double x2, double y2) { p->drawLine(QPointF(x, y), QPointF(x2, y2)); };
        auto path = [&](std::initializer_list<QPointF> points, bool close = false) {
            QPainterPath s;
            bool first = true;
            for (auto point : points) {
                if (first)
                    s.moveTo(point);
                else
                    s.lineTo(point);
                first = false;
            }
            if (close)
                s.closeSubpath();
            p->drawPath(s);
        };
        const auto &n = name_;
        if (n == "move") {
            path({{4, 3}, {4, 18}, {8, 14}, {12, 21}, {15, 19}, {11, 12}, {17, 12}}, true);
        } else if (n == "marquee") {
            p->setPen(QPen(c, 1.7, Qt::DashLine));
            p->drawRoundedRect(QRectF(3, 4, 18, 16), 1, 1);
        } else if (n == "lasso") {
            p->drawEllipse(QRectF(3, 3, 18, 12));
            p->drawEllipse(QRectF(11, 12, 5, 5));
            QPainterPath s;
            s.moveTo(14, 16);
            s.cubicTo(19, 23, 7, 24, 6, 19);
            p->drawPath(s);
        } else if (n == "wand") {
            path({{4, 21}, {16, 9}, {13, 6}, {1, 18}}, true);
            line(18, 2, 18, 6);
            line(16, 4, 20, 4);
            line(21, 10, 23, 10);
            line(6, 3, 6, 7);
            line(4, 5, 8, 5);
        } else if (n == "brush") {
            path({{9, 14}, {18, 3}, {21, 6}, {12, 17}}, true);
            QPainterPath s;
            s.moveTo(9, 14);
            s.cubicTo(3, 13, 7, 20, 2, 21);
            s.cubicTo(9, 23, 13, 20, 12, 17);
            p->drawPath(s);
        } else if (n == "eraser") {
            path({{3, 14}, {13, 4}, {21, 12}, {12, 21}, {9, 21}}, true);
            line(8, 9, 16, 17);
            line(12, 21, 22, 21);
        } else if (n == "healing") {
            p->translate(12, 12);
            p->rotate(-45);
            p->drawRoundedRect(QRectF(-5, -11, 10, 22), 4, 4);
            p->drawRect(QRectF(-4, -4, 8, 8));
            p->drawPoint(0, -7);
            p->drawPoint(0, 7);
        } else if (n == "clone") {
            p->drawEllipse(QRectF(9, 2, 6, 7));
            line(10, 9, 9, 14);
            line(14, 9, 15, 14);
            p->drawRoundedRect(QRectF(4, 14, 16, 5), 1, 1);
            line(4, 22, 20, 22);
        } else if (n == "blur") {
            QPainterPath s;
            s.moveTo(12, 2);
            s.cubicTo(10, 7, 4, 11, 4, 15);
            s.cubicTo(4, 25, 20, 25, 20, 15);
            s.cubicTo(20, 11, 14, 6, 12, 2);
            s.closeSubpath();
            p->drawPath(s);
            line(8, 16, 10, 19);
        } else if (n == "gradient") {
            QLinearGradient g(3, 3, 21, 21);
            g.setColorAt(0, c);
            g.setColorAt(1, QColor("#363636"));
            p->setBrush(g);
            p->drawRoundedRect(QRectF(3, 3, 18, 18), 1, 1);
        } else if (n == "rectangle" || n == "addLayer") {
            p->drawRect(QRectF(3, 3, 18, 18));
            if (n == "addLayer") {
                line(7, 12, 17, 12);
                line(12, 7, 12, 17);
            }
        } else if (n == "ellipse")
            p->drawEllipse(QRectF(3, 3, 18, 18));
        else if (n == "line")
            line(4, 20, 20, 4);
        else if (n == "text") {
            line(4, 4, 20, 4);
            line(12, 4, 12, 21);
            line(8, 21, 16, 21);
            line(4, 4, 4, 7);
            line(20, 4, 20, 7);
        } else if (n == "eyedropper") {
            path({{14, 4}, {20, 10}, {11, 19}, {4, 20}, {5, 13}}, true);
            line(12, 3, 21, 12);
            line(3, 21, 5, 19);
        } else if (n == "crop") {
            path({{6, 2}, {6, 18}, {22, 18}});
            path({{2, 6}, {18, 6}, {18, 22}});
            line(8, 16, 21, 3);
        } else if (n == "hand") {
            QPainterPath s;
            s.moveTo(8, 12);
            s.lineTo(8, 5);
            s.cubicTo(8, 2, 11, 2, 11, 5);
            s.lineTo(11, 11);
            s.lineTo(11, 3);
            s.cubicTo(11, 0, 14, 0, 14, 3);
            s.lineTo(14, 11);
            s.lineTo(14, 5);
            s.cubicTo(14, 2, 17, 2, 17, 5);
            s.lineTo(17, 12);
            s.lineTo(17, 8);
            s.cubicTo(17, 5, 20, 5, 20, 8);
            s.lineTo(20, 15);
            s.cubicTo(20, 25, 11, 25, 8, 21);
            s.lineTo(3, 14);
            s.cubicTo(1, 10, 4, 9, 6, 12);
            s.lineTo(8, 14);
            p->drawPath(s);
        } else if (n == "zoom" || n == "zoomIn" || n == "zoomOut") {
            p->drawEllipse(QRectF(3, 3, 13, 13));
            line(15, 15, 22, 22);
            if (n != "zoom") {
                line(6, 9.5, 13, 9.5);
                if (n == "zoomIn")
                    line(9.5, 6, 9.5, 13);
            }
        } else if (n == "addGroup" || n == "group") {
            path({{2, 6}, {9, 6}, {11, 9}, {22, 9}, {22, 21}, {2, 21}}, true);
        } else if (n == "mask" || n == "addMask") {
            p->setBrush(c);
            p->drawRect(QRectF(3, 4, 18, 16));
            p->setBrush(QColor("#303030"));
            p->setPen(Qt::NoPen);
            p->drawEllipse(QRectF(8, 8, 8, 8));
        } else if (n == "adjustments") {
            p->drawEllipse(QRectF(3, 3, 18, 18));
            p->setBrush(c);
            p->drawChord(QRectF(3, 3, 18, 18), 90 * 16, 180 * 16);
        } else if (n == "effects") {
            path({{12, 2}, {14, 9}, {21, 12}, {14, 15}, {12, 22}, {10, 15}, {3, 12}, {10, 9}}, true);
        } else if (n == "delete") {
            p->drawRoundedRect(QRectF(6, 7, 12, 15), 1, 1);
            line(4, 6, 20, 6);
            line(9, 2, 15, 2);
            line(10, 10, 10, 18);
            line(14, 10, 14, 18);
        } else if (n == "duplicate") {
            p->drawRect(QRectF(8, 8, 13, 13));
            path({{5, 16}, {3, 16}, {3, 3}, {16, 3}, {16, 5}});
        } else if (n == "moveUp" || n == "moveDown") {
            if (n == "moveDown") {
                p->translate(24, 24);
                p->rotate(180);
            }
            line(12, 21, 12, 3);
            path({{5, 10}, {12, 3}, {19, 10}});
        } else if (n == "flipX" || n == "flipY") {
            if (n == "flipY") {
                p->translate(24, 0);
                p->rotate(90);
            }
            p->setPen(QPen(c, 1.4, Qt::DashLine));
            line(12, 2, 12, 22);
            p->setPen(QPen(c, 1.6));
            path({{3, 18}, {9, 7}, {9, 18}}, true);
            path({{21, 18}, {15, 7}, {15, 18}}, true);
        } else if (n == "open") {
            path({{2, 19}, {2, 5}, {9, 5}, {11, 8}, {21, 8}, {21, 11}});
            path({{2, 20}, {6, 11}, {23, 11}, {19, 20}}, true);
        } else if (n == "save") {
            path({{3, 3}, {18, 3}, {21, 6}, {21, 21}, {3, 21}}, true);
            p->drawRect(QRectF(7, 3, 9, 6));
            p->drawRect(QRectF(7, 14, 10, 7));
        } else if (n == "close") {
            line(6, 6, 18, 18);
            line(18, 6, 6, 18);
        } else if (n == "new") {
            line(12, 4, 12, 20);
            line(4, 12, 20, 12);
        } else if (n == "eye") {
            QPainterPath s;
            s.moveTo(2, 12);
            s.cubicTo(7, 3, 17, 3, 22, 12);
            s.cubicTo(17, 21, 7, 21, 2, 12);
            p->drawPath(s);
            p->drawEllipse(QPointF(12, 12), 3, 3);
        } else {
            p->drawRoundedRect(QRectF(4, 4, 16, 16), 3, 3);
            line(8, 12, 16, 12);
        }
        p->restore();
    }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        QPixmap result(size);
        result.fill(Qt::transparent);
        QPainter p(&result);
        paint(&p, QRect(QPoint(), size), mode, state);
        return result;
    }
    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
        auto result = pixmap(size * scale, mode, state);
        result.setDevicePixelRatio(scale);
        return result;
    }
};
QIcon icon(const QString &name) {
    return QIcon(new VectorIcon(name));
}
} // namespace comp
