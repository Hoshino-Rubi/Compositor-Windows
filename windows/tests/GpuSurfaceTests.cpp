#include "GpuLayerSurface.h"
#include "GpuNative.h"
#include "GpuPreview.h"
#include "Pixels.h"
#include "Renderer.h"

#include <QColorSpace>
#include <QPainter>
#include <QSet>
#include <QTest>

#include <algorithm>
#include <cmath>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <wrl/client.h>
#endif

using namespace comp;

namespace {

struct Difference {
    int maximum = 0;
    double mean = 0;
    int count = 0;
    QPoint point{-1, -1};
    int channel = -1;
    int actual = 0;
    int expected = 0;
};

Difference comparePixels(const QImage &actual, const QImage &expected, int depth) {
    // Native staging readback has no document DPI; compare pixel samples only.
    Difference result;
    if (actual.size() != expected.size() || actual.isNull() || expected.isNull()) {
        result.maximum = depth == 16 ? 65535 : 255;
        return result;
    }
    const QImage a = actual.convertToFormat(pixelFormat(depth));
    const QImage b = expected.convertToFormat(pixelFormat(depth));
    const int channels = 4;
    quint64 total = 0;
    const auto record = [&](int x, int y, int channel, int left, int right) {
        const int value = std::abs(left - right);
        if (value > result.maximum) {
            result.maximum = value;
            result.point = QPoint(x, y);
            result.channel = channel;
            result.actual = left;
            result.expected = right;
        }
        total += quint64(value);
    };
    if (depth == 16) {
        for (int y = 0; y < a.height(); ++y) {
            const auto *left = reinterpret_cast<const QRgba64 *>(a.constScanLine(y));
            const auto *right = reinterpret_cast<const QRgba64 *>(b.constScanLine(y));
            for (int x = 0; x < a.width(); ++x) {
                record(x, y, 0, left[x].red(), right[x].red());
                record(x, y, 1, left[x].green(), right[x].green());
                record(x, y, 2, left[x].blue(), right[x].blue());
                record(x, y, 3, left[x].alpha(), right[x].alpha());
            }
        }
    } else {
        for (int y = 0; y < a.height(); ++y) {
            const auto *left = reinterpret_cast<const QRgb *>(a.constScanLine(y));
            const auto *right = reinterpret_cast<const QRgb *>(b.constScanLine(y));
            for (int x = 0; x < a.width(); ++x) {
                record(x, y, 0, qRed(left[x]), qRed(right[x]));
                record(x, y, 1, qGreen(left[x]), qGreen(right[x]));
                record(x, y, 2, qBlue(left[x]), qBlue(right[x]));
                record(x, y, 3, qAlpha(left[x]), qAlpha(right[x]));
            }
        }
    }
    result.count = a.width() * a.height() * channels;
    result.mean = result.count ? double(total) / result.count : 0;
    return result;
}

QString fixturePixel(const QImage &image, QPoint point) {
    if (!image.rect().contains(point)) return QStringLiteral("outside");
    const QColor color = image.pixelColor(point);
    return QStringLiteral("%1,%2,%3,%4")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

QImage sourceImage(QSize size, int depth) {
    QImage image(size, pixelFormat(depth));
    if (image.isNull()) return {};
    image.setColorSpace(QColorSpace::SRgb);
    const int maximum = depth == 16 ? 65535 : 255;
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            const int alpha = maximum * (2 + ((x * 11 + y * 7) % 7)) / 8;
            const int red = int((qint64(x) * maximum / std::max(1, size.width() - 1)) * alpha / maximum);
            const int green = int((qint64(y) * maximum / std::max(1, size.height() - 1)) * alpha / maximum);
            const int blue = int((qint64(x * 17 + y * 29) * maximum /
                                  std::max(1, (size.width() - 1) * 17 + (size.height() - 1) * 29)) * alpha / maximum);
            if (depth == 16)
                reinterpret_cast<QRgba64 *>(image.scanLine(y))[x] = rgba64(red, green, blue, alpha);
            else
                reinterpret_cast<QRgb *>(image.scanLine(y))[x] = qRgba(red, green, blue, alpha);
        }
    }
    return image;
}

