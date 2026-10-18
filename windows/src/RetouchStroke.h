#pragma once
#include <QImage>
#include <QPointF>
#include <QVector>
#include <memory>
namespace comp {
// Per-gesture state: liquify samples the untouched source through an accumulated
// displacement field; smudge carries the color it just painted, not an old stamp.
class RetouchStroke {
  public:
    RetouchStroke(const QImage &source, double radius, double hardness, double strength, bool smudge,
                  const QImage &coverage = {});
    ~RetouchStroke();
    void append(QImage &image, const QVector<QPointF> &points);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace comp
