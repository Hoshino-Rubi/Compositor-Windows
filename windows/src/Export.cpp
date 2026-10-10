#include "Export.h"
#include "Renderer.h"
#include <QImageWriter>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QSaveFile>
namespace comp {
void exportDocument(const State &state, const QString &path, const QByteArray &format, QSize size, int quality) {
    if (!size.isValid() || size.width() > MaxSide || size.height() > MaxSide ||
        qint64(size.width()) * size.height() > MaxCanvasPixels)
        throw Error("invalidSize");
    if (format != "png" && format != "jpeg" && format != "pdf")
        throw Error("unsupportedFeature", "export format");
    auto image = render(state, size);
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly))
        throw Error("fileError");
    if (format == "pdf") {
        // One raster-composited page, sized by document DPI, with no printer margins.
        QPdfWriter pdf(&out);
        pdf.setResolution(qRound(state.resolution));
        pdf.setPageSize(QPageSize(QSizeF(size.width() * 72. / state.resolution, size.height() * 72. / state.resolution),
                                  QPageSize::Point));
        pdf.setPageMargins(QMarginsF(0, 0, 0, 0), QPageLayout::Point);
        pdf.setCreator("Compositor for Windows");
        QPainter painter(&pdf);
        if (!painter.isActive())
            throw Error("fileError");
        painter.fillRect(QRect(0, 0, pdf.width(), pdf.height()), Qt::white);
        painter.drawImage(QRect(0, 0, pdf.width(), pdf.height()), image);
        if (!painter.end())
            throw Error("fileError");
    } else {
        if (format == "jpeg") {
            QImage white(size, QImage::Format_RGB32);
            white.fill(Qt::white);
            QPainter p(&white);
            p.drawImage(0, 0, image);
            p.end();
            image = white;
        }
        image.setDotsPerMeterX(qRound(state.resolution / .0254));
        image.setDotsPerMeterY(qRound(state.resolution / .0254));
        QImageWriter writer(&out, format);
        writer.setQuality(quality);
        if (!writer.write(image))
            throw Error("fileError", writer.errorString());
    }
    if (!out.commit())
        throw Error("fileError");
}
} // namespace comp
