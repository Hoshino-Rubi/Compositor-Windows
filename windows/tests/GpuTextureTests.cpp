#include "GpuNative.h"
#include "GpuPreview.h"
#include "Pixels.h"
#include "PreviewRenderer.h"
#include "Project.h"
#include "Renderer.h"

#include <QColorSpace>
#include <QJsonObject>
#include <QTest>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <wrl/client.h>
#endif

using namespace comp;

namespace {

QImage pattern(QSize size, int depth, int seed = 0) {
    QImage image(size, pixelFormat(depth));
    if (image.isNull()) return {};
    image.setColorSpace(QColorSpace::SRgb);
    const int maximum = depth == 16 ? 65535 : 255;
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            const int alpha = maximum * (3 + ((x * 13 + y * 7 + seed) % 5)) / 8;
            const int red = int((qint64(x * 17 + seed * 3) * maximum /
                                 std::max(1, size.width() * 17 - 1)) * alpha / maximum);
            const int green = int((qint64(y * 23 + seed * 5) * maximum /
                                   std::max(1, size.height() * 23 - 1)) * alpha / maximum);
            const int blue = int((qint64(x * 11 + y * 19 + seed * 7) * maximum /
                                  std::max(1, (size.width() - 1) * 11 + (size.height() - 1) * 19 + seed * 7)) *
                                 alpha / maximum);
            if (depth == 16)
                reinterpret_cast<QRgba64 *>(image.scanLine(y))[x] = rgba64(red, green, blue, alpha);
            else
                reinterpret_cast<QRgb *>(image.scanLine(y))[x] = qRgba(red, green, blue, alpha);
        }
    }
    return image;
}

QImage hardMask(QSize size, int depth, int inset = 0) {
    QImage image(size, maskFormat(depth));
    if (image.isNull()) return {};
    const int maximum = depth == 16 ? 65535 : 255;
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            const bool inside = x >= inset && y >= inset && x < size.width() - inset &&
                                y < size.height() - inset;
            if (depth == 16)
                reinterpret_cast<quint16 *>(image.scanLine(y))[x] = inside ? quint16(maximum) : 0;
            else
                image.scanLine(y)[x] = inside ? uchar(maximum) : 0;
        }
    }
    return image;
}

State basicState(int depth, QSize size = {48, 36}) {
    State state;
    state.id = QStringLiteral("GpuTextureBasic");
    state.size = size;
    state.resolution = 72;
    state.bitDepth = depth;
    Layer layer;
    layer.id = QStringLiteral("base");
    layer.name = QStringLiteral("base");
    layer.image = pattern(size, depth, 3);
    layer.placement.size = size;
    state.layers.append(layer);
    state.active = layer.id;
    return state;
}

State maskedAdjustmentState(int depth) {
    auto state = basicState(depth);
    const auto size = state.size;

    Layer group;
    group.id = QStringLiteral("mask-group");
    group.name = QStringLiteral("mask group");
    group.group = true;
    group.opacity = .81;
    group.mask = hardMask(size, depth, 4);
    group.maskEnabled = true;
    group.maskLinked = true;
    group.placement.size = size;

    Layer clipped;
    clipped.id = QStringLiteral("clipped");
    clipped.name = QStringLiteral("clipped");
    clipped.parent = group.id;
    clipped.image = pattern(size, depth, 17);
    clipped.opacity = .73;
    clipped.mask = hardMask(size, depth, 9);
    clipped.maskEnabled = true;
    clipped.maskLinked = false;
    clipped.maskPlacement.size = size;
    clipped.maskSource = state.layers[0].id;
    clipped.placement.size = size;

    Layer adjustment;
    adjustment.id = QStringLiteral("invert-adjustment");
    adjustment.name = QStringLiteral("invert");
    adjustment.parent = group.id;
    adjustment.metadata["adjustment"] = QJsonObject{{"kind", "Invert"}};
    adjustment.opacity = .57;
    adjustment.placement.size = size;

    state.layers[0].parent = group.id;
    state.layers.append(clipped);
    state.layers.append(adjustment);
    state.layers.append(group);
    return state;
}