QImage maskImage(QSize size, int depth, bool hardEdge = false) {
    QImage mask(size, maskFormat(depth));
    if (mask.isNull()) return {};
    const int maximum = depth == 16 ? 65535 : 255;
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            int value = 0;
            if (hardEdge)
                value = (x >= size.width() / 4 && x < size.width() * 3 / 4 &&
                         y >= size.height() / 4 && y < size.height() * 3 / 4)
                            ? maximum
                            : 0;
            else
                value = ((x * 19 + y * 13 + 23) % (maximum + 1));
            if (depth == 16)
                reinterpret_cast<quint16 *>(mask.scanLine(y))[x] = quint16(value);
            else
                mask.scanLine(y)[x] = uchar(value);
        }
    }
    return mask;
}

State geometryState(int depth) {
    State state;
    state.id = QStringLiteral("GpuLayerSurfaceGeometry");
    state.size = QSize(72, 56);
    state.bitDepth = depth;
    Layer layer;
    layer.id = QStringLiteral("geometry-layer");
    layer.image = sourceImage(QSize(23, 17), depth);
    layer.placement.size = layer.image.size();
    state.layers.append(layer);
    state.active = layer.id;
    return state;
}

State maskedState(int depth) {
    State state = geometryState(depth);
    state.id = QStringLiteral("GpuLayerSurfaceMasks");
    auto &layer = state.layers[0];
    layer.placement.origin = QPointF(13.25, 9.5);
    layer.placement.size = QSizeF(42.5, 31.75);
    layer.placement.rotation = 11.0;
    layer.placement.sampling = QStringLiteral("Smooth");
    layer.mask = maskImage(QSize(23, 17), depth, true);
    layer.maskEnabled = true;
    layer.maskLinked = false;
    layer.maskPlacement = layer.placement;
    layer.maskPlacement.origin += QPointF(2.0, -1.5);

    Layer clipping;
    clipping.id = QStringLiteral("clip-source");
    clipping.image = sourceImage(QSize(31, 22), depth);
    clipping.placement.origin = QPointF(7.0, 4.0);
    clipping.placement.size = QSizeF(52.0, 39.0);
    clipping.placement.rotation = -7.0;
    clipping.opacity = .77;
    state.layers.append(clipping);
    state.layers[0].maskSource = clipping.id;
    return state;
}

State ancestorState(int depth) {
    State state = maskedState(depth);
    state.id = QStringLiteral("GpuLayerSurfaceAncestors");
    auto &layer = state.layers[0];
    layer.parent = QStringLiteral("inner-group");

    Layer clipping = state.layers[1];
    clipping.parent.clear();
    state.layers[1] = clipping;

    Layer inner;
    inner.id = QStringLiteral("inner-group");
    inner.group = true;
    inner.parent = QStringLiteral("outer-group");
    inner.opacity = .68;
    inner.placement.size = state.size;
    inner.mask = maskImage(state.size, depth, false);
    inner.maskEnabled = true;
    inner.maskLinked = false;
    inner.maskPlacement.size = state.size;
    inner.maskPlacement.origin = QPointF(0.75, -0.5);

    Layer outer;
    outer.id = QStringLiteral("outer-group");
    outer.group = true;
    outer.opacity = .74;
    outer.placement.size = state.size;
    outer.mask = maskImage(state.size, depth, true);
    outer.maskEnabled = true;
    outer.maskLinked = true;
    outer.maskPlacement = outer.placement;

    state.layers.append(inner);
    state.layers.append(outer);
    return state;
}

QImage expectedSurface(const State &source, int index, QSize target, bool includeAncestors,
                       bool includeLayerOpacity) {
    State own = source;
    if (!includeLayerOpacity) own.layers[index].opacity = 1;
    QImage result = layerSurface(own, index, target);
    if (!includeAncestors) return result;

    QVector<int> ancestors;
    QSet<QString> visited;
    QString parent = source.layers[index].parent;
    while (!parent.isEmpty() && !visited.contains(parent)) {
        visited.insert(parent);
        const int ancestor = source.index(parent);
        if (ancestor < 0) break;
        ancestors.append(ancestor);
        parent = source.layers[ancestor].parent;
    }
    double opacity = 1;
    for (int ancestor : ancestors) {
        const auto &group = source.layers[ancestor];
        opacity *= group.opacity;
        if (!group.mask.isNull() && group.maskEnabled) {
            QPainter painter(&result);
            painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            painter.drawImage(0, 0, renderMask(group, source.size, target));
        }
    }
    if (opacity < 1) {
        QPainter painter(&result);
        painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        painter.fillRect(result.rect(), QColor::fromRgbF(0, 0, 0, opacity));
    }
    return result;
}

