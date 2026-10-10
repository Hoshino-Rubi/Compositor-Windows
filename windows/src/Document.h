#pragma once
#include "Project.h"
#include <QObject>
#include <functional>
#include <optional>
namespace comp {
class Document : public QObject {
    Q_OBJECT
  public:
    explicit Document(QObject *parent = nullptr);
    const State &state() const { return state_; }
    State &draft() { return state_; }
    bool hasDocument() const { return !state_.id.isEmpty(); }
    bool dirty() const { return revision_ != cleanRevision_; }
    bool canUndo() const { return !undo_.isEmpty(); }
    bool canRedo() const { return !redo_.isEmpty(); }
    quint64 revision() const { return revision_; }
    void reset(State state, bool saved = false);
    void markSaved(quint64 revision);
    void select(const QString &id);
    void edit(const std::function<void(State &)> &change);
    void begin();
    void preview();
    void commit();
    void cancel();
    void undo();
    void redo();
    void addLayer(const QString &name, const QImage &image = {}, bool group = false);
    void duplicate(const QString &name);
    void remove();
    void reorder(int direction);
    void moveBy(QPointF delta);
    void crop(QRect rect);
    struct Snapshot {
        State state;
        quint64 revision;
    };
    struct Session {
        State state;
        QVector<Snapshot> undo, redo;
        quint64 revision = 0, cleanRevision = 0, serial = 0;
    };
    Session session() const;
    void restore(const Session &session);
    void setSelection(QPainterPath path, int mode = 0);
    void deselect();
    void selectAll();
    void invertSelection();
    void selectPixels(bool mask = false);
    void modifySelection(double amount);
    void fill(QColor color, bool erase = false, bool mask = false);
    QImage copyPixels(bool merged = false) const;
    void pastePixels(const QImage &image, const QString &name);
    void applyFilter(const QJsonObject &settings, bool mask = false);
    void maskAction(const QString &action);
    void groupLayers(const QStringList &ids, const QString &name);
    void ungroup();
    void relocate(const QString &id, const QString &parent, const QString &before = {});
    void merge(const QStringList &ids);
    void resizeCanvas(QSize size, bool rescale, bool centered = true);
    void flipCanvas(bool horizontal);
    void trim();
    void addGuide(bool horizontal, double position);
    void contentFill();
    void featherSelection(double radius);
    void setSelectionMask(const QImage &mask);

  signals:
    void changed();
    void selectionChanged();
    void visualChanged();

  private:
    State state_;
    QVector<Snapshot> undo_, redo_;
    std::optional<Snapshot> before_;
    quint64 revision_ = 0, cleanRevision_ = 0, serial_ = 0;
    void trimHistory();
};
} // namespace comp
