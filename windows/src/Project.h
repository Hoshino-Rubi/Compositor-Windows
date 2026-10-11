#pragma once
#include <QImage>
#include <QPainterPath>
#include <QJsonObject>
#include <QJsonArray>
#include <QPointF>
#include <QTransform>
#include <QVector>
#include <QStringList>
#include <stdexcept>
namespace comp {
struct ProjectManifest {
    static constexpr int current = 12;
    static constexpr int eightBit = 11;
};
inline constexpr int MaxSide = 16384;
inline constexpr qint64 MaxCanvasPixels = 64000000;
inline constexpr qint64 MaxSourcePixels = 100000000;
inline constexpr int MaxLayers = 1000;
struct Error : std::runtime_error {
    QString key, detail;
    explicit Error(QString key, QString detail = {});
};
struct Placement {
    QPointF origin;
    QSizeF size{1, 1};
    double rotation = 0;
    bool flipX = false, flipY = false;
    QString sampling = "High quality";
    QTransform matrix(QSize source) const;
    QJsonObject json() const;
    static Placement fromJson(const QJsonObject &object);
};
struct Layer {
    QString id, name, parent;
    bool visible = true, group = false;
    double opacity = 1;
    QString blend = "Normal";
    Placement placement;
    QImage image, mask;
    QByteArray originalPng, originalMaskPng;
    bool maskEnabled = true, maskLinked = true;
    Placement maskPlacement;
    QString maskSource;
    QJsonObject metadata;
};
struct State {
    QString id;
    QSize size{1200, 800};
    double resolution = 72;
    int bitDepth = 8;
    QString active;
    QPainterPath selection;
    QImage selectionMask;
    bool selectionActive = false;
    QVector<Layer> layers;
    QJsonArray guides;
    QJsonObject metadata;
    int index(const QString &id) const;
    QVector<int> descendants(const QString &id) const;
    QVector<int> drawOrder() const;
    qint64 sourcePixels() const;
};
struct LoadedProject {
    State state;
    QByteArray digest;
};
QString newId();
QStringList blendModes();
void validateState(const State &state);
LoadedProject readProject(const QString &directory);
QByteArray projectDigest(const QString &directory);
QByteArray writeProject(const QString &directory, const State &state, const QByteArray &expectedDigest = {});
QImage readImage(const QString &path);
QByteArray pngBytes(const QImage &image, double resolution = 72);
} // namespace comp
