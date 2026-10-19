#include "Pixels.h"
#include "Selection.h"
#include "Filters.h"
#include <onnxruntime_cxx_api.h>
#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QPainter>
#include <QMutex>
#include <QMutexLocker>
#include <queue>
#include <cstring>
namespace comp {
static QSize analysisSize(QSize size, int limit) {
    const auto fitted=size.scaled(QSize(limit,limit),Qt::KeepAspectRatio);
    return {std::max(1,fitted.width()),std::max(1,fitted.height())};
}
static QImage inferSubjectMask(const QImage &source) {
    static QMutex mutex;
    QMutexLocker lock(&mutex);
    static Ort::Env environment(ORT_LOGGING_LEVEL_WARNING, "Compositor");
    static std::unique_ptr<Ort::Session> session;
    if (!session) {
        QString path = qEnvironmentVariable("COMPOSITOR_SUBJECT_MODEL");
        if (path.isEmpty())
            path = QCoreApplication::applicationDirPath() + "/models/u2netp.onnx";
        if (!QFile::exists(path))
            path = QString::fromUtf8(COMPOSITOR_MODEL_PATH);
        if (!QFile::exists(path))
            throw Error("modelMissing");
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(2);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        session = std::make_unique<Ort::Session>(environment, path.toStdWString().c_str(), options);
    }
    // Preserve aspect ratio and composite transparency before model inference.
    const auto scaled = source.scaled(analysisSize(source.size(),320), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QImage image(320, 320, QImage::Format_RGB32);
    image.fill(Qt::black);
    const QRect content(QPoint((320-scaled.width())/2, (320-scaled.height())/2), scaled.size());
    { QPainter painter(&image); painter.drawImage(content.topLeft(), scaled); }
    std::vector<float> input(3 * 320 * 320);
    const float mean[]{.485f, .456f, .406f}, stddev[]{.229f, .224f, .225f};
    float maximum = 1;
    for (int y = 0; y < 320; ++y) {
        auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < 320; ++x)
            maximum = std::max(maximum, float(std::max({qRed(row[x]), qGreen(row[x]), qBlue(row[x])})));
    }
    for (int y = 0; y < 320; ++y) {
        auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < 320; ++x) {
            const float values[]{float(qRed(row[x])), float(qGreen(row[x])), float(qBlue(row[x]))};
            for (int c = 0; c < 3; ++c)
                input[c * 320 * 320 + y * 320 + x] = (values[c] / maximum - mean[c]) / stddev[c];
        }
    }
    std::array<int64_t, 4> shape{1, 3, 320, 320};
    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto tensor = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(), shape.data(), shape.size());
    Ort::AllocatorWithDefaultOptions allocator;
    auto inputName = session->GetInputNameAllocated(0, allocator),
         outputName = session->GetOutputNameAllocated(0, allocator);
    const char *inputs[]{inputName.get()}, *outputs[]{outputName.get()};
    auto result = session->Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 1);
    const auto *data = result[0].GetTensorData<float>();
    const auto count = result[0].GetTensorTypeAndShapeInfo().GetElementCount();
    if (count < 320 * 320)
        throw Error("imageError");
    float low = data[0], high = data[0];
    for (int i = 0; i < 320 * 320; ++i) {
        low = std::min(low, data[i]);
        high = std::max(high, data[i]);
    }
    QImage mask(320, 320, QImage::Format_Grayscale8);
    for (int y = 0; y < 320; ++y) {
        auto *row = mask.scanLine(y);
        for (int x = 0; x < 320; ++x)
            row[x] = uchar(std::clamp(qRound((data[y * 320 + x] - low) / std::max(.00001f, high - low) * 255), 0, 255));
    }
    return mask.copy(content);
}
QImage connectedObjectMask(const QImage &input, QPoint seed, int threshold) {
    const auto mask = input.convertToFormat(QImage::Format_Grayscale8);
    QImage result(mask.size(), QImage::Format_Grayscale8);
    if (result.isNull()) throw Error("imageError");
    result.fill(0);
    if (!mask.rect().contains(seed) || mask.constScanLine(seed.y())[seed.x()] < threshold) return result;
    QByteArray visited(mask.width()*mask.height(), 0);
    std::queue<QPoint> queue; queue.push(seed); visited[seed.y()*mask.width()+seed.x()] = 1;
    while (!queue.empty()) {
        const auto p=queue.front();queue.pop();result.scanLine(p.y())[p.x()]=mask.constScanLine(p.y())[p.x()];
        for (const auto delta : {QPoint(1,0),QPoint(-1,0),QPoint(0,1),QPoint(0,-1)}) {
            const auto next=p+delta;
            if (mask.rect().contains(next)&&!visited[next.y()*mask.width()+next.x()]&&mask.constScanLine(next.y())[next.x()]>=threshold) {
                visited[next.y()*mask.width()+next.x()]=1;queue.push(next);
            }
        }
    }
    return result;
}
QImage subjectMask(const QImage &source, QPoint seed) {
    if (source.isNull()) throw Error("imageError");
    const bool object=seed!=QPoint(-1,-1);
    if (object&&!source.rect().contains(seed)) throw Error("invalidSize");
    // Keep connectivity bounded; retry a missed click with a local inference.
    const QSize analysis=analysisSize(source.size(),640);
    QImage mask=inferSubjectMask(source).scaled(analysis,Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
    if (object) {
        const QPoint point(std::min(analysis.width()-1,seed.x()*analysis.width()/source.width()),
                           std::min(analysis.height()-1,seed.y()*analysis.height()/source.height()));
        mask=connectedObjectMask(mask,point,80);
        if (!mask.constScanLine(point.y())[point.x()]) {
            const int side=std::clamp(std::min(source.width(),source.height())/2,64,2048);
            const QRect crop=QRect(seed-QPoint(side/2,side/2),QSize(side,side)).intersected(source.rect());
            auto local=inferSubjectMask(source.copy(crop));
            const QSize small=analysisSize(crop.size(),640);
            local=local.scaled(small,Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
            const QPoint at((seed.x()-crop.x())*small.width()/crop.width(),(seed.y()-crop.y())*small.height()/crop.height());
            local=connectedObjectMask(local,at,100);
            // A near-solid crop is not a credible object boundary.
            qsizetype selected=0;for(int y=0;y<local.height();++y)for(int x=0;x<local.width();++x)selected+=local.constScanLine(y)[x]>0;
            if(selected>0&&selected*10<qint64(local.width())*local.height()*9) {
                QImage full(source.size(),QImage::Format_Grayscale8);if(full.isNull())throw Error("tooLarge");full.fill(0);
                const auto expanded=local.scaled(crop.size(),Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
                for(int y=0;y<crop.height();++y)std::memcpy(full.scanLine(crop.y()+y)+crop.x(),expanded.constScanLine(y),crop.width());
                mask=std::move(full);
            }
        }
    }
    mask=mask.scaled(source.size(),Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
    if (source.hasAlphaChannel()) {
        const bool rgba64=source.format()==QImage::Format_RGBA64_Premultiplied||source.format()==QImage::Format_RGBA64;
        const bool argb=source.format()==QImage::Format_ARGB32_Premultiplied||source.format()==QImage::Format_ARGB32;
        for(int y=0;y<mask.height();++y) {
            auto *dst=mask.scanLine(y);
            for(int x=0;x<mask.width();++x) {
                const quint32 alpha=rgba64?reinterpret_cast<const QRgba64 *>(source.constScanLine(y))[x].alpha():
                    quint32(argb?qAlpha(reinterpret_cast<const QRgb *>(source.constScanLine(y))[x]):source.pixelColor(x,y).alpha())*257;
                dst[x]=uchar((quint32(dst[x])*alpha+32767)/65535);
            }
        }
    }
    return mask;
}
QImage selectionCoverage(const State &state, QSize target, const Placement &placement) {
    QImage result(target, pixelFormat(state.bitDepth));
    if (result.isNull())
        throw Error("tooLarge");
    result.fill(state.selectionActive ? Qt::transparent : Qt::white);
    if (!state.selectionActive)
        return result;
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setTransform(placement.matrix(target).inverted());
    if (state.selectionMask.isNull())
        painter.fillPath(state.selection, Qt::white);
    else {
        auto alpha = alphaFromGray(state.selectionMask, state.bitDepth);
        painter.drawImage(0, 0, alpha);
    }
    return result;
}
void constrainSelection(const State &state, const Placement &placement, const QImage &before, QImage &after) {
    if (!state.selectionActive)
        return;
    const auto coverage = selectionCoverage(state, after.size(), placement);
    if (hdrImage(before) || hdrImage(after)) {
        after=after.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
        const auto original=before.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
        const auto mask=coverage.convertToFormat(QImage::Format_RGBA64_Premultiplied);
        for(int y=0;y<after.height();++y){auto *out=reinterpret_cast<FloatPixel *>(after.scanLine(y));
            const auto *base=reinterpret_cast<const FloatPixel *>(original.constScanLine(y));const auto *m=reinterpret_cast<const QRgba64 *>(mask.constScanLine(y));
            for(int x=0;x<after.width();++x){const float a=m[x].alpha()/65535.f,b=1-a;
                out[x]={out[x].r*a+base[x].r*b,out[x].g*a+base[x].g*b,out[x].b*a+base[x].b*b,out[x].a*a+base[x].a*b};}}
        return;
    }
    if (state.bitDepth == 16) {
        after = after.convertToFormat(pixelFormat(16));
        const auto original = before.convertToFormat(pixelFormat(16));
        for (int y = 0; y < after.height(); ++y) {
            auto *dst = reinterpret_cast<QRgba64 *>(after.scanLine(y));
            const auto *src = reinterpret_cast<const QRgba64 *>(original.constScanLine(y));
            const auto *mask = reinterpret_cast<const QRgba64 *>(coverage.constScanLine(y));
            for (int x = 0; x < after.width(); ++x) {
                const quint64 a = mask[x].alpha(), b = 65535 - a;
                auto mix = [&](int d, int s) { return int((d * a + s * b + 32767) / 65535); };
                dst[x] = rgba64(mix(dst[x].red(), src[x].red()), mix(dst[x].green(), src[x].green()),
                                mix(dst[x].blue(), src[x].blue()), mix(dst[x].alpha(), src[x].alpha()));
            }
        }
        return;
    }
    after = after.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    auto original = before.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < after.height(); ++y) {
        auto *dst = reinterpret_cast<QRgb *>(after.scanLine(y));
        const auto *src = reinterpret_cast<const QRgb *>(original.constScanLine(y)),
                   *mask = reinterpret_cast<const QRgb *>(coverage.constScanLine(y));
        for (int x = 0; x < after.width(); ++x) {
            int a = qAlpha(mask[x]), b = 255 - a;
            dst[x] = qRgba((qRed(dst[x]) * a + qRed(src[x]) * b + 127) / 255,
                           (qGreen(dst[x]) * a + qGreen(src[x]) * b + 127) / 255,
                           (qBlue(dst[x]) * a + qBlue(src[x]) * b + 127) / 255,
                           (qAlpha(dst[x]) * a + qAlpha(src[x]) * b + 127) / 255);
        }
    }
}
} // namespace comp
