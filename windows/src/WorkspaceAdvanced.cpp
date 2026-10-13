#include "Pixels.h"
#include "MainWindow.h"
#include "Selection.h"
#include "Renderer.h"
#include "Warp.h"
#include "GpuPerspective.h"
#include "Preferences.h"
#include <QtConcurrent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTableWidget>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QSettings>
#include <QMessageBox>
#include <QStatusBar>
#include <QPainter>
#include <QApplication>
#include <QDirIterator>
#include <QCryptographicHash>
#include <QDateTime>
#include <QSignalBlocker>
#include <QScreen>
namespace comp {
void MainWindow::selectSubject(QPoint point, bool remove) {
    if (!document_.hasDocument() || busy_)
        return;
    State snapshot = document_.state();
    int i = snapshot.index(snapshot.active);
    if (remove && i < 0)
        return;
    const auto source = remove ? layerSurface(snapshot, i, snapshot.size) : comp::render(snapshot);
    auto mask = std::make_shared<QImage>();
    runIO(
        [mask, source, point] {
            try {
                *mask = subjectMask(source, point);
            } catch (const std::exception &e) {
                throw Error("subjectError", QString::fromUtf8(e.what()));
            }
        },
        [this, mask, source, remove, id = snapshot.id, layer = i >= 0 ? snapshot.layers[i].id : QString()] {
            if (document_.state().id != id) return;
            if (remove)
                backgroundDialog(*mask, source, layer, id);
            else
                document_.setSelectionMask(*mask);
        });
}
void MainWindow::backgroundDialog(QImage mask, QImage source, QString layerId, QString documentId) {
    QDialog dialog(this);
    dialog.setObjectName("backgroundDialog");
    dialog.setWindowTitle(t("removeBackground"));
    dialog.resize(630, 560);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    QComboBox quality;
    quality.setObjectName("backgroundQuality");
    quality.addItems({t("backgroundBasic"), t("backgroundAdvanced")});
    form->addRow(t("quality"), &quality);
    QSpinBox radius, contrast, shift;
    radius.setObjectName("refineEdges"); radius.setRange(0, 40); radius.setValue(12);
    contrast.setObjectName("matteContrast"); contrast.setRange(0, 100); contrast.setValue(25);
    shift.setObjectName("shiftEdge"); shift.setRange(-10, 10);
    form->addRow(t("refineEdges"), &radius);
    form->addRow(t("matteContrast"), &contrast);
    form->addRow(t("shiftEdge"), &shift);
    layout->addLayout(form);
    auto *preview = new QLabel;
    preview->setObjectName("backgroundPreview");
    preview->setMinimumSize(380, 320);
    preview->setAlignment(Qt::AlignCenter);
    preview->setStyleSheet("background:#282828;border:1px solid #555;");
    layout->addWidget(preview, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto refresh = [&] {
        const bool advanced = quality.currentIndex() == 1;
        radius.setEnabled(advanced);
        contrast.setEnabled(advanced);
        shift.setEnabled(advanced);
        const QSize size = source.size().scaled(440, 380, Qt::KeepAspectRatio);
        QImage image = source.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                             .convertToFormat(QImage::Format_ARGB32_Premultiplied);
        const QImage initial = mask.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        const QImage coverage = advanced ? refineSubjectMask(image, initial, radius.value(), contrast.value(), shift.value())
                                         : initial;
        QPainter p(&image);
        p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        p.drawImage(0, 0, alphaFromGray(coverage, 8));
        p.end();
        preview->setPixmap(QPixmap::fromImage(image));
    };
    connect(&quality, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, refresh);
    for (auto *field : {&radius, &contrast, &shift})
        connect(field, qOverload<int>(&QSpinBox::valueChanged), &dialog, refresh);
    refresh();
    if (dialog.exec() != QDialog::Accepted) return;
    const bool advanced = quality.currentIndex() == 1;
    const int radiusValue = radius.value(), contrastValue = contrast.value(), shiftValue = shift.value();
    auto finalMask = std::make_shared<QImage>();
    runIO([finalMask, mask, source, advanced, radiusValue, contrastValue, shiftValue] {
              *finalMask = advanced ? refineSubjectMask(source, mask, radiusValue, contrastValue, shiftValue) : mask;
          },
          [this, finalMask, documentId, layerId] {
              if (document_.state().id != documentId) return;
              document_.edit([&](State &state) {
                  const int index = state.index(layerId);
                  if (index < 0) return;
                  auto &layer = state.layers[index];
                  layer.mask = *finalMask;
                  layer.maskLinked = false;
                  layer.maskEnabled = true;
                  layer.maskPlacement = {};
                  layer.maskPlacement.size = state.size;
                  layer.originalMaskPng.clear();
              });
          });
}
void MainWindow::shortcutsDialog() {
    showPreferences(true);
}
void MainWindow::showPreferences(bool shortcuts) {
    if (!preferences_->isVisible()) {
        preferences_->refresh();
        const QSize available = screen()->availableGeometry().size() - QSize(48, 64);
        preferences_->resize(preferences_->size().boundedTo(available));
        preferences_->open();
    }
    if (shortcuts)
        preferences_->showShortcuts();
    preferences_->raise();
    preferences_->activateWindow();
}
void MainWindow::applyPreferences() {
    const auto p = Preferences::load();
    const QList<QPair<QString, bool>> flags{
        {"showRulers", p.rulers}, {"showGrid", p.grid}, {"showGuides", p.guides}, {"snap", p.snap},
        {"snapGuides", p.snapGuides}, {"snapGrid", p.snapGrid}, {"snapLayers", p.snapLayers},
        {"snapBounds", p.snapBounds}, {"lockGuides", p.lockGuides}, {"pixelGrid", p.pixelGrid},
        {"transformControls", p.transformControls}};
    for (const auto &flag : flags) {
        QSignalBlocker block(actions_[flag.first]);
        actions_[flag.first]->setChecked(flag.second);
    }
    canvas_->setRulers(p.rulers);
    canvas_->setGrid(p.grid);
    canvas_->setGuides(p.guides);
    canvas_->setSnap(p.snap);
    canvas_->setSnapTargets(p.snapGuides, p.snapGrid, p.snapLayers, p.snapBounds);
    canvas_->setLockGuides(p.lockGuides);
    canvas_->setPixelGrid(p.pixelGrid);
    canvas_->setTransformControls(p.transformControls);
    canvas_->setGridSpacing(p.gridSpacing);
    if (auto *depth = findChild<QComboBox *>("welcomeBitDepth"))
        depth->setCurrentIndex(depth->findData(p.depth));
    if (p.autoReload)
        projectWatchTimer_.start();
    else
        projectWatchTimer_.stop();
}
void MainWindow::transformDialog(bool distort) {
    const auto ids = selectedLayers();
    if (ids.isEmpty())
        return;
    const State original = document_.state();
    QSet<int> indices;
    for (const auto &id : ids) {
        int i = original.index(id);
        if (i >= 0)
            indices.insert(i);
        for (int child : original.descendants(id))
            indices.insert(child);
    }
    QRectF bounds;
    for (int i : indices) {
        const auto &l = original.layers[i];
        if (l.group)
            continue;
        const QSize size = l.image.isNull() ? l.placement.size.toSize() : l.image.size();
        bounds = bounds.united(l.placement.matrix(size).mapRect(QRectF(QPointF(), size)));
    }
    if (bounds.isEmpty())
        return;
    QDialog dialog(this);
    dialog.setWindowTitle(t(distort ? "freeDistort" : "transformLayers"));
    auto *layout = new QFormLayout(&dialog);
    QVector<QDoubleSpinBox *> controls;
    auto number = [&](QString key, double value, double low, double high) {
        auto *box = new QDoubleSpinBox;
        box->setObjectName(key);
        box->setRange(low, high);
        box->setValue(value);
        box->setDecimals(2);
        layout->addRow(t(key), box);
        controls.append(box);
    };
    if (distort) {
        for (int i = 0; i < 4; ++i) {
            QPointF point = i == 0   ? bounds.topLeft()
                            : i == 1 ? bounds.topRight()
                            : i == 2 ? bounds.bottomRight()
                                     : bounds.bottomLeft();
            number("corner" + QString::number(i + 1) + "X", point.x(), -1e6, 1e6);
            number("corner" + QString::number(i + 1) + "Y", point.y(), -1e6, 1e6);
        }
    } else {
        number("x", bounds.x(), -1e6, 1e6);
        number("y", bounds.y(), -1e6, 1e6);
        number("scale", 100, .1, 10000);
        number("rotation", 0, -360, 360);
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    invoke([&] {
        document_.edit([&](State &s) {
            if (!distort) {
                const double scale = controls[2]->value() / 100, angle = controls[3]->value();
                QTransform mapping;
                mapping.translate(controls[0]->value() + bounds.width() * scale / 2,
                                  controls[1]->value() + bounds.height() * scale / 2);
                mapping.rotate(angle);
                mapping.scale(scale, scale);
                mapping.translate(-bounds.center().x(), -bounds.center().y());
                for (int i : indices) {
                    auto &l = s.layers[i];
                    for (auto *p : {&l.placement, &l.maskPlacement}) {
                        auto center = mapping.map(p->origin + QPointF(p->size.width() / 2, p->size.height() / 2));
                        p->size *= scale;
                        p->origin = center - QPointF(p->size.width() / 2, p->size.height() / 2);
                        p->rotation += angle;
                    }
                }
            } else {
                QPolygonF source{bounds.topLeft(), bounds.topRight(), bounds.bottomRight(), bounds.bottomLeft()},
                    target;
                for (int i = 0; i < 4; ++i)
                    target.append({controls[i * 2]->value(), controls[i * 2 + 1]->value()});
                QTransform map;
                if (!QTransform::quadToQuad(source, target, map))
                    throw Error("invalidTransform");
                for (int i : indices) {
                    auto &l = s.layers[i];
                    if (l.group || l.image.isNull())
                        continue;
                    QPolygonF corners{{0, 0},
                                      {double(l.image.width()), 0},
                                      {double(l.image.width()), double(l.image.height())},
                                      {0, double(l.image.height())}};
                    auto quad = map.map(l.placement.matrix(l.image.size()).map(corners));
                    auto box = quad.boundingRect().toAlignedRect();
                    const auto warp = Preferences::gpuEditingEnabled() ? perspectiveWarpAccelerated : perspectiveWarp;
                    l.image = warp(l.image, quad);
                    if (!l.mask.isNull()) {
                        const auto maskPlace = l.maskLinked ? l.placement : l.maskPlacement;
                        QPolygonF maskCorners{{0, 0},
                                              {double(l.mask.width()), 0},
                                              {double(l.mask.width()), double(l.mask.height())},
                                              {0, double(l.mask.height())}};
                        auto maskQuad = map.map(maskPlace.matrix(l.mask.size()).map(maskCorners));
                        auto rect = maskQuad.boundingRect().toAlignedRect();
                        l.mask = maskImage(warp(l.mask.convertToFormat(pixelFormat(s.bitDepth)), maskQuad),
                                           s.bitDepth);
                        l.maskLinked = false;
                        l.maskPlacement = {};
                        l.maskPlacement.origin = rect.topLeft();
                        l.maskPlacement.size = rect.size();
                        l.originalMaskPng.clear();
                    }
                    l.placement = {};
                    l.placement.origin = box.topLeft();
                    l.placement.size = box.size();
                    l.originalPng.clear();
                    l.metadata.remove("text");
                    l.metadata.remove("shape");
                }
            }
        });
    });
}
void MainWindow::watchProjects() {
    if (!Preferences::load().autoReload)
        return;
    if (busy_ || checkingProjects_ || canvas_->editingText() || QApplication::activeModalWidget() ||
        documents_.isEmpty())
        return;
    watchPaths_.clear();
    for (const auto &tab : documents_)
        if (!tab.path.isEmpty() && !watchPaths_.contains(tab.path))
            watchPaths_.append(tab.path);
    if (watchPaths_.isEmpty())
        return;
    checkingProjects_ = true;
    QVector<QString> paths;
    QHash<QString, QByteArray> stamps;
    for (const auto &path : watchPaths_) {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        QDirIterator files(path, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        int count = 0;
        while (files.hasNext() && count++ < MaxLayers * 3) {
            files.next();
            auto file = files.fileInfo();
            if (file.isSymLink())
                continue;
            hash.addData(file.filePath().toUtf8());
            hash.addData(QByteArray::number(file.size()));
            hash.addData(QByteArray::number(file.lastModified().toMSecsSinceEpoch()));
        }
        stamps[path] = hash.result();
        if (watchStamps_.value(path) != stamps[path])
            paths.append(path);
    }
    if (paths.isEmpty()) {
        checkingProjects_ = false;
        return;
    }
    disconnect(&projectWatcher_, nullptr, this, nullptr);
    connect(&projectWatcher_, &QFutureWatcher<QVector<LoadedProject>>::finished, this, [this, paths, stamps] {
        checkingProjects_ = false;
        QVector<LoadedProject> loaded;
        try {
            loaded = projectWatcher_.result();
        } catch (...) {
            return;
        }
        if (busy_ || !Preferences::load().autoReload)
            return;
        for (int p = 0; p < paths.size() && p < loaded.size(); ++p) {
            if (loaded[p].state.id.isEmpty())
                continue;
            for (int i = 0; i < documents_.size(); ++i) {
                auto &tab = documents_[i];
                if (tab.path != paths[p])
                    continue;
                const auto digest = i == currentTab_ ? diskDigest_ : tab.digest;
                if (digest == loaded[p].digest) {
                    watchStamps_[paths[p]] = stamps[paths[p]];
                    continue;
                }
                const bool dirty =
                    i == currentTab_ ? document_.dirty() : tab.session.revision != tab.session.cleanRevision;
                if (dirty) {
                    if (!changedProjects_.contains(paths[p])) {
                        changedProjects_.insert(paths[p]);
                        statusBar()->showMessage(t("externalChange"), 15000);
                    }
                    continue;
                }
                if (i == currentTab_) {
                    canvas_->cancelGesture();
                    diskDigest_ = loaded[p].digest;
                    document_.reset(loaded[p].state, true);
                    stashTab();
                } else {
                    Document document;
                    document.reset(loaded[p].state, true);
                    tab.session = document.session();
                    tab.digest = loaded[p].digest;
                }
                watchStamps_[paths[p]] = stamps[paths[p]];
                changedProjects_.remove(paths[p]);
                statusBar()->showMessage(t("externalReloaded"), 4000);
            }
        }
    });
    projectWatcher_.setFuture(QtConcurrent::run([paths] {
        QVector<LoadedProject> loaded;
        for (const auto &path : paths) {
            try {
                loaded.append(readProject(path));
            } catch (...) {
                loaded.append(LoadedProject{});
            }
        }
        return loaded;
    }));
}
} // namespace comp
