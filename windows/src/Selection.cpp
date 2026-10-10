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
namespace comp {
QImage subjectMask(const QImage &source, QPoint seed) {
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
    const auto image =
        source.scaled(320, 320, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB32);
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
    if (seed.x() >= 0 && seed.y() >= 0) {
        QPoint point(seed.x() * 320 / source.width(), seed.y() * 320 / source.height());
        if (!mask.rect().contains(point) || mask.pixelColor(point).red() < 80) {
            mask.fill(Qt::black);
        } else {
            QByteArray visited(320 * 320, 0);
            std::queue<QPoint> queue;
            queue.push(point);
            visited[point.y() * 320 + point.x()] = 1;
            while (!queue.empty()) {
                auto p = queue.front();
                queue.pop();
                for (auto delta : {QPoint(1, 0), QPoint(-1, 0), QPoint(0, 1), QPoint(0, -1)}) {
                    auto next = p + delta;
                    if (mask.rect().contains(next) && !visited[next.y() * 320 + next.x()] &&
                        mask.constScanLine(next.y())[next.x()] >= 80) {
                        visited[next.y() * 320 + next.x()] = 1;
                        queue.push(next);
                    }
                }
            }
            for (int y = 0; y < 320; ++y)
                for (int x = 0; x < 320; ++x)
                    if (!visited[y * 320 + x])
                        mask.scanLine(y)[x] = 0;
        }
    }
    return mask.scaled(source.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}
QImage selectionCoverage(const State &state, QSize target, const Placement &placement) {
    QImage result(target, QImage::Format_ARGB32_Premultiplied);
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
        QImage alpha(state.selectionMask.size(), QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < alpha.height(); ++y) {
            const auto *src = state.selectionMask.constScanLine(y);
            auto *dst = reinterpret_cast<QRgb *>(alpha.scanLine(y));
            for (int x = 0; x < alpha.width(); ++x)
                dst[x] = qRgba(src[x], src[x], src[x], src[x]);
        }
        painter.drawImage(0, 0, alpha);
    }
    return result;
}
void constrainSelection(const State &state, const Placement &placement, const QImage &before, QImage &after) {
    if (!state.selectionActive)
        return;
    const auto coverage = selectionCoverage(state, after.size(), placement);
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
