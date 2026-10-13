#include "GpuProcessing.h"
#ifdef Q_OS_WIN
#include <algorithm>
#include <cmath>
#include <utility>
namespace comp {
GpuSurfacePtr GpuProcessor::warp(ID3D11ShaderResourceView *input, QSize size, int depth,
                                 const QVector<QPointF> &points, double radius, double hardness, double strength,
                                 bool smudge, ID3D11ShaderResourceView *selection) {
    pool_.clear();
    error.clear();
    size_ = size;
    depth_ = depth;
    if (!device_ || !context_ || !input || !size.isValid() || size.width() <= 0 || size.height() <= 0) {
        error = "GPU warp input unavailable";
        return {};
    }
    if (depth != 8 && depth != 16) {
        error = "GPU warp depth unsupported";
        return {};
    }
    if (points.size() < 2 || radius < 1 || !std::isfinite(radius) || !std::isfinite(hardness) ||
        !std::isfinite(strength)) {
        error = "GPU warp parameters invalid";
        return {};
    }
    for (const auto &point : points) {
        if (!std::isfinite(point.x()) || !std::isfinite(point.y())) {
            error = "GPU warp coordinates invalid";
            return {};
        }
    }

    auto current = std::make_shared<GpuSurface>();
    current->view = input;
    current->size = size;
    GpuSurfacePtr mask;
    if (selection) {
        mask = std::make_shared<GpuSurface>();
        mask->view = selection;
        mask->size = size;
    }
    const double hard = std::clamp(hardness, 0., .98);
    bool dispatched = false;
    for (int i = 1; i < points.size(); ++i) {
        const QPointF from = points[i - 1];
        const QPointF to = points[i];
        if (from == to)
            continue;
        const QPointF movement = (to - from) * (smudge ? 1. : strength);
        Params params{};
        // pass() fills P[0] with dimensions and native maximum.  Keep all CPU
        // geometry in double until this final ABI conversion to FP32.
        params[4] = float(to.x());
        params[5] = float(to.y());
        params[6] = float(radius);
        params[7] = float(hard);
        params[8] = float(movement.x());
        params[9] = float(movement.y());
        params[10] = float(strength);
        params[11] = float((smudge ? 1 : 0) | (selection ? 2 : 0));
        GpuSurfacePtr next = selection ? pass("warpDab", {current, mask}, params, size)
                                       : pass("warpDab", {current}, params, size);
        if (!next) {
            if (error.isEmpty())
                error = "GPU warp dispatch failed";
            pool_.clear();
            return {};
        }
        current = std::move(next);
        dispatched = true;
    }
    if (!dispatched) {
        error = "GPU warp has no dabs";
        pool_.clear();
        return {};
    }
    pool_.clear();
    return current;
}
} // namespace comp
#endif