struct CapturedSurface {
    QImage image;
    GpuNativeResult stats;
};

#ifdef Q_OS_WIN
int floatSample(float value, int maximum) {
    if (!std::isfinite(value)) return 0;
    return qRound(std::clamp(double(value), 0., 1.) * maximum);
}
#endif

CapturedSurface captureSurface(const State &state, int index, QSize target, bool includeAncestors,
                               bool includeLayerOpacity) {
    CapturedSurface capture;
#ifndef Q_OS_WIN
    Q_UNUSED(state);
    Q_UNUSED(index);
    Q_UNUSED(target);
    Q_UNUSED(includeAncestors);
    Q_UNUSED(includeLayerOpacity);
    return capture;
#else
    runGpuNative([&](ID3D11Device *device, ID3D11DeviceContext *context, quint64 budget) {
        GpuNativeResult stats;
        const auto native = renderGpuLayerSurface(device, context, state, index, target, includeAncestors,
                                                  includeLayerOpacity, budget, stats);
        if (!native || !native->texture) {
            capture.stats = stats;
            return stats;
        }
        D3D11_TEXTURE2D_DESC description{};
        native->texture->GetDesc(&description);
        if (description.Format != DXGI_FORMAT_R32G32B32A32_FLOAT ||
            description.Width != UINT(target.width()) || description.Height != UINT(target.height())) {
            capture.stats = stats;
            return stats;
        }
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        description.MiscFlags = 0;
        if (FAILED(device->CreateTexture2D(&description, nullptr, &staging))) {
            capture.stats = stats;
            return stats;
        }
        context->CopyResource(staging.Get(), native->texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            capture.stats = stats;
            return stats;
        }
        QImage image(target, pixelFormat(state.bitDepth));
        if (image.isNull()) {
            context->Unmap(staging.Get(), 0);
            capture.stats = stats;
            return stats;
        }
        const int maximum = state.bitDepth == 16 ? 65535 : 255;
        for (int y = 0; y < target.height(); ++y) {
            const auto *row = reinterpret_cast<const float *>(static_cast<const char *>(mapped.pData) +
                                                               size_t(y) * mapped.RowPitch);
            if (state.bitDepth == 16) {
                auto *out = reinterpret_cast<QRgba64 *>(image.scanLine(y));
                for (int x = 0; x < target.width(); ++x)
                    out[x] = rgba64(floatSample(row[x * 4], maximum), floatSample(row[x * 4 + 1], maximum),
                                    floatSample(row[x * 4 + 2], maximum), floatSample(row[x * 4 + 3], maximum));
            } else {
                auto *out = reinterpret_cast<QRgb *>(image.scanLine(y));
                for (int x = 0; x < target.width(); ++x)
                    out[x] = qRgba(floatSample(row[x * 4], maximum), floatSample(row[x * 4 + 1], maximum),
                                   floatSample(row[x * 4 + 2], maximum), floatSample(row[x * 4 + 3], maximum));
            }
        }
        context->Unmap(staging.Get(), 0);
        image.setColorSpace(QColorSpace::SRgb);
        stats.readbackBytes += quint64(target.width()) * target.height() * 16;
        capture.image = image;
        capture.stats = stats;
        return stats;
    });
    return capture;
#endif
}

void requireGpu(const CapturedSurface &capture, const char *message) {
    if (!capture.image.isNull()) return;
    if (qEnvironmentVariableIntValue("COMPOSITOR_REQUIRE_GPU")) QFAIL(message);
    QSKIP(message);
}

} // namespace

class GpuSurfaceTests final : public QObject {
    Q_OBJECT

