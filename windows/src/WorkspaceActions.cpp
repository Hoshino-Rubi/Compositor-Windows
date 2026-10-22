#include "Pixels.h"
#include "MainWindow.h"
#include "Cmyk.h"
#include <QFileDialog>
#include <QColorSpace>
#include <QFile>
#include <QSettings>
#include "Renderer.h"
#include "Icons.h"
#include "Filters.h"
#include "LayerClipboard.h"
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QClipboard>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QTextEdit>
#include <QTimer>
#include <QMenu>
#include <QMenuBar>
#include <QVBoxLayout>
#include <QStatusBar>
#include <QSignalBlocker>
#include <QPainter>
#include <QScrollArea>
namespace comp {
void MainWindow::buildEditingActions() {
    auto add = [this](const QString &key, const QString &shortcut, std::function<void()> callback) {
        if (actions_.contains(key))
            return;
        auto *a = new QAction(this);
        a->setObjectName(key + "Action");
        if (!shortcut.isEmpty())
            a->setShortcut(QKeySequence(shortcut, QKeySequence::PortableText));
        connect(a, &QAction::triggered, this, [this, callback] { invoke(callback); });
        actions_[key] = a;
    };
    add("closeDocument", "Ctrl+W", [this] {
        if (currentTab_ >= 0)
            closeTab(currentTab_);
    });
    add("cut", "Ctrl+X", [this] {
        if (document_.state().selectionActive) {
            auto image = document_.copyPixels();
            if (!image.isNull()) {
                QApplication::clipboard()->setImage(image);
                document_.fill(Qt::transparent, true);
            }
        } else {
            const auto ids = selectedLayers();
            const auto encoded = encodeLayers(document_.state(), ids);
            if (!encoded.isEmpty()) {
                auto *mime = new QMimeData;
                mime->setData(LayerMimeType, encoded);
                QApplication::clipboard()->setMimeData(mime);
                cutLayers(document_, ids);
            }
        }
    });
    add("copy", "Ctrl+C", [this] {
        if (document_.state().selectionActive) {
            auto image = document_.copyPixels();
            if (!image.isNull()) QApplication::clipboard()->setImage(image);
        } else {
            const auto encoded = encodeLayers(document_.state(), selectedLayers());
            if (!encoded.isEmpty()) {
                auto *mime = new QMimeData;
                mime->setData(LayerMimeType, encoded);
                QApplication::clipboard()->setMimeData(mime);
            }
        }
    });
    add("copyMerged", "Ctrl+Shift+C", [this] {
        auto image = document_.copyPixels(true);
        if (!image.isNull())
            QApplication::clipboard()->setImage(image);
    });
    add("paste", "Ctrl+V", [this] {
        const auto *mime = QApplication::clipboard()->mimeData();
        if (!mime) return;
        if (mime->hasFormat(LayerMimeType)) {
            if (document_.hasDocument()) pasteLayers(document_, mime->data(LayerMimeType));
            else adoptDocument(layersAsDocument(mime->data(LayerMimeType)), t("pastedLayer"));
        } else if (mime->hasUrls()) {
            QStringList paths;
            for (const auto &url : mime->urls())
                if (url.isLocalFile()) paths.append(url.toLocalFile());
            if (!paths.isEmpty()) importNext(paths, !document_.hasDocument());
        } else {
            QImage image;
            // Decode original PNG/TIFF bytes, not the clipboard's low-depth display thumbnail.
            for (const auto &type : {QString("image/png"), QString("image/tiff")}) {
                if (mime->hasFormat(type)) image = QImage::fromData(mime->data(type));
                if (!image.isNull()) break;
            }
            if (image.isNull()) image = QApplication::clipboard()->image();
            if (!image.isNull()) {
                if (image.colorSpace().isValid() && image.colorSpace() != QColorSpace::SRgb)
                    image.convertToColorSpace(QColorSpace::SRgb);
                if (document_.hasDocument()) document_.pastePixels(image, t("pastedLayer"));
                else insertImported(t("pastedLayer"), image, true);
            }
        }
    });
    add("fillForeground", "Alt+Backspace", [this] { document_.fill(canvas_->color(), false, mask_->isChecked()); });
    add("fillBackground", "Ctrl+Backspace",
        [this] { document_.fill(backgroundColorValue_, false, mask_->isChecked()); });
    add("clearPixels", "Backspace", [this] { document_.fill(Qt::transparent, true, mask_->isChecked()); });
    add("layerEffects", "", [this] { effectsDialog(); });
    add("selectSubject", "", [this] { selectSubject(); });
    add("removeBackground", "", [this] { selectSubject(QPoint(-1, -1), true); });
    add("featherSelection", "Shift+F6", [this] {
        bool ok = false;
        double radius = QInputDialog::getDouble(this, t("featherSelection"), t("radius"), 2, .1, 250, 1, &ok);
        if (ok)
            document_.featherSelection(radius);
    });
    add("transformLayers", "Ctrl+T", [this] { transformDialog(); });
    add("freeDistort", "", [this] { transformDialog(true); });
    add("contentFill", "Shift+F5", [this] { document_.contentFill(); });
    add("selectAll", "Ctrl+A", [this] { document_.selectAll(); });
    add("deselect", "Ctrl+D", [this] { document_.deselect(); });
    add("invertSelection", "Ctrl+Shift+I", [this] { document_.invertSelection(); });
    add("selectPixels", "", [this] { document_.selectPixels(false); });
    add("selectMask", "", [this] { document_.selectPixels(true); });
    add("expandSelection", "", [this] {
        bool ok = false;
        int n = QInputDialog::getInt(this, t("expandSelection"), t("pixels"), 1, 1, 500, 1, &ok);
        if (ok)
            document_.modifySelection(n);
    });
    add("contractSelection", "", [this] {
        bool ok = false;
        int n = QInputDialog::getInt(this, t("contractSelection"), t("pixels"), 1, 1, 500, 1, &ok);
        if (ok)
            document_.modifySelection(-n);
    });
    add("groupSelection", "Ctrl+G", [this] {
        document_.groupLayers(selectedLayers(),
                              t("groupName").replace("{n}", QString::number(document_.state().layers.size() + 1)));
    });
    add("ungroup", "Ctrl+Shift+G", [this] { document_.ungroup(); });
    add("merge", "Ctrl+E", [this] { document_.merge(selectedLayers()); });
    add("editText", "", [this] { editText(); });
    add("addMask", "", [this] { document_.maskAction("addMask"); });
    add("hideMask", "", [this] { document_.maskAction("hideMask"); });
    add("invertMask", "", [this] { document_.maskAction("invertMask"); });
    add("enableMask", "", [this] { document_.maskAction("enableMask"); });
    add("linkMask", "", [this] { document_.maskAction("linkMask"); });
    add("applyMask", "", [this] { document_.maskAction("applyMask"); });
    add("deleteMask", "", [this] { document_.maskAction("deleteMask"); });
    add("clipMask", "", [this] { document_.maskAction("clipMask"); });
    add("cmykProfiles", "", [this] { cmykProfilesDialog(); });
    add("loadCmykProfile", "", [this] {
        const auto path = QFileDialog::getOpenFileName(this,t("loadCmykProfile"),{},"ICC (*.icc *.icm)");
        if(path.isEmpty())return;QFile file(path);if(!file.open(QIODevice::ReadOnly))throw Error("fileError");
        if(file.size()>4*1024*1024)throw Error("tooLarge");chooseCmykProfile(file.readAll());
        canvas_->setCmykProof(actions_["cmykProof"]->isChecked(),actions_["cmykGamut"]->isChecked());
    });
    for (const auto &key : {QString("cmykProof"),QString("cmykGamut")}) {
        add(key,"",[this,key] {
            try {canvas_->setCmykProof(actions_["cmykProof"]->isChecked(),actions_["cmykGamut"]->isChecked());}
            catch(...) {actions_[key]->setChecked(false);throw;}
        });actions_[key]->setCheckable(true);
    }
    for (int depth : {8,16}) add(depth==16?"layerDepth16":"layerDepth8","",[this,depth] {
        const int index=document_.state().index(document_.state().active);
        const bool hdr=index>=0 && hdrImage(document_.state().layers[index].image);
        if((depth==8 || hdr) && QMessageBox::question(this,t("bitDepth"),t(hdr?"hdrConvertWarning":"reduceDepth"),QMessageBox::Ok|QMessageBox::Cancel,QMessageBox::Cancel)!=QMessageBox::Ok)return;
        document_.setLayerBitDepth(depth);
    });
    auto *depthGroup = new QActionGroup(this);
    for (int depth : {8, 16}) {
        const QString key = depth == 16 ? "depth16" : "depth8";
        add(key, "", [this, depth] {
            const bool hdr=hasHdr(document_.state());
            if ((hdr || (depth == 8 && document_.state().bitDepth == 16)) &&
                QMessageBox::question(this, t("bitDepth"), t(hdr ? "hdrConvertWarning" : "reduceDepth"), QMessageBox::Ok | QMessageBox::Cancel,
                                      QMessageBox::Cancel) != QMessageBox::Ok) {
                refreshSelection();
                return;
            }
            document_.setBitDepth(depth);
        });
        actions_[key]->setCheckable(true);
        depthGroup->addAction(actions_[key]);
    }
    add("imageSize", "", [this] { sizeDialog(true); });
    add("canvasSize", "", [this] { sizeDialog(false); });
    add("splitChannels", "", [this] { document_.splitChannels(); });
    add("trim", "", [this] { document_.trim(); });
    add("rotateCanvasCW", "", [this] {
        document_.rotateCanvas(true);
        canvas_->fit();
    });
    add("rotateCanvasCCW", "", [this] {
        document_.rotateCanvas(false);
        canvas_->fit();
    });
    add("searchCommands", "Ctrl+F", [this] { commandPalette(); });
    add("lastFilter", "Ctrl+Alt+F", [this] {
        if (!lastFilter_.isEmpty())
            document_.applyFilter(lastFilter_, mask_->isChecked());
    });
    add("toggleFullscreen", "F", [this] { toggleCanvasFullscreen(); });
    auto *leaveFullscreen = new QAction(this);
    leaveFullscreen->setShortcut(Qt::Key_Escape);
    connect(leaveFullscreen, &QAction::triggered, this, [this] {
        if (fullscreenCanvas_)
            toggleCanvasFullscreen();
        canvas_->cancelGesture();
    });
    addAction(leaveFullscreen);
    add("flipCanvasX", "", [this] { document_.flipCanvas(true); });
    add("flipCanvasY", "", [this] { document_.flipCanvas(false); });
    add("showRulers", "Ctrl+R", [this] { canvas_->setRulers(actions_["showRulers"]->isChecked()); });
    actions_["showRulers"]->setCheckable(true);
    add("showGrid", "", [this] { canvas_->setGrid(actions_["showGrid"]->isChecked()); });
    actions_["showGrid"]->setCheckable(true);
    add("snap", "", [this] { canvas_->setSnap(actions_["snap"]->isChecked()); });
    actions_["snap"]->setCheckable(true);
    actions_["snap"]->setChecked(true);
    for (const auto &entry : QList<QPair<QString, bool>>{{"snapGuides", true}, {"snapGrid", false},
                                                         {"snapLayers", true}, {"snapBounds", true},
                                                         {"lockGuides", false}, {"pixelGrid", false},
                                                         {"transformControls", true}}) {
        add(entry.first, "", [this, key = entry.first] {
            QSettings().setValue("preferences/" + key, actions_[key]->isChecked());
            applyPreferences();
            preferences_->refresh();
        });
        actions_[entry.first]->setCheckable(true);
        actions_[entry.first]->setChecked(entry.second);
    }
    add("gridSettings", "", [this] {
        bool ok = false;
        int n = QInputDialog::getInt(this, t("gridSettings"), t("gridSpacing"), Preferences::load().gridSpacing, 4,
                                     2000, 1, &ok);
        if (ok) {
            canvas_->setGridSpacing(n);
            QSettings().setValue("preferences/gridSpacing", n);
            preferences_->refresh();
        }
    });
    add("addGuide", "", [this] {
        QDialog dialog(this);
        dialog.setWindowTitle(t("addGuide"));
        auto *form = new QFormLayout(&dialog);
        QComboBox axis;
        axis.addItems({t("horizontal"), t("vertical")});
        QDoubleSpinBox position;
        position.setRange(-100000, 100000);
        position.setValue(document_.state().size.height() / 2.);
        form->addRow(t("axis"), &axis);
        form->addRow(t("position"), &position);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() == QDialog::Accepted)
            document_.addGuide(axis.currentIndex() == 0, position.value());
    });
    add("clearGuides", "", [this] { document_.edit([](State &s) { s.guides = {}; }); });
    for (const auto &entry : QList<QPair<QString, QString>>{{"hsv", "Hue/Saturation"},
                                                            {"levels", "Levels"},
                                                            {"curves", "Curves"},
                                                            {"exposure", "Exposure"},
                                                            {"grayscale", "Black & White"},
                                                            {"invert", "Invert"},
                                                            {"blur", "Gaussian Blur"},
                                                            {"noise", "Add Noise"},
                                                            {"motionBlur", "Motion Blur"},
                                                            {"grain", "Grain"},
                                                            {"colorBalance", "Color Balance"},
                                                            {"gradientMap", "Gradient Map"},
                                                            {"cameraRaw", "Camera Raw"},
                                                            {"scanlines", "Scanlines"},
                                                             {"vignette", "Vignette"},
                                                             {"bloom", "Bloom / Glow"},
                                                             {"dither", "Dither"},
                                                             {"tonalContrast", "Tonal Contrast"},
                                                             {"lensCorrection", "Lens Correction"}, {"progressiveBlur", "Progressive Blur"}, {"rgbSplit", "RGB Split"}}) {
        const QString key = "filter." + entry.first;
        add(key, "", [this, kind = entry.second] { filterDialog(kind, false); });
    }
    for (const auto &entry : QList<QPair<QString, QString>>{{"hsv", "Hue/Saturation"},
                                                            {"levels", "Levels"},
                                                            {"curves", "Curves"},
                                                            {"exposure", "Exposure"},
                                                            {"invert", "Invert"},
                                                            {"grayscale", "Black & White"},
                                                            {"colorBalance", "Color Balance"},
                                                            {"gradientMap", "Gradient Map"},
                                                            {"grain", "Grain"},
                                                            {"noise", "Add Noise"},
                                                            {"motionBlur", "Motion Blur"},
                                                            {"blur", "Gaussian Blur"}, {"progressiveBlur", "Progressive Blur"}, {"bloom", "Bloom / Glow"}, {"rgbSplit", "RGB Split"}}) {
        const QString key = "adjustment." + entry.first;
        add(key, "", [this, kind = entry.second] { filterDialog(kind, true); });
    }
}
void MainWindow::sizeDialog(bool image) {
    if (!document_.hasDocument())
        return;
    QDialog dialog(this);
    dialog.setWindowTitle(image ? t("imageSize") : t("canvasSize"));
    auto *form = new QFormLayout(&dialog);
    QSpinBox w, h;
    w.setRange(1, MaxSide);
    h.setRange(1, MaxSide);
    w.setValue(document_.state().size.width());
    h.setValue(document_.state().size.height());
    QCheckBox keep(t("keepAspect")), center(t("centered"));
    keep.setChecked(true);
    center.setChecked(true);
    const double ratio = double(w.value()) / h.value();
    connect(&w, qOverload<int>(&QSpinBox::valueChanged), &dialog, [&](int v) {
        if (image && keep.isChecked()) {
            QSignalBlocker block(&h);
            h.setValue(qRound(v / ratio));
        }
    });
    connect(&h, qOverload<int>(&QSpinBox::valueChanged), &dialog, [&](int v) {
        if (image && keep.isChecked()) {
            QSignalBlocker block(&w);
            w.setValue(qRound(v * ratio));
        }
    });
    form->addRow(t("width"), &w);
    form->addRow(t("height"), &h);
    form->addRow(image ? &keep : &center);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted)
        invoke([&] {
            document_.resizeCanvas({w.value(), h.value()}, image, center.isChecked());
        });
}
void MainWindow::editText() {
    textDialog({}, document_.state().active);
}
void MainWindow::maskMenu() {
    QMenu menu(this);
    for (const auto &key : QStringList{"addMask", "hideMask", "invertMask", "enableMask", "linkMask", "applyMask",
                                       "deleteMask", "clipMask"})
        menu.addAction(actions_[key]);
    menu.exec(QCursor::pos());
}
void MainWindow::stashTab() {
    if (currentTab_ >= 0 && currentTab_ < documents_.size()) {
        const auto key = documents_[currentTab_].key;
        documents_[currentTab_] = {document_.session(), path_, name_, diskDigest_, canvas_->zoom(), key, compatibilityMode_};
    }
}
void MainWindow::adoptDocument(State state, QString name, QString path, QByteArray digest, bool saved) {
    canvas_->finishInlineText();
    canvas_->cancelGesture();
    stashTab();
    switchingTab_ = true;
    name_ = std::move(name);
    path_ = std::move(path);
    diskDigest_ = std::move(digest);
    currentTab_ = int(documents_.size());
    compatibilityMode_ = state.metadata.value("windowsCompatibility").toBool();
    document_.reset(std::move(state), saved);
    documents_.append({document_.session(), path_, name_, diskDigest_, 1, newId(), compatibilityMode_});
    tabs_->addTab(name_);
    auto *close = new QToolButton;
    close->setObjectName("closeTabButton");
    close->setIcon(icon("close"));
    close->setIconSize(QSize(14, 14));
    close->setFixedSize(22, 22);
    close->setToolTip(t("closeDocument"));
    connect(close, &QToolButton::clicked, this, [this, close] {
        for (int i = 0; i < tabs_->count(); ++i)
            if (tabs_->tabButton(i, QTabBar::RightSide) == close) {
                closeTab(i);
                break;
            }
    });
    tabs_->setTabButton(currentTab_, QTabBar::RightSide, close);
    tabs_->setCurrentIndex(currentTab_);
    switchingTab_ = false;
    canvas_->fit(false);
    updateTitle();
}
void MainWindow::switchTab(int index) {
    if (switchingTab_ || index < 0 || index >= documents_.size() || index == currentTab_)
        return;
    canvas_->finishInlineText();
    canvas_->cancelGesture();
    stashTab();
    switchingTab_ = true;
    currentTab_ = index;
    const auto tab = documents_[index];
    path_ = tab.path;
    name_ = tab.name;
    diskDigest_ = tab.digest;
    compatibilityMode_ = tab.compatibility;
    document_.restore(tab.session);
    switchingTab_ = false;
    canvas_->setZoom(tab.zoom);
    refresh();
}
bool MainWindow::closeTab(int index) {
    if (busy_ || index < 0 || index >= documents_.size())
        return false;
    tabs_->setCurrentIndex(index);
    if (!maybeSave())
        return false;
    switchingTab_ = true;
    documents_.removeAt(index);
    tabs_->removeTab(index);
    currentTab_ = -1;
    switchingTab_ = false;
    if (documents_.isEmpty()) {
        document_.restore({});
        name_ = t("untitled");
        path_.clear();
        diskDigest_.clear();
        compatibilityMode_ = false;
        refresh();
    } else {
        const int next = std::min(index, int(documents_.size()) - 1);
        tabs_->setCurrentIndex(next);
        switchTab(next);
    }
    return true;
}
bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    const bool bar = watched == tabs_ || (watched->objectName() == "documentBar");
    if (bar && !busy_ && (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove)) {
        auto *drag = static_cast<QDropEvent *>(event);
        const auto *mime = drag->mimeData();
        if (mime->hasFormat(LayerMimeType) || mime->hasUrls() || mime->hasImage()) {
            drag->acceptProposedAction();
            return true;
        }
    }
    if (bar && !busy_ && event->type() == QEvent::Drop) {
        auto *drop = static_cast<QDropEvent *>(event);
        const int index = watched == tabs_ ? tabs_->tabAt(drop->position().toPoint()) : -1;
        const auto *mime = drop->mimeData();
        if (mime->hasFormat(LayerMimeType)) {
            const auto encoded = mime->data(LayerMimeType);
            invoke([&] {
                if (index < 0) adoptDocument(layersAsDocument(encoded), t("untitled"));
                else {
                    tabs_->setCurrentIndex(index);
                    if (layerClipboardDocument(encoded) != document_.state().id)
                        pasteLayers(document_, encoded);
                }
            });
        } else if (mime->hasUrls()) {
            QStringList paths;
            for (const auto &url : mime->urls()) if (url.isLocalFile()) paths.append(url.toLocalFile());
            if (index >= 0) tabs_->setCurrentIndex(index);
            if (!paths.isEmpty()) importNext(paths, index < 0);
        } else if (mime->hasImage()) {
            if (index >= 0) tabs_->setCurrentIndex(index);
            invoke([&] { insertImported(t("pastedLayer"), qvariant_cast<QImage>(mime->imageData()), index < 0); });
        } else return QMainWindow::eventFilter(watched, event);
        drop->acceptProposedAction();
        return true;
    }
    return QMainWindow::eventFilter(watched, event);
}
void MainWindow::toggleCanvasFullscreen() {
    fullscreenCanvas_ = !fullscreenCanvas_;
    const auto widgets = QList<QWidget *>{menuBar(), statusBar(), findChild<QWidget *>("documentBar"),
                                          findChild<QWidget *>("toolHeader"), findChild<QWidget *>("toolRail"),
                                          findChild<QWidget *>("layerPanel")};
    if (fullscreenCanvas_) {
        windowStateBeforeFullscreen_ = windowState();
        showFullScreen();
    }
    for (auto *widget : widgets)
        if (widget) widget->setVisible(!fullscreenCanvas_);
    canvas_->setFullscreenCanvas(fullscreenCanvas_);
    if (!fullscreenCanvas_) setWindowState(windowStateBeforeFullscreen_);
    canvas_->setFocus();
}
} // namespace comp