State nestedFolderState(int depth, bool outerFirst = false) {
    auto state = basicState(depth, {96, 72});
    state.id = QStringLiteral("GpuTextureNestedFolders");
    Layer paint = state.layers[0];
    paint.id = QStringLiteral("folder-paint");
    paint.parent = QStringLiteral("inner-folder");
    paint.opacity = .88;

    Layer inner;
    inner.id = QStringLiteral("inner-folder");
    inner.name = QStringLiteral("inner folder");
    inner.group = true;
    inner.parent = QStringLiteral("outer-folder");
    inner.opacity = .83;
    inner.placement.size = state.size;
    inner.metadata["effects"] = QJsonObject{
        {"colorOverlay", QJsonObject{{"red", .18}, {"green", .72}, {"blue", .36}, {"opacity", .22}}}};

    Layer outer;
    outer.id = QStringLiteral("outer-folder");
    outer.name = QStringLiteral("outer folder");
    outer.group = true;
    outer.opacity = .77;
    outer.placement.size = state.size;
    outer.metadata["effects"] = QJsonObject{
        {"shadow", QJsonObject{{"blur", 2.}, {"distance", 3.}, {"angle", 90.}, {"opacity", .28},
                                {"red", .08}, {"green", .12}, {"blue", .2}}}};

    state.layers.clear();
    if (outerFirst) {
        state.layers.append(outer);
        state.layers.append(inner);
        state.layers.append(paint);
    } else {
        state.layers.append(paint);
        state.layers.append(inner);
        state.layers.append(outer);
    }
    state.active = paint.id;
    return state;
}

int maxDifference(const QImage &actual, const QImage &expected, int depth) {
    if (actual.isNull() || expected.isNull() || actual.size() != expected.size()) return depth == 16 ? 65535 : 255;
    const auto left = actual.convertToFormat(pixelFormat(depth));
    const auto right = expected.convertToFormat(pixelFormat(depth));
    int maximum = 0;
    for (int y = 0; y < left.height(); ++y) {
        for (int x = 0; x < left.width(); ++x) {
            if (depth == 16) {
                const auto a = reinterpret_cast<const QRgba64 *>(left.constScanLine(y))[x];
                const auto b = reinterpret_cast<const QRgba64 *>(right.constScanLine(y))[x];
                maximum = std::max({maximum, std::abs(int(a.red()) - int(b.red())),
                                    std::abs(int(a.green()) - int(b.green())),
                                    std::abs(int(a.blue()) - int(b.blue())),
                                    std::abs(int(a.alpha()) - int(b.alpha()))});
            } else {
                const auto a = reinterpret_cast<const QRgb *>(left.constScanLine(y))[x];
                const auto b = reinterpret_cast<const QRgb *>(right.constScanLine(y))[x];
                maximum = std::max({maximum, std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)),
                                    std::abs(qBlue(a) - qBlue(b)), std::abs(qAlpha(a) - qAlpha(b))});
            }
        }
    }
    return maximum;
}

struct DifferenceInfo {
    int maximum = 0;
    QPoint point;
    int channel = -1;
    int actual = 0;
    int expected = 0;
};

DifferenceInfo differenceInfo(const QImage &actual, const QImage &expected, int depth) {
    DifferenceInfo result;
    if (actual.isNull() || expected.isNull() || actual.size() != expected.size()) {
        result.maximum = depth == 16 ? 65535 : 255;
        return result;
    }
    const auto left = actual.convertToFormat(pixelFormat(depth));
    const auto right = expected.convertToFormat(pixelFormat(depth));
    for (int y = 0; y < left.height(); ++y) {
        for (int x = 0; x < left.width(); ++x) {
            int values[4]{};
            int reference[4]{};
            if (depth == 16) {
                const auto a = reinterpret_cast<const QRgba64 *>(left.constScanLine(y))[x];
                const auto b = reinterpret_cast<const QRgba64 *>(right.constScanLine(y))[x];
                values[0] = a.red(); values[1] = a.green(); values[2] = a.blue(); values[3] = a.alpha();
                reference[0] = b.red(); reference[1] = b.green(); reference[2] = b.blue(); reference[3] = b.alpha();
            } else {
                const auto a = reinterpret_cast<const QRgb *>(left.constScanLine(y))[x];
                const auto b = reinterpret_cast<const QRgb *>(right.constScanLine(y))[x];
                values[0] = qRed(a); values[1] = qGreen(a); values[2] = qBlue(a); values[3] = qAlpha(a);
                reference[0] = qRed(b); reference[1] = qGreen(b); reference[2] = qBlue(b); reference[3] = qAlpha(b);
            }
            for (int channel = 0; channel < 4; ++channel) {
                const int difference = std::abs(values[channel] - reference[channel]);
                if (difference > result.maximum) {
                    result.maximum = difference;
                    result.point = {x, y};
                    result.channel = channel;
                    result.actual = values[channel];
                    result.expected = reference[channel];
                }
            }
        }
    }
    return result;
}