  private slots:
    void geometryAndSamplingMatchesCpu() {
        const QSize target(72, 56);
        QStringList mismatches;
        struct Case {
            const char *name;
            QPointF origin;
            QSizeF size;
            double rotation;
            bool flipX;
            bool flipY;
        };
        const Case cases[] = {
            {"identity", {0, 0}, {23, 17}, 0, false, false},
            {"translate", {11.25, 8.5}, {23, 17}, 0, false, false},
            {"scale", {4.5, 6.25}, {44.75, 29.5}, 0, false, false},
            {"rotate", {14, 8}, {37, 27}, 23, false, false},
            {"flip-x", {17, 10}, {31, 23}, 0, true, false},
            {"flip-y", {17, 10}, {31, 23}, 0, false, true},
        };
        for (const int depth : {8, 16}) {
            for (const auto &entry : cases) {
                for (const QString &sampling : {QStringLiteral("Nearest"), QStringLiteral("Smooth")}) {
                    State state = geometryState(depth);
                    auto &placement = state.layers[0].placement;
                    placement.origin = entry.origin;
                    placement.size = entry.size;
                    placement.rotation = entry.rotation;
                    placement.flipX = entry.flipX;
                    placement.flipY = entry.flipY;
                    placement.sampling = sampling;
                    const auto capture = captureSurface(state, 0, target, false, true);
                    requireGpu(capture, "Direct3D layer-surface GPU unavailable");
                    QVERIFY2(capture.stats.dispatches > 0, "layer-surface helper did not dispatch");
                    const auto expected = layerSurface(state, 0, target);
                    const auto difference = comparePixels(capture.image, expected, depth);
                    const auto source = state.layers[0].image;
                    const auto inverse = placement.matrix(source.size()).inverted();
                    const QPointF sourcePoint = inverse.map(QPointF(difference.point) + QPointF(.5, .5));
                    const QPoint fixturePoint(int(std::floor(sourcePoint.x())), int(std::floor(sourcePoint.y())));
                    qInfo().noquote() << QStringLiteral("%1 depth=%2 sampling=%3 max=%4 mean=%5")
                                             .arg(QString::fromLatin1(entry.name))
                                             .arg(depth)
                                             .arg(sampling)
                                             .arg(difference.maximum)
                                             .arg(difference.mean, 0, 'f', 3);
                    qInfo().noquote() << QStringLiteral("  worst out=(%1,%2) channel=%3 actual=%4 expected=%5 source=(%6,%7) fixture=%8")
                                             .arg(difference.point.x())
                                             .arg(difference.point.y())
                                             .arg(difference.channel >= 0 ? QStringLiteral("RGBA").at(difference.channel)
                                                                          : QChar('?'))
                                             .arg(difference.actual)
                                             .arg(difference.expected)
                                             .arg(sourcePoint.x(), 0, 'f', 3)
                                             .arg(sourcePoint.y(), 0, 'f', 3)
                                             .arg(fixturePixel(source, fixturePoint));
                    if (difference.maximum > (depth == 8 ? 4 : 32))
                        mismatches.append(QStringLiteral("%1 depth=%2 sampling=%3 max=%4 mean=%5")
                            .arg(QString::fromLatin1(entry.name)).arg(depth).arg(sampling)
                            .arg(difference.maximum).arg(difference.mean, 0, 'f', 3));
                }
            }
        }
        QVERIFY2(mismatches.isEmpty(), qPrintable(mismatches.join("; ")));
    }

