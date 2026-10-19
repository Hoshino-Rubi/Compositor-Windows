#include "Pixels.h"
#include "Document.h"
#include <QSet>
namespace comp {
Document::Document(QObject *parent) : QObject(parent) {}
void Document::reset(State state, bool saved) {
    normalizePrecision(state);
    validateState(state);
    state_ = std::move(state);
    undo_.clear();
    redo_.clear();
    before_.reset();
    revision_ = ++serial_;
    cleanRevision_ = saved ? revision_ : 0;
    emit changed();
}
void Document::markSaved(quint64 revision) {
    cleanRevision_ = revision;
    emit selectionChanged();
}
void Document::select(const QString &id) {
    if (id.isEmpty() || state_.index(id) >= 0) {
        state_.active = id;
        emit selectionChanged();
    }
}
void Document::edit(const std::function<void(State &)> &change) {
    if (!hasDocument() || before_)
        return;
    State candidate = state_;
    change(candidate);
    normalizePrecision(candidate);
    validateState(candidate);
    undo_.append({state_, revision_});
    state_ = std::move(candidate);
    revision_ = ++serial_;
    redo_.clear();
    trimHistory();
    emit changed();
}
void Document::begin() {
    if (!before_)
        before_ = Snapshot{state_, revision_};
}
void Document::preview() {
    emit visualChanged();
}
void Document::commit() {
    if (!before_)
        return;
    try {
        normalizePrecision(state_);
        validateState(state_);
    } catch (...) {
        cancel();
        throw;
    }
    undo_.append(*before_);
    before_.reset();
    revision_ = ++serial_;
    redo_.clear();
    trimHistory();
    emit changed();
}
void Document::cancel() {
    if (before_) {
        state_ = before_->state;
        revision_ = before_->revision;
        before_.reset();
        emit changed();
    }
}
void Document::undo() {
    if (before_)
        cancel();
    if (undo_.isEmpty())
        return;
    redo_.append({state_, revision_});
    auto entry = undo_.takeLast();
    state_ = entry.state;
    revision_ = entry.revision;
    emit changed();
}
void Document::redo() {
    if (before_)
        cancel();
    if (redo_.isEmpty())
        return;
    undo_.append({state_, revision_});
    auto entry = redo_.takeLast();
    state_ = entry.state;
    revision_ = entry.revision;
    emit changed();
}
void Document::trimHistory() {
    while (undo_.size() > 40)
        undo_.removeFirst();
    auto memory = [&] {
        QSet<qint64> seen;
        qint64 bytes = 0;
        for (const auto &snapshot : undo_)
            for (const auto &l : snapshot.state.layers)
                for (const auto &image : {l.image, l.mask})
                    if (!image.isNull() && !seen.contains(image.cacheKey())) {
                        seen.insert(image.cacheKey());
                        bytes += image.sizeInBytes();
                    }
        return bytes;
    };
    while (undo_.size() > 1 && memory() > 256ll * 1024 * 1024)
        undo_.removeFirst();
}
void Document::setLayerBitDepth(int depth) {
    const int i = state_.index(state_.active);
    if ((depth != 8 && depth != 16) || i < 0 || state_.layers[i].group || state_.layers[i].image.isNull()) return;
    edit([&](State &s) {
        auto &layer = s.layers[i];
        if(depth==8 && wideImage(layer.image)) {
            const auto source=layer.image.convertToFormat(QImage::Format_RGBA64);
            QImage result(source.size(),QImage::Format_ARGB32_Premultiplied);if(result.isNull())throw Error("tooLarge");
            for(int y=0;y<source.height();++y){const auto *in=reinterpret_cast<const QRgba64 *>(source.constScanLine(y));auto *out=reinterpret_cast<QRgb *>(result.scanLine(y));
                for(int x=0;x<source.width();++x){quint32 hash=quint32(x)*0x9e3779b9u ^ quint32(y)*0x85ebca6bu;hash^=hash>>16;hash*=0x7feb352du;hash^=hash>>15;
                    const double noise=(hash&65535)/65536.;auto quantize=[&](int v){return std::clamp(int(std::floor(v/257.+noise)),0,255);};
                    out[x]=qPremultiply(qRgba(quantize(in[x].red()),quantize(in[x].green()),quantize(in[x].blue()),qRound(in[x].alpha()/257.)));}}
            result.setColorSpace(QColorSpace::SRgb);layer.image=result;
        } else layer.image = layer.image.convertToFormat(pixelFormat(depth));
        layer.metadata["windowsLayerBitDepth"] = depth;
        layer.originalPng.clear();
        if (depth == 16) s.bitDepth = 16;
    });
}
void Document::setBitDepth(int depth) {
    if ((depth != 8 && depth != 16) || (state_.bitDepth == depth && !hasHdr(state_)))
        return;
    edit([&](State &s) {
        s.bitDepth = depth;
        for (auto &l : s.layers) {
            l.metadata.remove("windowsLayerBitDepth");
            if (!l.image.isNull())
                l.image = l.image.convertToFormat(pixelFormat(depth));
            if (!l.mask.isNull())
                l.mask = maskImage(l.mask, depth);
            l.originalPng.clear();
            l.originalMaskPng.clear();
        }
        if (!s.selectionMask.isNull())
            s.selectionMask = maskImage(s.selectionMask, depth);
    });
}
void Document::addLayer(const QString &name, const QImage &image, bool group) {
    edit([&](State &s) {
        Layer layer;
        layer.id = newId();
        layer.name = name;
        layer.group = group;
        layer.image = image;
        layer.placement.size = image.isNull() ? s.size : image.size();
        const int selected = s.index(s.active);
        if (selected >= 0)
            layer.parent = s.layers[selected].group ? s.layers[selected].id : s.layers[selected].parent;
        if (!image.isNull()) {
            const double scale =
                std::min({1., double(s.size.width()) / image.width(), double(s.size.height()) / image.height()});
            layer.placement.size *= scale;
            layer.placement.origin = {(s.size.width() - layer.placement.size.width()) / 2,
                                      (s.size.height() - layer.placement.size.height()) / 2};
        }
        s.layers.append(layer);
        s.active = layer.id;
    });
}
void Document::duplicate(const QString &name) {
    const int index = state_.index(state_.active);
    if (index < 0)
        return;
    edit([&](State &s) {
        auto selected = s.layers[index];
        auto indices = s.descendants(selected.id);
        indices.prepend(index);
        QHash<QString, QString> ids;
        for (int i : indices)
            ids[s.layers[i].id] = newId();
        QVector<Layer> copies;
        for (int i : indices) {
            auto l = s.layers[i];
            l.id = ids[l.id];
            if (ids.contains(l.parent))
                l.parent = ids[l.parent];
            if (ids.contains(l.maskSource))
                l.maskSource = ids[l.maskSource];
            if (i == index)
                l.name = name;
            l.placement.origin += QPointF(12, 12);
            if (!l.maskLinked)
                l.maskPlacement.origin += QPointF(12, 12);
            copies.append(l);
        }
        s.layers += copies;
        s.active = ids[selected.id];
    });
}
void Document::remove() {
    const QString active = state_.active;
    if (active.isEmpty())
        return;
    edit([&](State &s) {
        QSet<QString> removed{active};
        for (int i : s.descendants(active))
            removed.insert(s.layers[i].id);
        s.layers.removeIf([&](const Layer &l) { return removed.contains(l.id); });
        for (auto &l : s.layers)
            if (removed.contains(l.maskSource))
                l.maskSource.clear();
        s.active = s.layers.isEmpty() ? QString() : s.layers.last().id;
    });
}
void Document::reorder(int direction) {
    const int index = state_.index(state_.active);
    if (index < 0)
        return;
    int other = index + direction;
    while (other >= 0 && other < state_.layers.size() && state_.layers[other].parent != state_.layers[index].parent)
        other += direction;
    if (other < 0 || other >= state_.layers.size())
        return;
    edit([&](State &s) { s.layers.swapItemsAt(index, other); });
}
void Document::moveBy(QPointF delta) {
    const int index = state_.index(state_.active);
    if (index < 0)
        return;
    edit([&](State &s) {
        auto indices = s.descendants(s.active);
        indices.prepend(index);
        for (int i : indices) {
            s.layers[i].placement.origin += delta;
            if (!s.layers[i].maskLinked)
                s.layers[i].maskPlacement.origin += delta;
        }
    });
}
void Document::crop(QRect rect) {
    rect = rect.intersected(QRect(QPoint(0, 0), state_.size));
    if (rect.isEmpty() || rect.size() == state_.size)
        return;
    edit([&](State &s) {
        s.size = rect.size();
        QTransform shift;
        shift.translate(-rect.x(), -rect.y());
        s.selection = shift.map(s.selection);
        if (!s.selectionMask.isNull())
            s.selectionMask = s.selectionMask.copy(rect);
        for (auto &l : s.layers) {
            l.placement.origin -= rect.topLeft();
            l.maskPlacement.origin -= rect.topLeft();
        }
        for (int i = 0; i < s.guides.size(); ++i) {
            auto g = s.guides[i].toObject();
            g["position"] = g["position"].toDouble() - (g["axis"] == "horizontal" ? rect.y() : rect.x());
            s.guides[i] = g;
        }
    });
}
} // namespace comp