struct TextureReadback {
    QImage image;
    bool attempted = false;
};

TextureReadback readTextureForTest(const std::shared_ptr<GpuPreviewTexture> &texture) {
    TextureReadback result;
#ifndef Q_OS_WIN
    Q_UNUSED(texture);
    return result;
#else
    if (!texture || !texture->native || !texture->native->texture) return result;
    result.attempted = true;
    runGpuNative([&](ID3D11Device *device, ID3D11DeviceContext *context, quint64) {
        GpuNativeResult stats;
        D3D11_TEXTURE2D_DESC description{};
        texture->native->texture->GetDesc(&description);
        if (description.Format != DXGI_FORMAT_R32G32B32A32_FLOAT ||
            description.Width != UINT(texture->size.width()) || description.Height != UINT(texture->size.height()))
            return stats;
        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        description.MiscFlags = 0;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device->CreateTexture2D(&description, nullptr, &staging))) return stats;
        context->CopyResource(staging.Get(), texture->native->texture.Get());
        context->Flush();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return stats;
        QImage image(texture->size, pixelFormat(texture->depth));
        if (image.isNull()) {
            context->Unmap(staging.Get(), 0);
            return stats;
        }
        const int maximum = texture->depth == 16 ? 65535 : 255;
        for (int y = 0; y < image.height(); ++y) {
            const auto *row = reinterpret_cast<const float *>(static_cast<const char *>(mapped.pData) +
                                                               size_t(y) * mapped.RowPitch);
            for (int x = 0; x < image.width(); ++x) {
                const auto sample = [&](int channel) {
                    return qRound(std::clamp(double(row[x * 4 + channel]), 0., 1.) * maximum);
                };
                if (texture->depth == 16)
                    reinterpret_cast<QRgba64 *>(image.scanLine(y))[x] = rgba64(
                        sample(0), sample(1), sample(2), sample(3));
                else
                    reinterpret_cast<QRgb *>(image.scanLine(y))[x] = qRgba(
                        sample(0), sample(1), sample(2), sample(3));
            }
        }
        context->Unmap(staging.Get(), 0);
        image.setColorSpace(QColorSpace::SRgb);
        stats.readbackBytes = quint64(image.width()) * image.height() * 16;
        result.image = image;
        return stats;
    });
    return result;
#endif
}

std::shared_ptr<GpuPreviewTexture> requireTexture(const State &state, const char *where) {
    Q_UNUSED(where);
    const auto texture = renderGpuTexture(state, state.size);
    if (texture && texture->native && texture->native->shaderResourceView) return texture;
    return {};
}

void failIfGpuRequired(const char *where) {
    if (qEnvironmentVariableIntValue("COMPOSITOR_REQUIRE_GPU"))
        QFAIL(qPrintable(QStringLiteral("%1: %2").arg(QString::fromLatin1(where), gpuPreviewStatus())));
}

} // namespace

class GpuTextureTests final : public QObject {
    Q_OBJECT

  private slots:
    void textureSnapshotHasNoReadbackAndReusesOwner() {
        for (const int depth : {8, 16}) {
            clearGpuPreviewCache();
            resetGpuPreviewMetrics();
            auto state = basicState(depth);
            const auto first = requireTexture(state, depth == 8 ? "8-bit texture" : "16-bit texture");
            if (!first) {
                failIfGpuRequired(depth == 8 ? "8-bit texture" : "16-bit texture");
                QSKIP("Direct3D 11 texture preview is unavailable on this runner.");
            }
            const auto owner = first->native;
            const auto metrics = gpuPreviewMetrics();
            QCOMPARE(metrics.readbacks, quint64(0));
            QVERIFY(metrics.noReadbackFrames > 0);
            QVERIFY(metrics.textureFrameCopies > 0);
            const auto captured = readTextureForTest(first);
            QVERIFY(!captured.image.isNull());
            QVERIFY(maxDifference(captured.image, render(state, state.size), depth) <= (depth == 8 ? 2 : 16));

            resetGpuPreviewMetrics();
            const auto second = renderGpuTexture(state, state.size);
            QVERIFY(second && second->native);
            QCOMPARE(second->native->shaderResourceView.Get(), owner->shaderResourceView.Get());
            QCOMPARE(gpuPreviewMetrics().readbacks, quint64(0));
            QCOMPARE(gpuPreviewMetrics().textureFrameCopies, quint64(0));
            clearGpuPreviewCache();
            QVERIFY(owner->shaderResourceView);
        }
    }

