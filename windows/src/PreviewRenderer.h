#pragma once
#include "Project.h"
#include <atomic>
#include <memory>
namespace comp {
struct PreviewTile { QRect rectangle; QImage image; };
struct CanvasPreview {
    QImage overview;
    QVector<PreviewTile> tiles;
    quint64 revision = 0;
};
QImage renderPreview(const State &state, QSize target = {});
QImage renderRegion(const State &state, QRect region, bool accelerated = false);
CanvasPreview renderCanvasPreview(const State &state, QRect visible, QSize overviewSize, bool detailed,
                                  quint64 revision, const CanvasPreview &previous,
                                  const std::shared_ptr<std::atomic_bool> &cancelled);
} // namespace comp
