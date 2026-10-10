#include "MainWindow.h"
#include "Export.h"
#include "Renderer.h"
#include "Icons.h"
#include "Filters.h"
#include "Selection.h"
#include <QtConcurrent>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDialog>
#include <QEventLoop>
#include <QFontComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QImageWriter>
#include <QInputDialog>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QTextEdit>
#include <QToolBar>
#include <QVBoxLayout>
namespace comp {
MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    if (qEnvironmentVariableIsSet("COMPOSITOR_HEADLESS_TEST"))
        setAttribute(Qt::WA_DontShowOnScreen);
    setObjectName("mainWindow");
    resize(1320, 860);
    setMinimumSize(900, 600);
    setWindowIcon(QIcon(":/icons/app-icon-128.png"));
    canvas_ = new Canvas(&document_);
    buildActions();
    buildUi();
    connect(&localization_, &Localization::changed, this, &MainWindow::retranslate);
    connect(&document_, &Document::changed, this, &MainWindow::refresh);
    connect(&document_, &Document::selectionChanged, this, &MainWindow::refreshSelection);
    connect(&document_, &Document::visualChanged, this, &MainWindow::refreshSelection);
    connect(canvas_, &Canvas::objectRequested, this, [this](QPoint point) { selectSubject(point); });
    connect(canvas_, &Canvas::deleteRequested, this, [this] { actions_["delete"]->trigger(); });
    connect(canvas_, &Canvas::failed, this, &MainWindow::showError);
    connect(canvas_, &Canvas::textRequested, this, &MainWindow::addText);
    connect(canvas_, &Canvas::colorPicked, this, [this](QColor color) {
        color_->setStyleSheet("background:" + color.name() + ";border:2px solid #5c626b;border-radius:6px;");
    });
    connect(canvas_, &Canvas::filesDropped, this, [this](const QStringList &paths) {
        if (paths.size() == 1 && QFileInfo(paths.first()).isDir()) {
            openProject(paths.first());
        } else
            importImages(paths);
    });
    connect(canvas_, &Canvas::zoomChanged, this,
            [this](double value) { zoom_->setText(QString::number(qRound(value * 100)) + "%"); });
    connect(canvas_, &Canvas::brushSizeChanged, brushSize_, &QSpinBox::setValue);
    connect(&io_, &QFutureWatcher<void>::finished, this, [this] {
        setBusy(false);
        if (pendingError_) {
            try {
                std::rethrow_exception(pendingError_);
            } catch (const Error &e) {
                showError(e.key, e.detail);
            } catch (...) {
                showError("fileError");
            }
            pendingError_ = nullptr;
            completion_ = {};
            return;
        }
        auto done = std::move(completion_);
        completion_ = {};
        if (done)
            done();
    });
    for (auto *action : findChildren<QAction *>()) {
        const QString key = action->objectName();
        if (key.isEmpty())
            continue;
        action->setProperty("originalShortcut", action->shortcut().toString(QKeySequence::PortableText));
        if (QSettings().contains("shortcuts/" + key))
            action->setShortcut(QKeySequence(QSettings().value("shortcuts/" + key).toString()));
    }
    projectWatchTimer_.setInterval(2000);
    connect(&projectWatchTimer_, &QTimer::timeout, this, &MainWindow::watchProjects);
    projectWatchTimer_.start();
    name_ = t("untitled");
    retranslate();
    refresh();
    const auto geometry = QSettings().value("geometry").toByteArray();
    if (!geometry.isEmpty())
        restoreGeometry(geometry);
}
MainWindow::~MainWindow() {
    projectWatchTimer_.stop();
    projectWatcher_.waitForFinished();
    io_.waitForFinished();
}
void MainWindow::invoke(const std::function<void()> &action) {
    if (busy_)
        return;
    canvas_->finishInlineText();
    canvas_->cancelGesture();
    try {
        action();
    } catch (const Error &e) {
        showError(e.key, e.detail);
    }
}
void MainWindow::buildActions() {
    auto add = [&](QString key, QString shortcut, std::function<void()> callback) {
        auto *a = new QAction(this);
        a->setObjectName(key + "Action");
        if (!shortcut.isEmpty())
            a->setShortcut(QKeySequence(shortcut));
        connect(a, &QAction::triggered, this, [this, callback] { invoke(callback); });
        actions_[key] = a;
        return a;
    };
    add("new", "Ctrl+N", [this] { newDialog(); });
    add("open", "Ctrl+O", [this] {
        const auto path = QFileDialog::getExistingDirectory(this, t("open"));
        if (!path.isEmpty())
            openProject(path);
    });
    add("save", "Ctrl+S", [this] { save(); });
    add("saveAs", "Ctrl+Shift+S", [this] { save(true); });
    add("import", "Ctrl+I", [this] { importImages(); });
    add("export", "Ctrl+Shift+E", [this] { exportImage(); });
    add("undo", "Ctrl+Z", [this] { document_.undo(); });
    add("redo", "Ctrl+Shift+Z", [this] { document_.redo(); });
    auto *redo = new QShortcut(QKeySequence("Ctrl+Y"), this);
    connect(redo, &QShortcut::activated, this, [this] { invoke([this] { document_.redo(); }); });
    add("duplicate", "Ctrl+J", [this] {
        const int i = document_.state().index(document_.state().active);
        if (i >= 0)
            document_.duplicate(t("copyName").replace("{name}", document_.state().layers[i].name));
    });
    add("delete", "", [this] {
        if (mask_->isChecked())
            document_.maskAction("deleteMask");
        else if (canvas_->hasFocus() && document_.state().selectionActive)
            document_.fill(Qt::black, true);
        else
            document_.remove();
    });
    add("addLayer", "Ctrl+Shift+N", [this] {
        document_.addLayer(t("layerName").replace("{n}", QString::number(document_.state().layers.size() + 1)));
    });
    add("addGroup", "", [this] {
        document_.addLayer(t("groupName").replace("{n}", QString::number(document_.state().layers.size() + 1)), {},
                           true);
    });
    add("moveUp", "", [this] { document_.reorder(1); });
    add("moveDown", "", [this] { document_.reorder(-1); });
    add("rename", "F2", [this] {
        const int i = document_.state().index(document_.state().active);
        if (i < 0)
            return;
        bool ok = false;
        const auto name = QInputDialog::getText(this, t("rename"), t("name"), QLineEdit::Normal,
                                                document_.state().layers[i].name, &ok);
        if (ok && !name.trimmed().isEmpty())
            document_.edit([&](State &s) { s.layers[i].name = name; });
    });
    add("fit", "Ctrl+0", [this] { canvas_->fit(); });
    add("actualSize", "Ctrl+1", [this] { canvas_->setZoom(1); });
    add("zoomIn", "Ctrl++", [this] { canvas_->setZoom(canvas_->zoom() * 1.25); });
    add("zoomOut", "Ctrl+-", [this] { canvas_->setZoom(canvas_->zoom() / 1.25); });
    add("flipX", "", [this] {
        const int i = document_.state().index(document_.state().active);
        if (i >= 0)
            document_.edit([&](State &s) { s.layers[i].placement.flipX = !s.layers[i].placement.flipX; });
    });
    add("flipY", "", [this] {
        const int i = document_.state().index(document_.state().active);
        if (i >= 0)
            document_.edit([&](State &s) { s.layers[i].placement.flipY = !s.layers[i].placement.flipY; });
    });
    add("showGuides", "", [this] { canvas_->setGuides(actions_["showGuides"]->isChecked()); });
    actions_["showGuides"]->setCheckable(true);
    actions_["showGuides"]->setChecked(true);
    add("shortcuts", "", [this] { shortcutsDialog(); });
    add("about", "", [this] { QMessageBox::about(this, t("about"), t("aboutBody")); });
    add("quit", "Alt+F4", [this] { close(); });
    auto *group = new QActionGroup(this);
    group->setExclusive(true);
    const auto keys = ToolKeys;
    const QStringList shortcuts{"V", "B", "E", "U", "Shift+U", "T", "I", "H", "C",
                                "M", "L", "W", "S", "R",       "G", "Z", "J"};
    for (int i = 0; i < keys.size(); ++i) {
        auto *a = new QAction(this);
        a->setCheckable(true);
        a->setObjectName(keys[i] + "Tool");
        a->setShortcut(QKeySequence(shortcuts[i]));
        group->addAction(a);
        tools_[i] = a;
        connect(a, &QAction::triggered, this, [this, i] {
            if (!busy_)
                setTool(static_cast<Tool>(i));
        });
    }
    tools_[0]->setChecked(true);
    buildEditingActions();
}
void MainWindow::buildWelcome() {
    welcome_->setStyleSheet("QWidget#welcome{background:#1c1c1c;}");
    welcome_->setObjectName("welcome");
    auto *outer = new QVBoxLayout(welcome_);
    outer->addStretch();
    auto *card = new QWidget;
    card->setMaximumWidth(520);
    auto *form = new QVBoxLayout(card);
    form->setContentsMargins(28, 28, 28, 28);
    form->setSpacing(20);
    auto *heading = new QHBoxLayout;
    auto *title = new QLabel;
    title->setStyleSheet("font-size:22px;font-weight:600;");
    labels_.append({title, "newTitle"});
    heading->addWidget(title);
    heading->addStretch();
    auto *presets = new QComboBox;
    presets->addItem("1920 × 1080", QSize(1920, 1080));
    presets->addItem("2560 × 1440", QSize(2560, 1440));
    presets->addItem("3840 × 2160", QSize(3840, 2160));
    presets->addItem("1080 × 1080", QSize(1080, 1080));
    presets->addItem("1080 × 1920", QSize(1080, 1920));
    heading->addWidget(presets);
    form->addLayout(heading);
    auto *dimensions = new QHBoxLayout;
    auto dimension = [&](QString key, int value) {
        auto *column = new QVBoxLayout;
        auto *label = new QLabel;
        labels_.append({label, key});
        column->addWidget(label);
        auto *box = new QSpinBox;
        box->setRange(1, MaxSide);
        box->setValue(value);
        box->setSuffix(" px");
        box->setMinimumHeight(36);
        column->addWidget(box);
        dimensions->addLayout(column);
        return box;
    };
    auto *width = dimension("width", 1920), *height = dimension("height", 1080);
    form->addLayout(dimensions);
    connect(presets, qOverload<int>(&QComboBox::currentIndexChanged), this, [=](int index) {
        auto size = presets->itemData(index).toSize();
        width->setValue(size.width());
        height->setValue(size.height());
    });
    auto *note = new QLabel;
    labels_.append({note, "transparentCanvas"});
    note->setStyleSheet("color:#9b9b9b;");
    form->addWidget(note);
    auto *row = new QHBoxLayout;
    auto button = [&](QString key, std::function<void()> callback) {
        auto *b = new QPushButton;
        buttons_.append({b, key});
        row->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, callback] { invoke(callback); });
        return b;
    };
    button("open", [this] { actions_["open"]->trigger(); });
    button("import", [this] { importImages(); });
    row->addStretch();
    auto *create = button("create", [=, this] {
        State s;
        s.id = newId();
        s.size = {width->value(), height->value()};
        Layer l;
        l.id = newId();
        l.name = t("layerName").replace("{n}", "1");
        l.placement.size = s.size;
        s.layers.append(l);
        s.active = l.id;
        adoptDocument(s, t("untitled"));
    });
    create->setObjectName("primary");
    auto validate = [=] { create->setEnabled(qint64(width->value()) * height->value() <= MaxCanvasPixels); };
    connect(width, qOverload<int>(&QSpinBox::valueChanged), this, validate);
    connect(height, qOverload<int>(&QSpinBox::valueChanged), this, validate);
    form->addLayout(row);
    auto *sample = new QPushButton;
    buttons_.append({sample, "sample"});
    sample->setFlat(true);
    connect(sample, &QPushButton::clicked, this, &MainWindow::sample);
    form->addWidget(sample);
    outer->addWidget(card, 0, Qt::AlignHCenter);
    outer->addStretch();
}
void MainWindow::updateTitle() {
    setWindowTitle((document_.dirty() ? "● " : "") + (document_.hasDocument() ? name_ : QString("Compositor")) +
                   " — Compositor");
    if (tabs_ && currentTab_ >= 0 && currentTab_ < tabs_->count()) {
        tabs_->setTabText(currentTab_, name_ + (document_.dirty() ? " •" : ""));
        tabs_->setTabToolTip(currentTab_, path_.isEmpty() ? name_ : path_);
    }
}
void MainWindow::applyPlacement() {
    if (refreshing_ || busy_)
        return;
    auto *edited = qobject_cast<QDoubleSpinBox *>(sender());
    invoke([&] {
        const int i = document_.state().index(document_.state().active);
        if (i < 0)
            return;
        const Placement current = document_.state().layers[i].placement;
        Placement p = current;
        if (edited == x_)
            p.origin.setX(x_->value());
        else if (edited == y_)
            p.origin.setY(y_->value());
        else if (edited == width_)
            p.size.setWidth(width_->value());
        else if (edited == height_)
            p.size.setHeight(height_->value());
        else if (edited == rotation_)
            p.rotation = rotation_->value();
        if (p.json() != current.json())
            document_.edit([&](State &s) {
                const QPointF delta = p.origin - current.origin;
                s.layers[i].placement = p;
                if (s.layers[i].group)
                    for (int j : s.descendants(s.layers[i].id))
                        s.layers[j].placement.origin += delta;
            });
    });
}
void MainWindow::chooseColor() {
    const auto color = QColorDialog::getColor(canvas_->color(), this, t("color"));
    if (color.isValid()) {
        canvas_->setColor(color);
        color_->setStyleSheet("background:" + color.name() + ";border:2px solid #5c626b;border-radius:6px;");
    }
}
void MainWindow::newDialog() {
    QDialog dialog(this);
    dialog.setWindowTitle(t("new"));
    auto *outer = new QVBoxLayout(&dialog);
    auto *title = new QLabel(t("newTitle"));
    title->setStyleSheet("font-size:24px;font-weight:600;");
    outer->addWidget(title);
    outer->addWidget(new QLabel(t("newSubtitle")));
    auto *form = new QFormLayout;
    QDoubleSpinBox width, height;
    for (auto *box : {&width, &height}) {
        box->setRange(.01, MaxSide);
        box->setDecimals(2);
    }
    width.setObjectName("canvasWidth");
    height.setObjectName("canvasHeight");
    width.setValue(1600);
    height.setValue(1000);
    QComboBox units, dpi;
    units.setObjectName("canvasUnits");
    dpi.setObjectName("canvasDpi");
    units.addItems({t("units.pixels"), t("units.inches"), t("units.cm"), t("units.mm")});
    dpi.addItem("72 DPI", 72);
    dpi.addItem("300 DPI", 300);
    auto factor = [&] {
        double resolution = dpi.currentData().toDouble();
        switch (units.currentIndex()) {
        case 1:
            return resolution;
        case 2:
            return resolution / 2.54;
        case 3:
            return resolution / 25.4;
        default:
            return 1.;
        }
    };
    double oldFactor = 1;
    connect(&units, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [&] {
        const double next = factor();
        width.setValue(width.value() * oldFactor / next);
        height.setValue(height.value() * oldFactor / next);
        oldFactor = next;
    });
    connect(&dpi, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [&] { oldFactor = factor(); });
    auto pixels = [&] { return QSize(qRound(width.value() * factor()), qRound(height.value() * factor())); };
    QComboBox background;
    background.addItem(t("transparent"), 0);
    background.addItem(t("white"), 1);
    background.addItem(t("dark"), 2);
    form->addRow(t("width"), &width);
    form->addRow(t("height"), &height);
    form->addRow(t("units"), &units);
    form->addRow(t("resolution"), &dpi);
    form->addRow(t("background"), &background);
    outer->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(t("create"));
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (!pixels().isValid() || pixels().width() > MaxSide || pixels().height() > MaxSide ||
            qint64(pixels().width()) * pixels().height() > MaxCanvasPixels) {
            showError("invalidSize");
            return;
        }
        dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted)
        return;
    State s;
    s.id = newId();
    s.size = pixels();
    s.resolution = dpi.currentData().toDouble();
    if (background.currentData().toInt() != 0) {
        Layer l;
        l.id = newId();
        l.name = t("backgroundLayer");
        l.placement.size = s.size;
        l.image = QImage(s.size, QImage::Format_ARGB32_Premultiplied);
        if (l.image.isNull())
            throw Error("tooLarge");
        l.image.fill(background.currentData().toInt() == 1 ? Qt::white : Qt::black);
        s.layers.append(l);
        s.active = l.id;
    }
    if (s.layers.isEmpty()) {
        Layer l;
        l.id = newId();
        l.name = t("layerName").replace("{n}", "1");
        l.placement.size = s.size;
        s.layers.append(l);
        s.active = l.id;
    }
    adoptDocument(s, t("untitled"));
}
void MainWindow::addText(QPointF point) {
    const int i = document_.state().index(document_.state().active);
    if (i >= 0 && document_.state().layers[i].metadata["text"].isObject()) {
        const auto &l = document_.state().layers[i];
        if (l.placement.matrix(l.image.size()).mapRect(QRectF(QPointF(), l.image.size())).contains(point)) {
            textDialog(point, l.id);
            return;
        }
    }
    textDialog(point);
}
void MainWindow::setBusy(bool busy) {
    busy_ = busy;
    editor_->setEnabled(!busy);
    welcome_->setEnabled(!busy);
    for (auto *a : actions_)
        a->setEnabled(!busy);
    refreshSelection();
}
void MainWindow::runIO(std::function<void()> work, std::function<void()> done) {
    if (busy_)
        return;
    pendingError_ = nullptr;
    completion_ = std::move(done);
    setBusy(true);
    io_.setFuture(QtConcurrent::run([this, work] {
        try {
            work();
        } catch (...) {
            pendingError_ = std::current_exception();
        }
    }));
}
void MainWindow::remember(const QString &path) {
    auto recent = QSettings().value("recent").toStringList();
    recent.removeAll(path);
    recent.prepend(path);
    while (recent.size() > 8)
        recent.removeLast();
    QSettings().setValue("recent", recent);
}
void MainWindow::openProject(const QString &path) {
    if (busy_)
        return;
    canvas_->finishInlineText();
    if (QFileInfo(path).isFile()) {
        importImages({path});
        return;
    }
    auto loaded = std::make_shared<LoadedProject>();
    for (int i = 0; i < documents_.size(); ++i)
        if (documents_[i].path == path) {
            tabs_->setCurrentIndex(i);
            return;
        }
    runIO([loaded, path] { *loaded = readProject(path); },
          [this, loaded, path] {
              adoptDocument(loaded->state, QFileInfo(path).completeBaseName(), path, loaded->digest, true);
              remember(path);
          });
}
bool MainWindow::save(bool as, std::function<void(bool)> after) {
    if (busy_ || !document_.hasDocument())
        return false;
    QString target = as ? QString() : path_;
    if (target.isEmpty()) {
        target = QFileDialog::getSaveFileName(this, t("saveAs"), name_ + ".comp", "Compositor (*.comp)", nullptr,
                                              QFileDialog::DontConfirmOverwrite);
        if (target.isEmpty()) {
            if (after)
                after(false);
            return false;
        }
        if (!target.endsWith(".comp", Qt::CaseInsensitive))
            target += ".comp";
        if (QFileInfo::exists(target) &&
            QMessageBox::question(this, t("overwrite"), t("overwriteBody"), QMessageBox::Yes | QMessageBox::Cancel,
                                  QMessageBox::Cancel) != QMessageBox::Yes) {
            if (after)
                after(false);
            return false;
        }
    }
    const State snapshot = document_.state();
    savedRevision_ = document_.revision();
    const QByteArray expected = target == path_ ? diskDigest_ : QByteArray();
    auto digest = std::make_shared<QByteArray>();
    runIO([target, snapshot, expected, digest] { *digest = writeProject(target, snapshot, expected); },
          [this, target, digest, after] {
              path_ = target;
              diskDigest_ = *digest;
              name_ = QFileInfo(target).completeBaseName();
              document_.markSaved(savedRevision_);
              stashTab();
              remember(target);
              statusBar()->showMessage(t("projectSaved"), 3000);
              if (after)
                  after(true);
          });
    return true;
}
bool MainWindow::maybeSave() {
    if (busy_)
        return false;
    canvas_->finishInlineText();
    canvas_->cancelGesture();
    if (!document_.dirty())
        return true;
    QMessageBox box(QMessageBox::Warning, t("savedChangesTitle"), t("savedChangesBody"), QMessageBox::NoButton, this);
    auto *saveButton = box.addButton(t("save"), QMessageBox::AcceptRole);
    auto *discard = box.addButton(t("discard"), QMessageBox::DestructiveRole);
    auto *cancel = box.addButton(t("keepEditing"), QMessageBox::RejectRole);
    box.setDefaultButton(cancel);
    box.exec();
    if (box.clickedButton() == discard)
        return true;
    if (box.clickedButton() == saveButton) {
        QEventLoop loop;
        const auto connection = connect(&io_, &QFutureWatcher<void>::finished, &loop, &QEventLoop::quit);
        if (save())
            loop.exec();
        disconnect(connection);
        return !document_.dirty();
    }
    return false;
}
void MainWindow::importImages(QStringList paths) {
    if (busy_)
        return;
    if (paths.isEmpty())
        paths = QFileDialog::getOpenFileNames(this, t("import"), {}, t("imageFilter"));
    importNext(paths);
}
void MainWindow::exportImage() {
    if (!document_.hasDocument() || busy_)
        return;
    QDialog dialog(this);
    dialog.setObjectName("exportDialog");
    dialog.setWindowTitle(t("export"));
    dialog.resize(660, 600);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    QComboBox format;
    format.addItem("PNG", "png");
    format.addItem("JPEG", "jpeg");
    format.addItem("PDF", "pdf");
    QSpinBox width, height, quality;
    width.setRange(1, MaxSide);
    height.setRange(1, MaxSide);
    quality.setRange(1, 100);
    quality.setValue(92);
    width.setValue(document_.state().size.width());
    height.setValue(document_.state().size.height());
    QCheckBox aspect(t("keepAspect"));
    aspect.setChecked(true);
    form->addRow(t("format"), &format);
    form->addRow(t("width"), &width);
    form->addRow(t("height"), &height);
    form->addRow(&aspect);
    form->addRow(t("quality"), &quality);
    layout->addLayout(form);
    auto *preview = new QLabel;
    preview->setMinimumSize(460, 250);
    preview->setAlignment(Qt::AlignCenter);
    layout->addWidget(preview, 1);
    const auto image = comp::render(document_.state(), document_.state().size.scaled(540, 280, Qt::KeepAspectRatio));
    auto update = [&] {
        quality.setEnabled(format.currentData() == "jpeg");
        QImage tile(16, 16, QImage::Format_RGB32);
        tile.fill(QColor("#dddddd"));
        {
            QPainter p(&tile);
            p.fillRect(0, 0, 8, 8, QColor("#f0f0f0"));
            p.fillRect(8, 8, 8, 8, QColor("#f0f0f0"));
        }
        const auto size = QSize(width.value(), height.value()).scaled(540, 280, Qt::KeepAspectRatio);
        QImage picture(size, QImage::Format_RGB32);
        QPainter p(&picture);
        p.fillRect(picture.rect(), format.currentData() == "png" ? QBrush(tile) : QBrush(Qt::white));
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(picture.rect(), image);
        p.end();
        preview->setPixmap(QPixmap::fromImage(picture));
    };
    const double ratio = double(document_.state().size.width()) / document_.state().size.height();
    connect(&width, qOverload<int>(&QSpinBox::valueChanged), &dialog, [&](int v) {
        if (aspect.isChecked()) {
            QSignalBlocker block(&height);
            height.setValue(qRound(v / ratio));
        }
        update();
    });
    connect(&height, qOverload<int>(&QSpinBox::valueChanged), &dialog, [&](int v) {
        if (aspect.isChecked()) {
            QSignalBlocker block(&width);
            width.setValue(qRound(v * ratio));
        }
        update();
    });
    connect(&format, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, update);
    update();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(t("exportAction"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (qint64(width.value()) * height.value() > MaxCanvasPixels) {
            showError("invalidSize");
            return;
        }
        dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const auto encoding = format.currentData().toByteArray();
    const QString extension = encoding == "jpeg" ? "jpg" : QString::fromLatin1(encoding);
    const QString file = QFileDialog::getSaveFileName(this, t("export"), name_ + "." + extension,
                                                      format.currentText() + " (*." + extension + ")");
    if (file.isEmpty())
        return;
    const State snapshot = document_.state();
    runIO([snapshot, file, encoding, size = QSize(width.value(), height.value()),
           q = quality.value()] { exportDocument(snapshot, file, encoding, size, q); },
          [this] { statusBar()->showMessage(t("exported"), 3000); });
}
void MainWindow::sample() {
    State s;
    s.id = newId();
    s.size = {1600, 1000};
    Layer background;
    background.id = newId();
    background.name = t("sampleBackground");
    background.placement.size = s.size;
    background.image = QImage(s.size, QImage::Format_ARGB32_Premultiplied);
    {
        QPainter p(&background.image);
        QLinearGradient gradient(0, 0, 1600, 1000);
        gradient.setColorAt(0, QColor("#273a46"));
        gradient.setColorAt(.55, QColor("#77948c"));
        gradient.setColorAt(1, QColor("#e8b99a"));
        p.fillRect(background.image.rect(), gradient);
    }
    s.layers.append(background);
    Layer orb;
    orb.id = newId();
    orb.name = t("sampleOrb");
    orb.image = QImage(740, 740, QImage::Format_ARGB32_Premultiplied);
    orb.image.fill(Qt::transparent);
    orb.placement.origin = {830, 120};
    orb.placement.size = orb.image.size();
    {
        QPainter p(&orb.image);
        p.setRenderHint(QPainter::Antialiasing);
        QRadialGradient gradient(270, 180, 560);
        gradient.setColorAt(0, QColor("#ffdea9"));
        gradient.setColorAt(.6, QColor("#ec8057"));
        gradient.setColorAt(1, QColor("#a83d38"));
        p.setPen(Qt::NoPen);
        p.setBrush(gradient);
        p.drawEllipse(20, 20, 700, 700);
    }
    s.layers.append(orb);
    Layer text;
    text.id = newId();
    text.name = t("sampleName");
    text.placement.size = s.size;
    text.image = QImage(s.size, QImage::Format_ARGB32_Premultiplied);
    text.image.fill(Qt::transparent);
    {
        QPainter p(&text.image);
        p.setRenderHint(QPainter::TextAntialiasing);
        p.setPen(QColor("#fff4e5"));
        QFont font(localization_.language() == "zh-CN"   ? "Microsoft YaHei UI"
                   : localization_.language() == "zh-TW" ? "Microsoft JhengHei UI"
                                                         : "Segoe UI");
        font.setPixelSize(110);
        font.setWeight(QFont::DemiBold);
        p.setFont(font);
        auto lines = t("sampleTitle").split('\n');
        for (int i = 0; i < lines.size(); ++i)
            p.drawText(100, 440 + i * 130, lines[i]);
        font.setPixelSize(20);
        font.setWeight(QFont::Normal);
        p.setFont(font);
        p.drawText(100, 100, t("sampleLabel"));
        p.drawText(100, 880, t("sampleCaption"));
    }
    s.layers.append(text);
    s.active = orb.id;
    adoptDocument(s, t("sampleName"));
}
void MainWindow::showError(const QString &key, const QString &detail) {
    const QString translatedDetail = detail == "adjustments" || detail == "effects" ? t(detail) : detail;
    QMessageBox::warning(this, t("errorTitle"), t(key).replace("{detail}", translatedDetail));
}
void MainWindow::closeEvent(QCloseEvent *event) {
    if (busy_) {
        event->ignore();
        return;
    }
    stashTab();
    for (int i = 0; i < documents_.size(); ++i) {
        tabs_->setCurrentIndex(i);
        if (!maybeSave()) {
            event->ignore();
            return;
        }
    }
    QSettings().setValue("geometry", saveGeometry());
    event->accept();
}
} // namespace comp
