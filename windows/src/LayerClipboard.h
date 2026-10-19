#pragma once
#include "Document.h"
namespace comp {
inline constexpr const char *LayerMimeType = "application/x-compositor-layers+json";
QByteArray encodeLayers(const State &source, const QStringList &selected);
bool pasteLayers(Document &target, const QByteArray &data, const QString &parent = {},
                 const QString &before = {}, bool offset = false);
bool moveLayers(Document &document, const QStringList &selected, const QString &parent, const QString &before);
bool cutLayers(Document &source, const QStringList &selected);
QString layerClipboardDocument(const QByteArray &data);
State layersAsDocument(const QByteArray &data);
} // namespace comp