    void maskAdjustmentClippingAndGroupOpacityMatchCpu() {
        for (const int depth : {8, 16}) {
            clearGpuPreviewCache();
            resetGpuPreviewMetrics();
            const auto state = maskedAdjustmentState(depth);
            const auto texture = requireTexture(state, "mask/adjustment texture");
            if (!texture) {
                failIfGpuRequired("mask/adjustment texture");
                QSKIP("Direct3D 11 texture preview is unavailable on this runner.");
            }
            QCOMPARE(gpuPreviewMetrics().readbacks, quint64(0));
            QVERIFY(gpuPreviewMetrics().noReadbackFrames > 0);
            const auto capture = readTextureForTest(texture);
            QVERIFY(!capture.image.isNull());
            const auto expected = render(state, state.size);
            const int difference = maxDifference(capture.image, expected, depth);
            qInfo() << "texture mask/adjustment depth" << depth << "difference" << difference;
            QVERIFY2(difference <= (depth == 8 ? 2 : 16),
                     qPrintable(QStringLiteral("depth=%1 difference=%2").arg(depth).arg(difference)));
            QVERIFY(gpuPreviewMetrics().readbacks > 0); // only the explicit test capture read back.
        }
    }

    void nestedFolderEffectsUseGpuOwnerOrExplicitFallback() {
        for (const int depth : {8, 16}) {
            for (const bool outerFirst : {false, true}) {
            clearGpuPreviewCache();
            resetGpuPreviewMetrics();
            auto state = nestedFolderState(depth, outerFirst);
            const auto texture = renderGpuTexture(state, state.size);
            if (!texture || !texture->native) {
                // A sequential/global effect may conservatively select the
                // reference renderer.  This is an explicit no-raster-texture
                // fallback, not a failed CPU result or a hidden readback.
                QVERIFY(gpuPreviewMetrics().readbacks == 0);
                QVERIFY(!render(state, state.size).isNull());
                if (qEnvironmentVariableIntValue("COMPOSITOR_REQUIRE_GPU"))
                    QFAIL("nested folder effects unexpectedly returned no GPU texture");
                QSKIP("nested folder effects selected the reference CPU fallback");
            }
            QCOMPARE(gpuPreviewMetrics().readbacks, quint64(0));
            QVERIFY(gpuPreviewMetrics().noReadbackFrames > 0);
            const auto capture = readTextureForTest(texture);
            QVERIFY(!capture.image.isNull());
            const auto expected = render(state, state.size);
            const int difference = maxDifference(capture.image, expected, depth);
            qInfo() << "nested folder effects depth" << depth << "outerFirst" << outerFirst
                    << "difference" << difference;
            if (difference > (depth == 8 ? 2 : 16)) {
                const auto nativeInfo = differenceInfo(capture.image, expected, depth);
                const auto legacy = renderGpuPreview(state, state.size);
                const auto legacyInfo = differenceInfo(legacy, expected, depth);
                const auto crossInfo = differenceInfo(capture.image, legacy, depth);
                qInfo().noquote() << QStringLiteral("FOLDER_DIAGNOSTIC depth=%1 outerFirst=%2 native=%3@(%4,%5) ch=%6 %7/%8 legacy=%9@(%10,%11) ch=%12 %13/%14 cross=%15")
                                         .arg(depth).arg(outerFirst).arg(nativeInfo.maximum)
                                         .arg(nativeInfo.point.x()).arg(nativeInfo.point.y()).arg(nativeInfo.channel)
                                         .arg(nativeInfo.actual).arg(nativeInfo.expected).arg(legacyInfo.maximum)
                                         .arg(legacyInfo.point.x()).arg(legacyInfo.point.y()).arg(legacyInfo.channel)
                                         .arg(legacyInfo.actual).arg(legacyInfo.expected).arg(crossInfo.maximum);
            }
            QVERIFY2(difference <= (depth == 8 ? 2 : 16),
                     qPrintable(QStringLiteral("depth=%1 difference=%2").arg(depth).arg(difference)));

            // A later revision must publish a new immutable owner without
            // mutating the nested-folder snapshot retained by the caller.
            const auto oldOwner = texture->native;
            state.layers[state.index(QStringLiteral("folder-paint"))].image = pattern(state.size, depth, 91);
            const auto revised = renderGpuTexture(state, state.size);
            if (!revised || !revised->native) {
                if (qEnvironmentVariableIntValue("COMPOSITOR_REQUIRE_GPU"))
                    QFAIL("nested folder revision lost its GPU texture");
                QSKIP("nested folder revision selected the reference fallback");
            }
            QVERIFY(revised->native->shaderResourceView.Get() != oldOwner->shaderResourceView.Get());
            const auto oldAgain = readTextureForTest(texture).image;
            QVERIFY(maxDifference(oldAgain, capture.image, depth) == 0);
            }
        }
    }