    void ownMaskAndClippingChainMatchesCpu() {
        const QSize target(72, 56);
        for (const int depth : {8, 16}) {
            const State state = maskedState(depth);
            const auto capture = captureSurface(state, 0, target, false, true);
            requireGpu(capture, "Direct3D layer-surface GPU unavailable");
            QVERIFY(capture.stats.dispatches > 0);
            const auto expected = layerSurface(state, 0, target);
            const auto difference = comparePixels(capture.image, expected, depth);
            qInfo() << "own mask + clipping" << depth << "max" << difference.maximum << "mean" << difference.mean << "worst" << difference.point << "channel" << difference.channel
                    << "actual" << difference.actual << "expected" << difference.expected;
            // Qt's 8-bit affine SIMD sampler rounds interior bilinear weights
            // to 1/16. The GPU intentionally retains full fractions; a binary
            // mask edge therefore differs by five levels in this fixture.
            // Bound the Qt discrepancy AND independently check the precise
            // interpolation, rather than hiding a sampler/coverage error.
            QVERIFY(difference.maximum <= (depth == 8 ? 8 : 32));
            QVERIFY(difference.mean <= (depth == 8 ? .1 : 2.));
            if (depth == 8) {
                const QPoint pixel(26, 14);
                const auto smooth = [&](const QImage &image, const Placement &placement, bool mask) {
                    const QPointF q = placement.matrix(image.size()).inverted().map(QPointF(pixel) + QPointF(.5, .5))
                                      - QPointF(.5, .5);
                    const int x = int(std::floor(q.x())), y = int(std::floor(q.y()));
                    const double fx = q.x() - x, fy = q.y() - y;
                    const auto tap = [&](int dx, int dy) {
                        const QColor c = image.pixelColor(std::clamp(x + dx, 0, image.width() - 1),
                                                          std::clamp(y + dy, 0, image.height() - 1));
                        return mask ? c.red() : c.alpha();
                    };
                    return qRound((tap(0, 0) * (1 - fx) + tap(1, 0) * fx) * (1 - fy) +
                                  (tap(0, 1) * (1 - fx) + tap(1, 1) * fx) * fy);
                };
                const auto &layer = state.layers[0];
                const auto &clip = state.layers[state.index(layer.maskSource)];
                const int alpha = smooth(layer.image, layer.placement, false);
                const int mask = smooth(layer.mask, layer.maskPlacement, true);
                const double opacity = ((int(clip.opacity * 256) * 255) >> 8) / 255.;
                const int clipAlpha = qRound(smooth(clip.image, clip.placement, false) * opacity);
                const int precise = qRound(qRound(alpha * mask / 255.) * clipAlpha / 255.);
                QCOMPARE(precise, 97);
                QVERIFY(std::abs(capture.image.pixelColor(pixel).alpha() - precise) <= 1);
            }

            // The hard-edge mask has transparent corners.  Those pixels are
            // outside both the mask and the transformed layer and must stay
            // exactly transparent after the native pass.
            const auto outside = capture.image.pixelColor(0, 0);
            QCOMPARE(outside.alpha(), 0);
            const auto farOutside = capture.image.pixelColor(target.width() - 1, target.height() - 1);
            QCOMPARE(farOutside.alpha(), 0);
        }
    }

    void ancestorMasksAndGroupOpacityMatchCpu() {
        const QSize target(72, 56);
        for (const int depth : {8, 16}) {
            const State state = ancestorState(depth);
            const auto capture = captureSurface(state, 0, target, true, true);
            requireGpu(capture, "Direct3D layer-surface GPU unavailable");
            QVERIFY(capture.stats.dispatches > 0);
            const auto expected = expectedSurface(state, 0, target, true, true);
            const auto difference = comparePixels(capture.image, expected, depth);
            qInfo() << "ancestor masks + group opacity" << depth << "max" << difference.maximum << "mean"
                    << difference.mean << "worst" << difference.point << "channel" << difference.channel
                    << "actual" << difference.actual << "expected" << difference.expected;
            QVERIFY(difference.maximum <= (depth == 8 ? 4 : 32));

            const auto noOpacityCapture = captureSurface(state, 0, target, true, false);
            requireGpu(noOpacityCapture, "Direct3D layer-surface GPU unavailable");
            const auto noOpacityExpected = expectedSurface(state, 0, target, true, false);
            const auto noOpacityDifference = comparePixels(noOpacityCapture.image, noOpacityExpected, depth);
            qInfo() << "ancestor masks without layer opacity" << depth << "max" << noOpacityDifference.maximum
                    << "mean" << noOpacityDifference.mean;
            QVERIFY(noOpacityDifference.maximum <= (depth == 8 ? 4 : 32));
        }
    }
};

QTEST_MAIN(GpuSurfaceTests)
#include "GpuSurfaceTests.moc"
