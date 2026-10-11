#include "Pixels.h"
#include "MainWindow.h"
#include "Selection.h"
#include "Renderer.h"
#include "Warp.h"
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
        [this, mask, remove, i] {
            if (remove)
                document_.edit([&](State &s) {
                    auto &l = s.layers[i];
                    l.mask = *mask;
                    l.maskLinked = false;
                    l.maskEnabled = true;
                    l.maskPlacement = {};
                    l.maskPlacement.size = s.size;
                    l.originalMaskPng.clear();
                });
            else
                document_.setSelectionMask(*mask);
        });
}
void MainWindow::shortcutsDialog() {
    QDialog dialog(this);
    dialog.setObjectName("shortcutsDialog");
    dialog.setWindowTitle(t("shortcuts"));
    dialog.resize(650, 620);
    auto *layout = new QVBoxLayout(&dialog);
    auto *table = new QTableWidget;
    table->setColumnCount(2);
    table->setHorizontalHeaderLabels({t("command"), t("shortcut")});
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    QList<QAction *> list;
    for (auto *a : findChildren<QAction *>())
        if (!a->objectName().isEmpty())
            list.append(a);
    std::sort(list.begin(), list.end(), [](auto *a, auto *b) { return a->text() < b->text(); });
    QVector<QKeySequenceEdit *> editors;
    table->setRowCount(list.size());
    for (int i = 0; i < list.size(); ++i) {
        auto *item = new QTableWidgetItem(list[i]->text());
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        table->setItem(i, 0, item);
        auto *editor = new QKeySequenceEdit(list[i]->shortcut());
        editor->setObjectName(list[i]->objectName() + "Shortcut");
        editor->setMaximumSequenceLength(1);
        table->setCellWidget(i, 1, editor);
        editors.append(editor);
    }
    layout->addWidget(table);
    auto *buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, &dialog, [&] {
        for (int i = 0; i < list.size(); ++i)
            editors[i]->setKeySequence(QKeySequence(list[i]->property("originalShortcut").toString()));
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        QSet<QString> used;
        for (int i = 0; i < list.size(); ++i) {
            const auto key = editors[i]->keySequence().toString(QKeySequence::PortableText);
            if (key.isEmpty())
                continue;
            if (used.contains(key)) {
                QMessageBox::warning(&dialog, t("shortcuts"), t("shortcutConflict").replace("{detail}", key));
                return;
            }
            used.insert(key);
        }
        dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted)
        return;
    for (int i = 0; i < list.size(); ++i) {
        list[i]->setShortcut(editors[i]->keySequence());
        QSettings().setValue("shortcuts/" + list[i]->objectName(),
                             editors[i]->keySequence().toString(QKeySequence::PortableText));
    }
    retranslate();
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
                    l.image = perspectiveWarp(l.image, quad);
                    if (!l.mask.isNull()) {
                        const auto maskPlace = l.maskLinked ? l.placement : l.maskPlacement;
                        QPolygonF maskCorners{{0, 0},
                                              {double(l.mask.width()), 0},
                                              {double(l.mask.width()), double(l.mask.height())},
                                              {0, double(l.mask.height())}};
                        auto maskQuad = map.map(maskPlace.matrix(l.mask.size()).map(maskCorners));
                        auto rect = maskQuad.boundingRect().toAlignedRect();
                        l.mask = maskImage(perspectiveWarp(l.mask.convertToFormat(pixelFormat(s.bitDepth)), maskQuad),
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
        if (busy_)
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
