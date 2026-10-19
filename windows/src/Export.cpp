#include "Export.h"
#include "Cmyk.h"
#include "GpuPreview.h"
#include "Pixels.h"
#include "PreviewRenderer.h"
#include "Renderer.h"
#include <QColorSpace>
#include <QImageWriter>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QSaveFile>
#include <algorithm>
#include <cmath>
#include <cstring>
namespace comp {
namespace {

void validateExportSize(QSize size) {
    if (!size.isValid() || size.width() > MaxSide || size.height() > MaxSide ||
        qint64(size.width()) * size.height() > MaxCanvasPixels)
        throw Error("invalidSize");
}

bool globalAdjustment(const State &state) {
    for (const auto &layer : state.layers) {
        const auto kind = layer.metadata["adjustment"].toObject()["kind"].toString();
        // These kernels depend on full-image coordinates or a rotated image
        // grid.  Rendering one tile at a time would change their result.
        if (QStringList{"Add Noise", "Grain", "Motion Blur", "Camera Raw", "Vignette", "Bloom / Glow",
                        "Dither", "Tonal Contrast", "Lens Correction", "Scanlines"}
                .contains(kind))
            return true;
    }
    return false;
}

QPointF scaledPoint(QPointF point, double sx, double sy) {
    return {point.x() * sx, point.y() * sy};
}

void scalePlacement(Placement &placement, double sx, double sy) {
    placement.origin = scaledPoint(placement.origin, sx, sy);
    placement.size = {placement.size.width() * sx, placement.size.height() * sy};
}

void scaleValue(QJsonObject &object, const char *key, double scale) {
    if (object.contains(key)) object[key] = object[key].toDouble() * scale;
}

void scaleEffects(QJsonObject &effects, double scale) {
    for (auto it = effects.begin(); it != effects.end(); ++it) {
        if (!it.value().isObject()) continue;
        auto effect = it.value().toObject();
        if (it.key() == "stroke") {
            scaleValue(effect, "size", scale);
        } else if (it.key() == "shadow" || it.key() == "innerShadow") {
            scaleValue(effect, "distance", scale);
            scaleValue(effect, "blur", scale);
        } else if (it.key() == "outerGlow" || it.key() == "innerGlow") {
            scaleValue(effect, "size", scale);
        }
        it.value() = effect;
    }
}

// Renderer::render already applies the output scale to placements.  For a
// tiled render, however, each tile is a local State whose output scale is one;
// carry that scale into placements and spatial effect parameters first.
State scaledState(const State &source, QSize target) {
    State result = source;
    const double sx = double(target.width()) / source.size.width();
    const double sy = double(target.height()) / source.size.height();
    result.size = target;
    result.id += "/export/" + QString::number(target.width()) + "x" + QString::number(target.height());
    for (auto &layer : result.layers) {
        scalePlacement(layer.placement, sx, sy);
        scalePlacement(layer.maskPlacement, sx, sy);
        if (layer.metadata["adjustment"].isObject()) {
            auto adjustment = layer.metadata["adjustment"].toObject();
            if (adjustment["kind"] == "Gaussian Blur")
                scaleValue(adjustment, "blurRadius", sx);
            layer.metadata["adjustment"] = adjustment;
        }
        auto effects = layer.metadata["effects"].toObject();
        if (!effects.isEmpty()) {
            scaleEffects(effects, sx);
            layer.metadata["effects"] = effects;
        }
    }
    return result;
}

// Keep the same halo policy as PreviewRenderer::renderRegion.  The helper is
// local because export must call renderGpuPreview directly even for a small
// edge tile; renderPreview intentionally routes tiny requests to the CPU.
int tilePadding(const State &state) {
    int total = 2;
    const auto blur = [](double radius) {
        return radius <= 0 ? 0 : 3 * std::clamp(qRound(radius * .58), 1, 250);
    };
    for (const auto &layer : state.layers) {
        const auto adjustment = layer.metadata["adjustment"].toObject();
        if (adjustment["kind"] == "Gaussian Blur")
            total += blur(adjustment["blurRadius"].toDouble(10));
        const auto effects = layer.metadata["effects"].toObject();
        int extent = 0;
        for (auto it = effects.begin(); it != effects.end(); ++it) {
            const auto effect = it.value().toObject();
            if (!it.value().isObject() || !effect["enabled"].toBool(true)) continue;
            int radius = 0;
            if (it.key() == "shadow" || it.key() == "innerShadow")
                radius = blur(effect["blur"].toDouble(it.key() == "shadow" ? 20 : 10)) +
                         int(std::ceil(effect["distance"].toDouble(it.key() == "shadow" ? 20 : 10))) + 2;
            else if (it.key() == "outerGlow" || it.key() == "innerGlow")
                radius = blur(effect["size"].toDouble(20)) + 2;
            else if (it.key() == "stroke")
                radius = int(std::ceil(effect["size"].toDouble(4))) + 2;
            extent = std::max(extent, radius);
        }
        total = std::min(MaxSide, total + extent);
    }
    return total;
}

QImage renderGpuRegion(const State &state, QRect region) {
    region = region.intersected(QRect(QPoint(), state.size));
    if (region.isEmpty()) return {};
    const int halo = tilePadding(state);
    const QRect work = region.adjusted(-halo, -halo, halo, halo).intersected(QRect(QPoint(), state.size));
    State local = state;
    local.size = work.size();
    local.id += "/tile/" + QString::number(work.x()) + "," + QString::number(work.y());
    for (auto &layer : local.layers) {
        layer.placement.origin -= work.topLeft();
        layer.maskPlacement.origin -= work.topLeft();
    }
    const auto image = renderGpuPreview(local, work.size(), work.topLeft());
    if (image.isNull()) return {};
    return image.copy(region.translated(-work.topLeft()));
}

void copyTile(QImage &destination, const QImage &tile, QPoint topLeft) {
    if (tile.format() == destination.format()) {
        const int bytes = tile.depth() / 8;
        for (int y = 0; y < tile.height(); ++y)
            std::memcpy(destination.scanLine(topLeft.y() + y) + topLeft.x() * bytes,
                        tile.constScanLine(y), size_t(tile.width()) * bytes);
        return;
    }
    // GPU readback and the staging image normally use the document format;
    // retain a safe Qt conversion for a backend that returns an equivalent
    // premultiplied format with a different enum.
    QPainter painter(&destination);
    painter.drawImage(topLeft, tile);
}

bool exactTilePlacement(const Placement &p, QSize pixels) {
    auto whole=[](double x){return std::abs(x-std::round(x))<1e-9;};
    // Qt's transformed image edge rasterizer is not invariant under tile
    // translation at fractional/rotated boundaries. Do not change reference
    // export pixels merely to lower peak memory.
    return p.rotation==0 && !p.flipX && !p.flipY && whole(p.origin.x()) && whole(p.origin.y()) &&
           whole(p.size.width()) && whole(p.size.height()) &&
           (pixels==QSize(1,1) || p.size==QSizeF(pixels));
}

QImage renderTiledExport(const State &state, QSize target, bool accelerated) {
    if (qint64(target.width()) * target.height() <= 8 * 1024 * 1024) {
        // A full-size request retains the renderer's native scaling and can
        // safely run full-image-dependent adjustments (noise, Camera Raw,
        // vignette, and similar kernels) while it fits the GPU limit.
        const auto image = accelerated ? renderGpuPreview(state, target) : QImage();
        if (!image.isNull()) return image;
        return render(state, target);
    }

    // Full-image-dependent kernels must see the whole output.  Rendering them
    // per tile would change coordinates/statistics, so large exports remain a
    // clearly conservative reference-CPU route.
    if (globalAdjustment(state)) return render(state, target);

    // Placement scaling is represented by origin/size.  For a non-uniform
    // resize a rotated placement would require carrying the complete affine
    // matrix (including shear); reject tiled GPU export rather than silently
    // changing its sampling geometry.
    const double sx = double(target.width()) / state.size.width();
    const double sy = double(target.height()) / state.size.height();
    if (!qFuzzyCompare(sx, sy)) return render(state, target);

    const State scaled = scaledState(state, target);
    for (const auto &layer : scaled.layers) {
        if (!layer.visible || layer.opacity<=0) continue;
        if (!layer.image.isNull() && !exactTilePlacement(layer.placement,layer.image.size()))
            return render(state,target);
        if (!layer.mask.isNull() && layer.maskEnabled &&
            !exactTilePlacement(layer.maskLinked?layer.placement:layer.maskPlacement,layer.mask.size()))
            return render(state,target);
    }
    constexpr int tileSize = 512;

    QImage result(target, pixelFormat(state.bitDepth));
    if (result.isNull()) throw Error("tooLarge");
    result.fill(Qt::transparent);
    result.setColorSpace(QColorSpace::SRgb);
    result.setDotsPerMeterX(qRound(state.resolution / .0254));
    result.setDotsPerMeterY(qRound(state.resolution / .0254));
    const QRect bounds(QPoint(), target);
    for (int y = 0; y < target.height(); y += tileSize) {
        for (int x = 0; x < target.width(); x += tileSize) {
            const QRect tile(QPoint(x, y), QSize(tileSize, tileSize));
            const QRect region = tile.intersected(bounds);
            QImage image = accelerated ? renderGpuRegion(scaled, region) : QImage();
            if (image.isNull())
                image = renderRegion(scaled, region, false);
            if (image.isNull()) throw Error("tooLarge");
            copyTile(result, image, region.topLeft());
        }
    }
    return result;
}
} // namespace

QImage renderExportImage(const State &state, QSize size, ExportRenderer renderer) {
    validateExportSize(size);
    if (hasHdr(state)) return renderHdr(state, size);
    return renderTiledExport(state, size, renderer == ExportRenderer::AcceleratedGpu);
}

void exportDocument(const State &state, const QString &path, const QByteArray &format, QSize size, int quality,
                    ExportRenderer renderer, const QByteArray &cmykProfile) {
    validateExportSize(size);
    if (format != "png" && format != "jpeg" && format != "pdf" && format != "tiff" && format != "exr")
        throw Error("unsupportedFeature", "export format");
    auto image = renderExportImage(state, size, renderer);
    // Encoding remains Qt's CPU path; only compositing above is accelerated.
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly))
        throw Error("fileError");
    if (format == "exr") {
        const auto data = exrBytes(image);
        if (out.write(data) != data.size()) throw Error("fileError");
    } else if (!cmykProfile.isEmpty()) {
        const auto data = encodeCmyk(image, cmykProfile, format, state.resolution, quality);
        if (out.write(data) != data.size()) throw Error("fileError");
    } else if (format == "pdf") {
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
        if (hdrImage(image)) image=image.convertToFormat(QImage::Format_RGBA64_Premultiplied);
        if (format == "jpeg") {
            QImage white(size, QImage::Format_RGB32);
            white.fill(Qt::white);
            white.setColorSpace(QColorSpace::SRgb);
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
