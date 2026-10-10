#include "MainWindow.h"
#include "Renderer.h"
#include "Icons.h"
#include "Filters.h"
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
            a->setShortcut(QKeySequence(shortcut));
        connect(a, &QAction::triggered, this, [this, callback] { invoke(callback); });
        actions_[key] = a;
    };
    add("closeDocument", "Ctrl+W", [this] {
        if (currentTab_ >= 0)
            closeTab(currentTab_);
    });
    add("cut", "Ctrl+X", [this] {
        auto image = document_.copyPixels();
        if (!image.isNull())
            QApplication::clipboard()->setImage(image);
        document_.fill(Qt::transparent, true);
    });
    add("copy", "Ctrl+C", [this] {
        auto image = document_.copyPixels();
        if (!image.isNull())
            QApplication::clipboard()->setImage(image);
    });
    add("copyMerged", "Ctrl+Shift+C", [this] {
        auto image = document_.copyPixels(true);
        if (!image.isNull())
            QApplication::clipboard()->setImage(image);
    });
    add("paste", "Ctrl+V", [this] {
        auto image = QApplication::clipboard()->image();
        if (!image.isNull())
            document_.pastePixels(image, t("pastedLayer"));
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
    add("imageSize", "", [this] { sizeDialog(true); });
    add("canvasSize", "", [this] { sizeDialog(false); });
    add("trim", "", [this] { document_.trim(); });
    add("flipCanvasX", "", [this] { document_.flipCanvas(true); });
    add("flipCanvasY", "", [this] { document_.flipCanvas(false); });
    add("showRulers", "Ctrl+R", [this] { canvas_->setRulers(actions_["showRulers"]->isChecked()); });
    actions_["showRulers"]->setCheckable(true);
    add("showGrid", "", [this] { canvas_->setGrid(actions_["showGrid"]->isChecked()); });
    actions_["showGrid"]->setCheckable(true);
    add("snap", "", [this] { canvas_->setSnap(actions_["snap"]->isChecked()); });
    actions_["snap"]->setCheckable(true);
    actions_["snap"]->setChecked(true);
    add("gridSettings", "", [this] {
        bool ok = false;
        int n = QInputDialog::getInt(this, t("gridSettings"), t("gridSpacing"), 100, 4, 2000, 1, &ok);
        if (ok)
            canvas_->setGridSpacing(n);
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
                                                            {"cameraRaw", "Camera Raw"}}) {
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
                                                            {"blur", "Gaussian Blur"}}) {
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
            canvas_->fit();
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
    if (currentTab_ >= 0 && currentTab_ < documents_.size())
        documents_[currentTab_] = {document_.session(), path_, name_, diskDigest_, canvas_->zoom()};
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
    document_.reset(std::move(state), saved);
    documents_.append({document_.session(), path_, name_, diskDigest_, 1});
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
    canvas_->fit();
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
        refresh();
    } else {
        const int next = std::min(index, int(documents_.size()) - 1);
        tabs_->setCurrentIndex(next);
        switchTab(next);
    }
    return true;
}
} // namespace comp