    void switchingLegacyCpuFrameDoesNotReturnStaleTexture() {
        clearGpuPreviewCache();
        resetGpuPreviewMetrics();
        auto state = basicState(8);
        const auto first = requireTexture(state, "revision texture");
        if (!first) {
            failIfGpuRequired("revision texture");
            QSKIP("Direct3D 11 texture preview is unavailable on this runner.");
        }
        const auto firstCapture = readTextureForTest(first).image;
        state.layers[0].image.fill(QColor(20, 188, 73, 255));
        const auto cpu = renderGpuPreview(state, state.size);
        QVERIFY(!cpu.isNull());
        resetGpuPreviewMetrics();
        const auto second = renderGpuTexture(state, state.size);
        QVERIFY(second && second->native);
        QVERIFY(second->native->shaderResourceView.Get() != first->native->shaderResourceView.Get());
        QCOMPARE(gpuPreviewMetrics().readbacks, quint64(0));
        const auto secondCapture = readTextureForTest(second).image;
        QVERIFY(maxDifference(firstCapture, secondCapture, 8) > 20);
        QVERIFY(maxDifference(secondCapture, cpu, 8) <= 2);
    }

    void externalOwnerSurvivesCacheClearAndBudgetAccounting() {
        clearGpuPreviewCache();
        resetGpuPreviewMetrics();
        const auto state = basicState(8, {96, 64});
        auto texture = requireTexture(state, "external owner");
        if (!texture) {
            failIfGpuRequired("external owner");
            QSKIP("Direct3D 11 texture preview is unavailable on this runner.");
        }
        auto owner = texture->native;
        QVERIFY(owner->bytes > 0);
        clearGpuPreviewCache();
        const auto retained = gpuPreviewMetrics();
        QVERIFY(retained.externalBytes >= owner->bytes);
        QVERIFY(retained.residentBytes >= owner->bytes);
        QVERIFY(owner->shaderResourceView);
        texture.reset();
        QVERIFY(owner->shaderResourceView);
        QVERIFY(gpuPreviewMetrics().externalBytes >= owner->bytes);
        owner.reset();
        QCOMPARE(gpuPreviewMetrics().externalBytes, quint64(0));
    }

    void texturePreviewTilesStayNativeAndExposeUvCrop() {
        clearGpuPreviewCache();
        resetGpuPreviewMetrics();
        auto state = basicState(8, {1024, 768});
        Layer blur;
        blur.id = QStringLiteral("tile-blur");
        blur.name = QStringLiteral("tile blur");
        blur.metadata["adjustment"] = QJsonObject{{"kind", "Gaussian Blur"}, {"blurRadius", 6.}};
        blur.placement.size = state.size;
        state.layers.append(blur);
        const auto overviewProbe = renderGpuTexture(state, {256, 192});
        if (!overviewProbe || !overviewProbe->native) {
            failIfGpuRequired("tile texture preview");
            QSKIP("Direct3D 11 texture preview is unavailable for tile rendering.");
        }
        clearGpuPreviewCache();
        resetGpuPreviewMetrics();
        const auto cancelled = std::make_shared<std::atomic_bool>(false);
        const QRect visible(600, 180, 128, 128);
        const auto preview = renderCanvasPreview(state, visible, {256, 192}, true, 11, {}, cancelled, true);
        QVERIFY2(preview.overview.isNull(), "texture preview unexpectedly materialized a CPU overview");
        QVERIFY(preview.overviewTexture);
        QVERIFY(!preview.tiles.isEmpty());
        QVERIFY(gpuPreviewMetrics().readbacks == 0);
        for (const auto &tile : preview.tiles) {
            QVERIFY(tile.image.isNull());
            QVERIFY(tile.texture && tile.texture->native);
            QVERIFY(tile.textureSource.size() == tile.rectangle.size());
            QVERIFY(tile.textureSource.x() >= 0 && tile.textureSource.y() >= 0);
            QVERIFY(tile.textureSource.right() < tile.texture->size.width());
            QVERIFY(tile.textureSource.bottom() < tile.texture->size.height());
            QVERIFY2(tile.textureSource != QRect(QPoint(), tile.texture->size),
                     "tile did not expose a halo/full-frame UV crop");
        }
    }
};

QTEST_MAIN(GpuTextureTests)
#include "GpuTextureTests.moc"
