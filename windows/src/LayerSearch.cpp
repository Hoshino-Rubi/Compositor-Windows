#include "LayerSearch.h"
namespace comp {
QSet<QString> matchingLayers(const State &state, const QString &name, const QString &type, const QStringList &selected) {
    QSet<QString> result, direct, folders;
    QHash<QString, QString> parents;
    const auto query = name.trimmed();
    for (const auto &layer : state.layers) {
        parents[layer.id] = layer.parent;
        const auto kind = layer.group ? "group" : layer.metadata.contains("adjustment") ? "adjustment"
                        : layer.metadata.contains("text") ? "text" : layer.metadata.contains("shape") ? "shape" : "raster";
        if (!layer.name.contains(query, Qt::CaseInsensitive) ||
            (type == "selected" ? !selected.contains(layer.id) : type != "all" && type != kind)) continue;
        direct.insert(layer.id);
        if (layer.group) folders.insert(layer.id);
    }
    for (const auto &layer : state.layers) {
        QString parent = layer.parent;
        bool included = direct.contains(layer.id);
        for (int depth = 0; !parent.isEmpty() && depth < state.layers.size(); ++depth) {
            included |= folders.contains(parent); parent = parents.value(parent);
        }
        if (!included) continue;
        result.insert(layer.id); parent = layer.parent;
        for (int depth = 0; !parent.isEmpty() && depth < state.layers.size(); ++depth) {
            result.insert(parent); parent = parents.value(parent);
        }
    }
    return result;
}
}
