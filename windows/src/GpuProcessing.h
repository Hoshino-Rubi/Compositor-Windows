#pragma once
#include "Project.h"
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <array>
#include <d3d11.h>
#include <map>
#include <memory>
#include <vector>
#include <wrl/client.h>
namespace comp {
struct GpuSurface {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> output;
    QSize size;
};
using GpuSurfacePtr = std::shared_ptr<GpuSurface>;
class GpuProcessor {
  public:
    GpuProcessor(ID3D11Device *device, ID3D11DeviceContext *context);
    static bool supports(const QString &kind);
    static bool prefersCpu(const QJsonObject &settings, int clipping = 0);
    GpuSurfacePtr filter(ID3D11ShaderResourceView *input, QSize size, int depth, const QJsonObject &settings,
                         double scale = 1, int clipping = 0);
    GpuSurfacePtr effects(ID3D11ShaderResourceView *input, QSize size, int depth, const QJsonObject &settings,
                          double scale);
    GpuSurfacePtr coverage(const GpuSurfacePtr &input, ID3D11ShaderResourceView *mask, int depth, double opacity);
    void setScratchBudget(quint64 bytes) { scratchBudget_ = std::min<quint64>(256 * 1024 * 1024, bytes); }
    quint64 dispatches = 0;
    QString error;

  private:
    using Params = std::array<float, 256>;
    quint64 scratchBudget_ = 256 * 1024 * 1024;
    bool precisePredicates_ = false;
    ID3D11Device *device_;
    ID3D11DeviceContext *context_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    std::map<QByteArray, Microsoft::WRL::ComPtr<ID3D11ComputeShader>> shaders_;
    std::vector<GpuSurfacePtr> pool_;
    GpuSurfacePtr alphaFactors_[2];
    GpuSurfacePtr alphaFactors();
    Microsoft::WRL::ComPtr<ID3D11Buffer> histogramBuffer_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> histogramOutput_;
    std::array<float, 3> haze(const GpuSurfacePtr &input);
    QSize size_;
    int depth_ = 8;
    GpuSurfacePtr surface(QSize size);
    GpuSurfacePtr pass(const char *entry, const std::vector<GpuSurfacePtr> &inputs, Params params = {},
                       QSize size = {});
    GpuSurfacePtr blur(const GpuSurfacePtr &input, double radius);
    GpuSurfacePtr resample(const GpuSurfacePtr &input, QSize size, const QTransform &transform, bool antialias = false);
    GpuSurfacePtr adjustment(const GpuSurfacePtr &input, const QJsonObject &settings);
    GpuSurfacePtr upload(const QVector<float> &rgba, QSize size);
    QVector<float> download(const GpuSurfacePtr &image);
    GpuSurfacePtr cameraRaw(const GpuSurfacePtr &input, const QJsonObject &settings, double scale, int clipping);
    GpuSurfacePtr rawGeometry(const GpuSurfacePtr &input, const QJsonObject &settings);
    GpuSurfacePtr rawEffects(const GpuSurfacePtr &input, const QJsonObject &settings, double scale);
    GpuSurfacePtr rawDetail(const GpuSurfacePtr &input, const QJsonObject &settings, double scale, bool mask);
    GpuSurfacePtr rawOptics(const GpuSurfacePtr &input, const QJsonObject &settings, double scale);
    GpuSurfacePtr floatBox(const GpuSurfacePtr &input, int radius);
    GpuSurfacePtr guided(const GpuSurfacePtr &input, int radius);
    std::array<double, 3> statistics(const GpuSurfacePtr &input);
    GpuSurfacePtr dither(const GpuSurfacePtr &input, const QJsonObject &settings);
    GpuSurfacePtr scanlines(const GpuSurfacePtr &input, const QJsonObject &settings);
    GpuSurfacePtr standalone(const GpuSurfacePtr &input, const QJsonObject &settings);
};
} // namespace comp
#endif
