#include "Document.h"
#include "Renderer.h"
#include "Filters.h"
#include "Selection.h"
#include <QPainter>
#include <QPainterPathStroker>
#include <QSet>
extern "C" {
#include "ContentFill.h"
}

namespace comp {
Document::Session Document::session() const {
    return {state_, undo_, redo_, revision_, cleanRevision_, serial_};
}
void Document::restore(const Session &s) {
    state_ = s.state;
    undo_ = s.undo;
    redo_ = s.redo;
    revision_ = s.revision;
    cleanRevision_ = s.cleanRevision;
    serial_ = s.serial;
    before_.reset();
    emit changed();
}
void Document::setSelection(QPainterPath path, int mode) {
    QPainterPath bounds;
    bounds.addRect(QRectF(QPointF(), state_.size));
    path = path.intersected(bounds);
    if (mode == 1 && state_.selectionActive)
        path = state_.selection.united(path);
    if (mode == 2 && state_.selectionActive)
        path = state_.selection.subtracted(path);
    if (mode == 3 && state_.selectionActive)
        path = state_.selection.intersected(path);
    state_.selectionMask = {};
    state_.selection = path;
    state_.selectionActive = true;
    emit selectionChanged();
}
void Document::deselect() {
    state_.selectionMask = {};
    state_.selection = {};
    state_.selectionActive = false;
    emit selectionChanged();
}
void Document::selectAll() {
    QPainterPath path;
    path.addRect(QRectF(QPointF(), state_.size));
    setSelection(path);
}
void Document::invertSelection() {
    QPainterPath path;
    path.addRect(QRectF(QPointF(), state_.size));
    if (!state_.selectionMask.isNull()) {
        state_.selectionMask.invertPixels();
        state_.selection = path.subtracted(state_.selection);
        emit selectionChanged();
    } else
        setSelection(path.subtracted(state_.selection));
}
void Document::modifySelection(double amount) {
    if (!state_.selectionActive)
        return;
    QPainterPathStroker stroke;
    stroke.setWidth(abs(amount) * 2);
    const auto border = stroke.createStroke(state_.selection);
    setSelection(amount >= 0 ? state_.selection.united(border) : state_.selection.subtracted(border));
}
void Document::selectPixels(bool mask) {
    const int i = state_.index(state_.active);
    if (i < 0)
        return;
    auto image = mask ? renderMask(state_.layers[i], state_.size, state_.size) : layerSurface(state_, i, state_.size);
    setSelection(alphaPath(image));
}
static void makeRaster(Layer &l, const State &s, bool mask) {
    QImage &image = mask ? l.mask : l.image;
    if (image.isNull() || (mask && image.size() == QSize(1, 1))) {
        const QColor base =
            mask ? (image.isNull() ? QColor(Qt::white) : image.pixelColor(0, 0)) : QColor(Qt::transparent);
        QSize size = mask && !l.image.isNull() ? l.image.size() : s.size;
        image = QImage(size, QImage::Format_ARGB32_Premultiplied);
        if (image.isNull())
            throw Error("tooLarge");
        image.fill(base);
        if (!mask) {
            l.placement = {};
            l.placement.size = s.size;
        } else if (!l.maskLinked)
            l.maskPlacement = l.placement;
    }
    image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (mask)
        l.originalMaskPng.clear();
    else {
        l.originalPng.clear();
        l.metadata.remove("text");
        l.metadata.remove("shape");
    }
}
void Document::fill(QColor color, bool erase, bool mask) {
    const int index = state_.index(state_.active);
    if (index < 0 || (state_.layers[index].group && !mask))
        return;
    edit([&](State &s) {
        auto &l = s.layers[index];
        makeRaster(l, s, mask);
        auto &image = mask ? l.mask : l.image;
        const QImage before = image;
        QPainter p(&image);
        const auto placement = mask && !l.maskLinked ? l.maskPlacement : l.placement;
        p.setTransform(placement.matrix(image.size()).inverted());
        if (s.selectionActive && s.selectionMask.isNull())
            p.setClipPath(s.selection);
        if (erase && !mask) {
            p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
            color = Qt::black;
        }
        if (mask)
            color = QColor::fromRgb(erase ? qRgb(0, 0, 0)
                                          : qRgb(qGray(color.rgb()), qGray(color.rgb()), qGray(color.rgb())));
        p.fillRect(QRectF(QPointF(), s.size), color);
        p.end();
        if (!s.selectionMask.isNull())
            constrainSelection(s, placement, before, image);
    });
}
QImage Document::copyPixels(bool merged) const {
    const int index = state_.index(state_.active);
    if (index < 0 && !merged)
        return {};
    QImage image = merged ? render(state_) : layerSurface(state_, index, state_.size);
    if (state_.selectionActive) {
        QImage masked(image.size(), image.format());
        if (masked.isNull())
            throw Error("tooLarge");
        masked.fill(Qt::transparent);
        QPainter p(&masked);
        p.setClipPath(state_.selection);
        p.drawImage(0, 0, image);
        p.end();
        if (!state_.selectionMask.isNull()) {
            masked = image;
            Placement placement;
            placement.size = state_.size;
            auto coverage = selectionCoverage(state_, masked.size(), placement);
            QPainter blend(&masked);
            blend.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            blend.drawImage(0, 0, coverage);
        }
        QRect bounds = state_.selectionMask.isNull() ? state_.selection.boundingRect().toAlignedRect()
                                                     : state_.selectionMask.rect();
        if (bounds.isEmpty())
            return {};
        return masked.copy(bounds.intersected(image.rect()));
    }
    return image;
}
void Document::pastePixels(const QImage &image, const QString &name) {
    if (image.isNull())
        return;
    addLayer(name, image);
}
void Document::applyFilter(const QJsonObject &settings, bool mask) {
    const int index = state_.index(state_.active);
    if (index < 0 || (state_.layers[index].group && !mask))
        return;
    edit([&](State &s) {
        auto &l = s.layers[index];
        makeRaster(l, s, mask);
        auto &image = mask ? l.mask : l.image;
        const auto before = image;
        auto output = filtered(image, settings);
        if (!s.selectionMask.isNull()) {
            constrainSelection(s, mask && !l.maskLinked ? l.maskPlacement : l.placement, before, output);
            image = output;
            return;
        }
        if (s.selectionActive) {
            QPainter p(&image);
            auto placement = mask && !l.maskLinked ? l.maskPlacement : l.placement;
            p.setClipPath(placement.matrix(image.size()).inverted().map(s.selection));
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.drawImage(0, 0, output);
        } else
            image = output;
    });
}
void Document::maskAction(const QString &action) {
    const int i = state_.index(state_.active);
    if (i < 0)
        return;
    edit([&](State &s) {
        auto &l = s.layers[i];
        if (action == "addMask" || action == "hideMask") {
            l.mask = QImage(s.size, QImage::Format_Grayscale8);
            if (l.mask.isNull())
                throw Error("tooLarge");
            l.mask.fill(action == "hideMask" || s.selectionActive ? Qt::black : Qt::white);
            l.maskLinked = false;
            l.maskPlacement = {};
            l.maskPlacement.size = s.size;
            if (s.selectionActive) {
                QPainter p(&l.mask);
                p.fillPath(s.selection, Qt::white);
                p.end();
                if (!s.selectionMask.isNull())
                    l.mask = s.selectionMask.copy();
            }
            l.maskEnabled = true;
        } else if (action == "deleteMask")
            l.mask = {};
        else if (action == "invertMask")
            l.mask.invertPixels();
        else if (action == "enableMask")
            l.maskEnabled = !l.maskEnabled;
        else if (action == "linkMask") {
            if (l.maskLinked) {
                l.maskPlacement = l.placement;
                l.maskLinked = false;
            } else {
                QImage mapped(l.image.isNull() ? s.size : l.image.size(), QImage::Format_Grayscale8);
                mapped.fill(Qt::black);
                QPainter p(&mapped);
                p.setTransform(l.placement.matrix(mapped.size()).inverted());
                p.setTransform(l.maskPlacement.matrix(l.mask.size()), true);
                p.drawImage(0, 0, l.mask);
                p.end();
                l.mask = mapped;
                l.maskLinked = true;
            }
        } else if (action == "applyMask" && !l.image.isNull()) {
            QImage alpha = renderMask(l, s.size, s.size);
            QImage mapped(l.image.size(), QImage::Format_ARGB32_Premultiplied);
            mapped.fill(Qt::transparent);
            {
                QPainter p(&mapped);
                p.setTransform(l.placement.matrix(l.image.size()).inverted());
                p.drawImage(0, 0, alpha);
            }
            QPainter p(&l.image);
            p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            p.drawImage(0, 0, mapped);
            p.end();
            l.mask = {};
            l.originalPng.clear();
            l.metadata.remove("text");
            l.metadata.remove("shape");
        } else if (action == "clipMask") {
            if (!l.maskSource.isEmpty())
                l.maskSource.clear();
            else
                for (int j = i - 1; j >= 0; --j)
                    if (!s.layers[j].group && s.layers[j].parent == l.parent) {
                        l.maskSource = s.layers[j].id;
                        break;
                    }
        }
        l.originalMaskPng.clear();
    });
}
void Document::groupLayers(const QStringList &ids, const QString &name) {
    if (ids.isEmpty())
        return;
    edit([&](State &s) {
        const int first = s.index(ids.first());
        if (first < 0)
            return;
        Layer group;
        group.id = newId();
        group.name = name;
        group.group = true;
        group.parent = s.layers[first].parent;
        group.placement.size = s.size;
        int insertion = first;
        for (const auto &id : ids) {
            int i = s.index(id);
            if (i >= 0 && s.layers[i].parent == group.parent) {
                s.layers[i].parent = group.id;
                insertion = std::max(insertion, i);
            }
        }
        s.layers.insert(insertion + 1, group);
        s.active = group.id;
    });
}
void Document::ungroup() {
    const int i = state_.index(state_.active);
    if (i < 0 || !state_.layers[i].group)
        return;
    edit([&](State &s) {
        const Layer group = s.layers[i];
        for (auto &l : s.layers)
            if (l.parent == group.id) {
                l.parent = group.parent;
                l.opacity *= group.opacity;
                l.visible = l.visible && group.visible;
            }
        s.layers.removeAt(i);
        s.active = s.layers.isEmpty() ? QString() : s.layers.last().id;
    });
}
void Document::relocate(const QString &id, const QString &parent, const QString &before) {
    if (id == parent)
        return;
    const int index = state_.index(id);
    if (index < 0)
        return;
    for (int i : state_.descendants(id))
        if (state_.layers[i].id == parent)
            return;
    edit([&](State &s) {
        auto layer = s.layers.takeAt(index);
        layer.parent = parent;
        int position = s.index(before);
        s.layers.insert(position < 0 ? s.layers.size() : position, layer);
        s.active = id;
    });
}
void Document::merge(const QStringList &ids) {
    if (ids.isEmpty())
        return;
    QSet<QString> chosen;
    for (const auto &id : ids) {
        chosen.insert(id);
        for (int i : state_.descendants(id))
            chosen.insert(state_.layers[i].id);
    }
    if (chosen.size() == 1) {
        int i = state_.index(ids.first());
        if (i > 0 && state_.layers[i - 1].parent == state_.layers[i].parent && !state_.layers[i - 1].group)
            chosen.insert(state_.layers[i - 1].id);
        else
            return;
    }
    edit([&](State &s) {
        State subset = s;
        subset.layers.removeIf([&](const Layer &l) { return !chosen.contains(l.id); });
        for (auto &l : subset.layers) {
            if (!chosen.contains(l.parent))
                l.parent.clear();
            if (!chosen.contains(l.maskSource))
                l.maskSource.clear();
        }
        Layer result;
        const int i = s.index(ids.first());
        result.id = newId();
        result.name = s.layers[i].name;
        result.parent = s.layers[i].parent;
        if (chosen.contains(result.parent))
            result.parent.clear();
        result.placement.size = s.size;
        result.image = render(subset);
        int position = 0;
        for (int j = 0; j < i; ++j)
            if (!chosen.contains(s.layers[j].id))
                ++position;
        s.layers.removeIf([&](const Layer &l) { return chosen.contains(l.id); });
        for (auto &l : s.layers)
            if (chosen.contains(l.maskSource))
                l.maskSource = result.id;
        s.layers.insert(position, result);
        s.active = result.id;
    });
}
void Document::resizeCanvas(QSize size, bool rescale, bool centered) {
    if (!size.isValid() || size.width() > MaxSide || size.height() > MaxSide ||
        qint64(size.width()) * size.height() > MaxCanvasPixels)
        throw Error("invalidSize");
    edit([&](State &s) {
        const double sx = double(size.width()) / s.size.width(), sy = double(size.height()) / s.size.height();
        const QPointF delta =
            centered ? QPointF((size.width() - s.size.width()) / 2., (size.height() - s.size.height()) / 2.)
                     : QPointF();
        QTransform mapping;
        if (rescale)
            mapping.scale(sx, sy);
        else
            mapping.translate(delta.x(), delta.y());
        for (auto &l : s.layers)
            for (Placement *p : {&l.placement, &l.maskPlacement}) {
                p->origin = mapping.map(p->origin);
                if (rescale)
                    p->size = {p->size.width() * sx, p->size.height() * sy};
            }
        s.selection = mapping.map(s.selection);
        if (!s.selectionMask.isNull()) {
            QImage mapped(size, QImage::Format_Grayscale8);
            mapped.fill(Qt::black);
            QPainter p(&mapped);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.setTransform(mapping);
            p.drawImage(0, 0, s.selectionMask);
            p.end();
            s.selectionMask = mapped;
        }
        for (int i = 0; i < s.guides.size(); ++i) {
            auto g = s.guides[i].toObject();
            const bool h = g["axis"] == "horizontal";
            g["position"] = rescale ? g["position"].toDouble() * (h ? sy : sx)
                                    : g["position"].toDouble() + (h ? delta.y() : delta.x());
            s.guides[i] = g;
        }
        s.size = size;
    });
}
void Document::flipCanvas(bool horizontal) {
    edit([&](State &s) {
        for (auto &l : s.layers)
            for (auto *p : {&l.placement, &l.maskPlacement}) {
                if (horizontal) {
                    p->origin.setX(s.size.width() - p->origin.x() - p->size.width());
                    p->flipX = !p->flipX;
                } else {
                    p->origin.setY(s.size.height() - p->origin.y() - p->size.height());
                    p->flipY = !p->flipY;
                }
                p->rotation = -p->rotation;
            }
        QTransform map;
        if (horizontal) {
            map.translate(s.size.width(), 0);
            map.scale(-1, 1);
        } else {
            map.translate(0, s.size.height());
            map.scale(1, -1);
        }
        s.selection = map.map(s.selection);
        if (!s.selectionMask.isNull())
            s.selectionMask = s.selectionMask.mirrored(horizontal, !horizontal);
        for (int i = 0; i < s.guides.size(); ++i) {
            auto g = s.guides[i].toObject();
            if ((g["axis"] == "vertical") == horizontal)
                g["position"] = (horizontal ? s.size.width() : s.size.height()) - g["position"].toDouble();
            s.guides[i] = g;
        }
    });
}
void Document::trim() {
    auto image = render(state_);
    int minX = image.width(), minY = image.height(), maxX = -1, maxY = -1;
    for (int y = 0; y < image.height(); ++y) {
        const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x)
            if (qAlpha(row[x])) {
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
    }
    if (maxX >= minX)
        crop(QRect(QPoint(minX, minY), QPoint(maxX, maxY)));
}
void Document::addGuide(bool horizontal, double position) {
    edit([&](State &s) {
        s.guides.append(
            QJsonObject{{"id", newId()}, {"axis", horizontal ? "horizontal" : "vertical"}, {"position", position}});
    });
}
void Document::contentFill() {
    int i = state_.index(state_.active);
    if (i < 0 || !state_.selectionActive || state_.layers[i].image.isNull())
        return;
    edit([&](State &s) {
        auto &l = s.layers[i];
        auto rgba = l.image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        QImage coverage(rgba.size(), QImage::Format_Grayscale8);
        coverage.fill(Qt::black);
        QPainter p(&coverage);
        p.setTransform(l.placement.matrix(rgba.size()).inverted());
        p.fillPath(s.selection, Qt::white);
        p.end();
        int result = content_fill(rgba.bits(), rgba.bytesPerLine(), coverage.constBits(), coverage.bytesPerLine(),
                                  rgba.width(), rgba.height());
        if (result < 0)
            throw Error("tooLarge");
        if (result == 0)
            throw Error("noDonor");
        auto resultImage = rgba.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        if (!s.selectionMask.isNull())
            constrainSelection(s, l.placement, l.image, resultImage);
        l.image = resultImage;
        l.originalPng.clear();
        l.metadata.remove("text");
        l.metadata.remove("shape");
    });
}
void Document::featherSelection(double radius) {
    if (!state_.selectionActive)
        return;
    Placement placement;
    placement.size = state_.size;
    auto alpha = selectionCoverage(state_, state_.size, placement);
    auto blurred = blurImage(alpha, radius);
    QImage gray(state_.size, QImage::Format_Grayscale8);
    for (int y = 0; y < gray.height(); ++y) {
        const auto *src = reinterpret_cast<const QRgb *>(blurred.constScanLine(y));
        auto *dst = gray.scanLine(y);
        for (int x = 0; x < gray.width(); ++x)
            dst[x] = qAlpha(src[x]);
    }
    state_.selectionMask = gray;
    emit selectionChanged();
}
void Document::setSelectionMask(const QImage &mask) {
    if (mask.size() != state_.size)
        throw Error("invalidSize");
    state_.selectionMask = mask.convertToFormat(QImage::Format_Grayscale8);
    QImage alpha(mask.size(), QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < alpha.height(); ++y) {
        const auto *src = state_.selectionMask.constScanLine(y);
        auto *dst = reinterpret_cast<QRgb *>(alpha.scanLine(y));
        for (int x = 0; x < alpha.width(); ++x)
            dst[x] = qRgba(src[x], src[x], src[x], src[x]);
    }
    state_.selection = alphaPath(alpha);
    state_.selectionActive = true;
    emit selectionChanged();
}
} // namespace comp
