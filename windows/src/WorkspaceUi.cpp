#include "MainWindow.h"
#include "Icons.h"
#include <QApplication>
#include <QMenuBar>
#include <QMenu>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QStatusBar>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QSignalBlocker>
#include <QShortcut>
#include <QSettings>
#include <QPainter>
#include <QHeaderView>
#include <QDropEvent>
#include <QColorDialog>
#include <QStyledItemDelegate>
namespace comp {
class LayerDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        if (index.column() != 0) {
            QStyledItemDelegate::paint(p, option, index);
            return;
        }
        auto base = option;
        initStyleOption(&base, index);
        const auto image = base.icon;
        p->fillRect(option.rect, option.state.testFlag(QStyle::State_Selected) ? QColor("#414141") : QColor("#242424"));
        if (!image.isNull()) {
            QRect rect(0, 0, 18, 18);
            rect.moveCenter(option.rect.center());
            image.paint(p, rect);
        }
    }
};
class LayerTree final : public QTreeWidget {
  public:
    std::function<void(QString, QString, QString)> moved;
    using QTreeWidget::QTreeWidget;

  protected:
    void dropEvent(QDropEvent *event) override {
        auto *dragged = currentItem(), *target = itemAt(event->position().toPoint());
        if (!dragged)
            return;
        const QString id = dragged->data(0, Qt::UserRole).toString();
        QString parent, before;
        if (target) {
            const QRect box = visualItemRect(target);
            const bool folder = target->data(0, Qt::UserRole + 1).toBool();
            if (folder && event->position().y() > box.top() + box.height() / 4 &&
                event->position().y() < box.bottom() - box.height() / 4)
                parent = target->data(0, Qt::UserRole).toString();
            else {
                if (target->parent())
                    parent = target->parent()->data(0, Qt::UserRole).toString();
                if (event->position().y() > box.center().y())
                    before = target->data(0, Qt::UserRole).toString();
                else {
                    auto *sibling = target->parent()
                                        ? target->parent()->child(target->parent()->indexOfChild(target) - 1)
                                        : topLevelItem(indexOfTopLevelItem(target) - 1);
                    if (sibling)
                        before = sibling->data(0, Qt::UserRole).toString();
                }
            }
        }
        event->acceptProposedAction();
        if (moved)
            moved(id, parent, before);
    }
};
void MainWindow::buildUi() {
    setStyleSheet(R"(
QMainWindow,QWidget{background:#242424;color:#dcdcdc;font-size:12px;}
QMenuBar{background:#292929;border-bottom:1px solid #191919;}QMenuBar::item{padding:5px 10px;}QMenuBar::item:selected,QMenu::item:selected{background:#454545;}
QMenu{background:#2c2c2c;border:1px solid #484848;padding:5px;}QMenu::item{padding:7px 30px 7px 16px;}QMenu::separator{height:1px;background:#454545;margin:5px;}
QWidget#documentBar{background:#2b2b2b;border-bottom:1px solid #181818;}QWidget#toolHeader{background:#272727;border-bottom:1px solid #171717;}
QToolButton{background:transparent;border:1px solid transparent;border-radius:5px;padding:4px;}QToolButton:hover{background:#393939;}QToolButton:checked{background:#454545;border:1px solid #616161;}QToolButton:disabled{color:#606060;}
QTabBar::tab{background:#292929;color:#999999;border:1px solid transparent;border-radius:5px;padding:8px 18px;min-width:120px;margin:2px;}QTabBar::tab:selected{background:#404040;color:#eeeeee;border-color:#4a4a4a;}QTabBar::tab:hover{background:#383838;}
QTreeWidget{background:#242424;border:0;outline:0;}QTreeWidget::item{border:0;padding:6px 2px;}QTreeWidget::item:selected{background:#414141;}QTreeWidget::item:hover{background:#333333;}QTreeWidget::branch{background:transparent;}
QSpinBox,QDoubleSpinBox,QComboBox,QLineEdit,QTextEdit{background:#303030;border:1px solid #484848;border-radius:4px;padding:4px 6px;selection-background-color:#42628a;min-height:19px;}
QSpinBox:disabled,QDoubleSpinBox:disabled,QComboBox:disabled{color:#666666;border-color:#353535;}QComboBox::drop-down{width:24px;border:0;}QComboBox::down-arrow{image:url(:/controls/chevron-down.svg);width:14px;height:14px;}QComboBox QAbstractItemView{background:#303030;selection-background-color:#4b4b4b;}
QPushButton{background:#404040;border:1px solid #505050;border-radius:5px;padding:7px 13px;}QPushButton:hover{background:#4d4d4d;}QPushButton#primary{background:#dedede;color:#202020;font-weight:600;}
QCheckBox{spacing:6px;}QCheckBox::indicator{width:14px;height:14px;}QLabel#heading,QLabel#toolTitle{font-weight:600;color:#e5e5e5;}
QStatusBar{background:#242424;border-top:1px solid #151515;font-size:11px;}QStatusBar::item{border:0;}QScrollArea{border:0;}
QSplitter::handle{background:#181818;}QSplitter::handle:hover{background:#555555;}QScrollBar:vertical{background:#242424;width:7px;margin:0;}QScrollBar::handle:vertical{background:#505050;border-radius:3px;min-height:24px;}QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}
QSlider::groove:horizontal{height:3px;background:#111111;border-radius:1px;}QSlider::handle:horizontal{width:12px;margin:-5px 0;background:#b7b7b7;border-radius:6px;}
    )");
    auto menu = [&](QString key, QStringList keys) {
        auto *m = menuBar()->addMenu("");
        m->setObjectName(key + "Menu");
        for (const auto &a : keys)
            if (a.isEmpty())
                m->addSeparator();
            else
                m->addAction(actions_[a]);
        return m;
    };
    menu("file", {"new", "open", "import", "", "save", "saveAs", "export", "", "closeDocument", "quit"});
    menu("edit", {"undo", "redo", "", "cut", "copy", "copyMerged", "paste", "", "fillForeground", "fillBackground",
                  "clearPixels", "contentFill", "", "shortcuts"});
    menu("image", {"imageSize", "canvasSize", "trim", "", "flipCanvasX", "flipCanvasY"});
    menu("layer", {"addLayer",   "addGroup",   "groupSelection",  "ungroup",     "",           "duplicate", "merge",
                   "rename",     "editText",   "layerEffects",    "delete",      "",           "moveUp",    "moveDown",
                   "flipX",      "flipY",      "transformLayers", "freeDistort", "",           "addMask",   "hideMask",
                   "invertMask", "enableMask", "linkMask",        "applyMask",   "deleteMask", "clipMask"});
    menu("select", {"selectSubject", "removeBackground", "selectAll", "deselect", "invertSelection", "selectPixels",
                    "selectMask", "expandSelection", "contractSelection", "featherSelection"});
    auto *adjust =
        menu("adjustments", {"filter.hsv", "filter.levels", "filter.curves", "filter.exposure", "filter.grayscale",
                             "filter.invert", "filter.blur", "filter.motionBlur", "filter.noise", "filter.grain",
                             "filter.colorBalance", "filter.gradientMap", "filter.cameraRaw"});
    auto *adjustLayer = adjust->addMenu("");
    adjustLayer->setObjectName("adjustmentLayerMenu");
    for (const auto &key : QStringList{"hsv", "levels", "curves", "exposure", "grayscale", "colorBalance",
                                       "gradientMap", "grain", "noise", "blur", "motionBlur", "invert"})
        adjustLayer->addAction(actions_["adjustment." + key]);
    menu("view", {"fit", "actualSize", "zoomIn", "zoomOut", "", "showRulers", "showGuides", "showGrid", "snap",
                  "gridSettings", "addGuide", "clearGuides"});
    menu("help", {"shortcuts", "about"});
    auto *root = new QWidget;
    auto *outer = new QVBoxLayout(root);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    setCentralWidget(root);
    auto toolButton = [&](QAction *action, int size = 32) {
        auto *b = new QToolButton;
        b->setDefaultAction(action);
        b->setToolButtonStyle(Qt::ToolButtonIconOnly);
        b->setFixedSize(size, size);
        b->setIconSize(QSize(22, 22));
        b->setFocusPolicy(Qt::NoFocus);
        return b;
    };
    auto *bar = new QWidget;
    bar->setObjectName("documentBar");
    auto *barRow = new QHBoxLayout(bar);
    barRow->setContentsMargins(9, 4, 10, 4);
    barRow->setSpacing(6);
    barRow->addWidget(toolButton(actions_["new"]));
    tabs_ = new QTabBar;
    tabs_->setObjectName("documentTabs");
    tabs_->setExpanding(false);
    tabs_->setUsesScrollButtons(true);
    tabs_->setDrawBase(false);
    tabs_->setElideMode(Qt::ElideRight);
    tabs_->setTabsClosable(false);
    barRow->addWidget(tabs_, 1);
    connect(tabs_, &QTabBar::currentChanged, this, &MainWindow::switchTab);
    connect(tabs_, &QTabBar::tabCloseRequested, this, &MainWindow::closeTab);
    for (const auto &key : QStringList{"fit", "actualSize"}) {
        auto *b = new QToolButton;
        b->setDefaultAction(actions_[key]);
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
        barRow->addWidget(b);
    }
    barRow->addWidget(toolButton(actions_["zoomOut"]));
    barRow->addWidget(toolButton(actions_["zoomIn"]));
    language_ = new QComboBox;
    language_->setObjectName("languageSelector");
    language_->addItem("English", "en");
    language_->addItem("简体中文", "zh-CN");
    language_->addItem("繁體中文", "zh-TW");
    language_->setFixedWidth(112);
    barRow->addWidget(language_);
    connect(language_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int i) { localization_.setLanguage(language_->itemData(i).toString()); });
    outer->addWidget(bar);
    auto label = [&](const QString &key, QHBoxLayout *row) {
        auto *l = new QLabel;
        labels_.append({l, key});
        row->addWidget(l);
        return l;
    };
    auto *header = new QWidget;
    header->setObjectName("toolHeader");
    header->setFixedHeight(50);
    auto *headerRow = new QHBoxLayout(header);
    headerRow->setContentsMargins(16, 0, 12, 0);
    headerRow->setSpacing(16);
    toolTitle_ = new QLabel;
    toolTitle_->setObjectName("toolTitle");
    toolTitle_->setMinimumWidth(60);
    headerRow->addWidget(toolTitle_);
    auto *optionScroll = new QScrollArea;
    optionScroll->setWidgetResizable(true);
    optionScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    optionScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    toolOptions_ = new QStackedWidget;
    optionScroll->setWidget(toolOptions_);
    headerRow->addWidget(optionScroll, 1);
    auto page = [&]() {
        auto *w = new QWidget;
        auto *r = new QHBoxLayout(w);
        r->setContentsMargins(0, 4, 0, 4);
        r->setSpacing(9);
        toolOptions_->addWidget(w);
        return r;
    };
    auto *transform = page();
    autoSelect_ = new QCheckBox;
    autoSelect_->setObjectName("autoSelect");
    autoSelect_->setChecked(true);
    transform->addWidget(autoSelect_);
    connect(autoSelect_, &QCheckBox::toggled, canvas_, &Canvas::setAutoSelect);
    auto number = [&](QString key, QDoubleSpinBox *&box, double low, double high) {
        label(key, transform);
        box = new QDoubleSpinBox;
        box->setObjectName(key);
        box->setRange(low, high);
        box->setDecimals(1);
        box->setKeyboardTracking(false);
        box->setFixedWidth(key == "rotation" ? 68 : 78);
        box->setButtonSymbols(QAbstractSpinBox::NoButtons);
        transform->addWidget(box);
        connect(box, &QDoubleSpinBox::editingFinished, this, &MainWindow::applyPlacement);
    };
    number("x", x_, -1e6, 1e6);
    number("y", y_, -1e6, 1e6);
    number("width", width_, 1, 300000);
    number("height", height_, 1, 300000);
    number("rotation", rotation_, -3600, 3600);
    rotation_->setSuffix("°");
    for (const auto &key : QStringList{"flipX", "flipY"})
        transform->addWidget(toolButton(actions_[key], 30));
    transform->addStretch();
    auto *brush = page();
    brushMode_ = new QComboBox;
    brushMode_->addItems({"", ""});
    brush->addWidget(brushMode_);
    connect(brushMode_, qOverload<int>(&QComboBox::activated), this,
            [this](int i) { setTool(i == 0 ? Tool::Brush : Tool::Eraser); });
    retouchMode_ = new QComboBox;
    retouchMode_->setObjectName("retouchMode");
    retouchMode_->addItems({"", "", ""});
    brush->addWidget(retouchMode_);
    connect(retouchMode_, qOverload<int>(&QComboBox::currentIndexChanged), canvas_, &Canvas::setRetouchMode);
    label("size", brush);
    brushSize_ = new QSpinBox;
    brushSize_->setRange(1, 2000);
    brushSize_->setValue(28);
    brushSize_->setSuffix(" px");
    brushSize_->setFixedWidth(88);
    brush->addWidget(brushSize_);
    connect(brushSize_, qOverload<int>(&QSpinBox::valueChanged), canvas_, &Canvas::setBrushSize);
    label("hardness", brush);
    hardness_ = new QSpinBox;
    hardness_->setRange(0, 100);
    hardness_->setValue(100);
    hardness_->setSuffix("%");
    hardness_->setFixedWidth(78);
    brush->addWidget(hardness_);
    connect(hardness_, qOverload<int>(&QSpinBox::valueChanged), this,
            [this](int v) { canvas_->setHardness(v / 100.); });
    label("brushOpacity", brush);
    flow_ = new QSlider(Qt::Horizontal);
    flow_->setRange(1, 100);
    flow_->setValue(100);
    flow_->setFixedWidth(100);
    brush->addWidget(flow_);
    auto *flowValue = new QLabel("100%");
    flowValue->setMinimumWidth(40);
    brush->addWidget(flowValue);
    connect(flow_, &QSlider::valueChanged, this, [this, flowValue](int v) {
        canvas_->setFlow(v / 100.);
        flowValue->setText(QString::number(v) + "%");
    });
    connect(canvas_, &Canvas::flowChanged, flow_, &QSlider::setValue);
    brush->addStretch();
    auto *selection = page();
    lassoMode_ = new QComboBox;
    lassoMode_->setObjectName("lassoMode");
    lassoMode_->addItems({"", ""});
    selection->addWidget(lassoMode_);
    connect(lassoMode_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int i) { canvas_->setPolygonal(i == 1); });
    wandMode_ = new QComboBox;
    wandMode_->setObjectName("wandMode");
    wandMode_->addItems({"", ""});
    selection->addWidget(wandMode_);
    connect(wandMode_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int i) { canvas_->setObjectMode(i == 1); });
    marqueeShape_ = new QComboBox;
    marqueeShape_->addItems({"", ""});
    selection->addWidget(marqueeShape_);
    connect(marqueeShape_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int v) { canvas_->setEllipseSelection(v == 1); });
    selectionModeBox_ = new QComboBox;
    for (int i = 0; i < 4; ++i)
        selectionModeBox_->addItem("");
    selection->addWidget(selectionModeBox_);
    connect(selectionModeBox_, qOverload<int>(&QComboBox::currentIndexChanged), canvas_, &Canvas::setSelectionMode);
    label("tolerance", selection)->setObjectName("toleranceLabel");
    tolerance_ = new QSpinBox;
    tolerance_->setRange(0, 255);
    tolerance_->setValue(32);
    tolerance_->setFixedWidth(65);
    selection->addWidget(tolerance_);
    connect(tolerance_, qOverload<int>(&QSpinBox::valueChanged), canvas_, &Canvas::setWandTolerance);
    contiguous_ = new QCheckBox;
    contiguous_->setChecked(true);
    selection->addWidget(contiguous_);
    connect(contiguous_, &QCheckBox::toggled, canvas_, &Canvas::setContiguous);
    auto *deselect = new QToolButton;
    deselect->setDefaultAction(actions_["deselect"]);
    selection->addWidget(deselect);
    selection->addStretch();
    auto *shape = page();
    shapeMode_ = new QComboBox;
    shapeMode_->addItems({"", ""});
    shape->addWidget(shapeMode_);
    connect(shapeMode_, qOverload<int>(&QComboBox::activated), this,
            [this](int i) { setTool(i == 0 ? Tool::Rectangle : Tool::Ellipse); });
    label("radius", shape);
    auto *radius = new QSpinBox;
    radius->setRange(0, 5000);
    radius->setSuffix(" px");
    shape->addWidget(radius);
    connect(radius, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v) { canvas_->setCornerRadius(v); });
    auto *shapeColor = new QPushButton;
    buttons_.append({shapeColor, "color"});
    shape->addWidget(shapeColor);
    connect(shapeColor, &QPushButton::clicked, this, &MainWindow::chooseColor);
    shape->addStretch();
    auto *text = page();
    auto *textEdit = new QToolButton;
    textEdit->setDefaultAction(actions_["editText"]);
    text->addWidget(textEdit);
    label("textEditHint", text);
    text->addStretch();
    auto *gradient = page();
    gradientShape_ = new QComboBox;
    gradientShape_->addItems({"", ""});
    gradient->addWidget(gradientShape_);
    connect(gradientShape_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int v) { canvas_->setGradientRadial(v == 1); });
    gradientStyle_ = new QComboBox;
    gradientStyle_->addItems({"", ""});
    gradient->addWidget(gradientStyle_);
    connect(gradientStyle_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int v) { canvas_->setTransparentGradient(v == 0); });
    gradient->addStretch();
    auto *crop = page();
    label("ratio", crop);
    cropRatioBox_ = new QComboBox;
    for (const auto &entry : QList<QPair<QString, double>>{
             {"", 0}, {"1 : 1", 1}, {"4 : 3", 4. / 3}, {"3 : 4", 3. / 4}, {"16 : 9", 16. / 9}, {"9 : 16", 9. / 16}})
        cropRatioBox_->addItem(entry.first, entry.second);
    crop->addWidget(cropRatioBox_);
    connect(cropRatioBox_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int i) { canvas_->setCropRatio(cropRatioBox_->itemData(i).toDouble()); });
    label("toolHintCrop", crop);
    crop->addStretch();
    auto *navigation = page();
    for (const auto &key : QStringList{"fit", "actualSize", "zoomOut", "zoomIn"}) {
        auto *b = new QToolButton;
        b->setDefaultAction(actions_[key]);
        navigation->addWidget(b);
    }
    navigation->addStretch();
    auto *other = page();
    label("toolHintEyedropper", other);
    other->addStretch();
    mask_ = new QToolButton;
    mask_->setObjectName("maskTarget");
    mask_->setCheckable(true);
    mask_->setIcon(icon("mask"));
    mask_->setIconSize(QSize(20, 20));
    mask_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    headerRow->addWidget(mask_);
    connect(mask_, &QToolButton::toggled, canvas_, &Canvas::setMaskTarget);
    outer->addWidget(header);
    stack_ = new QStackedWidget;
    welcome_ = new QWidget;
    editor_ = new QWidget;
    stack_->addWidget(welcome_);
    stack_->addWidget(canvas_);
    outer->addWidget(editor_, 1);
    auto *body = new QHBoxLayout(editor_);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    auto *railScroll = new QScrollArea;
    railScroll->setObjectName("toolRail");
    railScroll->setFixedWidth(58);
    railScroll->setWidgetResizable(true);
    railScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *rail = new QWidget;
    auto *railLayout = new QVBoxLayout(rail);
    railLayout->setContentsMargins(8, 10, 8, 10);
    railLayout->setSpacing(3);
    const QVector<Tool> order{Tool::Move,      Tool::Marquee, Tool::Lasso,      Tool::Wand, Tool::Crop,
                              Tool::Brush,     Tool::Healing, Tool::Clone,      Tool::Blur, Tool::Gradient,
                              Tool::Rectangle, Tool::Text,    Tool::Eyedropper, Tool::Hand, Tool::Zoom};
    for (auto tool : order) {
        auto *b = toolButton(tools_[int(tool)], 36);
        b->setObjectName(ToolKeys[int(tool)] + "Button");
        b->setIconSize(QSize(24, 24));
        b->setAccessibleName(ToolKeys[int(tool)]);
        railLayout->addWidget(b);
        addAction(tools_[int(tool)]);
    }
    addAction(tools_[int(Tool::Eraser)]);
    addAction(tools_[int(Tool::Ellipse)]);
    railLayout->addSpacing(6);
    color_ = new QToolButton;
    color_->setObjectName("foregroundColor");
    color_->setFixedSize(30, 30);
    color_->setStyleSheet("background:#ed6a45;border:2px solid #b0b0b0;border-radius:4px;");
    railLayout->addWidget(color_, 0, Qt::AlignLeft);
    connect(color_, &QToolButton::clicked, this, &MainWindow::chooseColor);
    backgroundColor_ = new QToolButton;
    backgroundColor_->setFixedSize(28, 28);
    backgroundColor_->setStyleSheet("background:white;border:2px solid #777;border-radius:4px;");
    railLayout->addWidget(backgroundColor_, 0, Qt::AlignRight);
    connect(backgroundColor_, &QToolButton::clicked, this, [this] {
        auto c = QColorDialog::getColor(backgroundColorValue_, this, t("background"));
        if (c.isValid()) {
            backgroundColorValue_ = c;
            canvas_->setBackgroundColor(c);
            backgroundColor_->setStyleSheet("background:" + c.name() + ";border:2px solid #777;border-radius:4px;");
        }
    });
    railLayout->addStretch();
    railScroll->setWidget(rail);
    body->addWidget(railScroll);
    auto *split = new QSplitter;
    split->setObjectName("editorSplitter");
    split->setHandleWidth(2);
    split->setChildrenCollapsible(false);
    split->addWidget(stack_);
    auto *panel = new QWidget;
    panel->setMinimumWidth(235);
    panel->setMaximumWidth(430);
    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(0);
    auto *titleRow = new QHBoxLayout;
    titleRow->setContentsMargins(17, 15, 17, 15);
    label("layers", titleRow)->setObjectName("heading");
    titleRow->addStretch();
    layerCount_ = new QLabel;
    layerCount_->setStyleSheet("color:#828282;");
    titleRow->addWidget(layerCount_);
    panelLayout->addLayout(titleRow);
    auto *appearance = new QHBoxLayout;
    appearance->setContentsMargins(12, 0, 12, 12);
    appearance->setSpacing(7);
    blend_ = new QComboBox;
    blend_->setObjectName("blendMode");
    blend_->setMinimumWidth(110);
    for (const auto &mode : blendModes())
        blend_->addItem(mode, mode);
    appearance->addWidget(blend_, 1);
    opacity_ = new QSpinBox;
    opacity_->setObjectName("opacity");
    opacity_->setRange(0, 100);
    opacity_->setSuffix("%");
    opacity_->setFixedWidth(75);
    appearance->addWidget(opacity_);
    panelLayout->addLayout(appearance);
    connect(blend_, qOverload<int>(&QComboBox::activated), this, [this](int row) {
        invoke([&] {
            auto ids = selectedLayers();
            document_.edit([&](State &s) {
                for (auto &l : s.layers)
                    if (ids.contains(l.id) && !l.group)
                        l.blend = blend_->itemData(row).toString();
            });
        });
    });
    connect(opacity_, &QSpinBox::editingFinished, this, [this] {
        invoke([&] {
            auto ids = selectedLayers();
            const double value = opacity_->value() / 100.;
            document_.edit([&](State &s) {
                for (auto &l : s.layers)
                    if (ids.contains(l.id))
                        l.opacity = value;
            });
        });
    });
    auto *tree = new LayerTree;
    layers_ = tree;
    layers_->setObjectName("layerList");
    layers_->setItemDelegate(new LayerDelegate(layers_));
    layers_->setHeaderHidden(true);
    layers_->setColumnCount(3);
    layers_->setIconSize(QSize(42, 34));
    layers_->setRootIsDecorated(false);
    layers_->setIndentation(14);
    layers_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    layers_->setSelectionBehavior(QAbstractItemView::SelectRows);
    layers_->setDragDropMode(QAbstractItemView::InternalMove);
    layers_->setDefaultDropAction(Qt::MoveAction);
    layers_->setEditTriggers(QAbstractItemView::EditKeyPressed);
    layers_->setContextMenuPolicy(Qt::CustomContextMenu);
    layers_->header()->setStretchLastSection(false);
    layers_->header()->setSectionResizeMode(0, QHeaderView::Fixed);
    layers_->setColumnWidth(0, 28);
    layers_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    layers_->header()->setSectionResizeMode(2, QHeaderView::Fixed);
    layers_->setColumnWidth(2, 42);
    panelLayout->addWidget(layers_, 1);
    tree->moved = [this](QString id, QString parent, QString before) {
        QTimer::singleShot(0, this,
                           [this, id, parent, before] { invoke([&] { document_.relocate(id, parent, before); }); });
    };
    connect(layers_, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (!refreshing_)
            canvas_->setSelectedLayers(selectedLayers());
    });
    connect(layers_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (!refreshing_ && item) {
            mask_->setChecked(false);
            document_.select(item->data(0, Qt::UserRole).toString());
        }
    });
    connect(layers_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item, int column) {
        if (refreshing_)
            return;
        const QString id = item->data(0, Qt::UserRole).toString();
        if (column == 0)
            invoke([&] {
                document_.edit([&](State &s) {
                    int i = s.index(id);
                    s.layers[i].visible = !s.layers[i].visible;
                });
            });
        if (column == 2) {
            int i = document_.state().index(id);
            if (i >= 0 && !document_.state().layers[i].mask.isNull())
                mask_->setChecked(true);
        }
    });
    connect(layers_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int column) {
        if (column != 1)
            return;
        int i = document_.state().index(item->data(0, Qt::UserRole).toString());
        if (i >= 0 && document_.state().layers[i].metadata["adjustment"].isObject())
            filterDialog(document_.state().layers[i].metadata["adjustment"].toObject()["kind"].toString(), false,
                         document_.state().layers[i].id);
        else if (i >= 0 && document_.state().layers[i].metadata.contains("text"))
            editText();
        else if (i >= 0 && document_.state().layers[i].group)
            item->setExpanded(!item->isExpanded());
        else
            layers_->editItem(item, 1);
    });
    connect(layers_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int column) {
        if (refreshing_ || column != 1 || item->text(1).trimmed().isEmpty())
            return;
        const QString id = item->data(0, Qt::UserRole).toString(), name = item->text(1);
        QTimer::singleShot(0, this, [this, id, name] {
            invoke([&] {
                document_.edit([&](State &s) {
                    int i = s.index(id);
                    if (i >= 0)
                        s.layers[i].name = name;
                });
            });
        });
    });
    connect(layers_, &QTreeWidget::customContextMenuRequested, this, [this](QPoint point) {
        QMenu menu(this);
        for (const auto &key : QStringList{"addLayer", "groupSelection", "ungroup", "duplicate", "merge", "rename",
                                           "editText", "", "addMask", "clipMask", "invertMask", "applyMask",
                                           "deleteMask", "", "moveUp", "moveDown", "delete"})
            if (key.isEmpty())
                menu.addSeparator();
            else
                menu.addAction(actions_[key]);
        menu.exec(layers_->viewport()->mapToGlobal(point));
    });
    auto *footer = new QHBoxLayout;
    footer->setContentsMargins(9, 8, 9, 8);
    footer->setSpacing(2);
    for (const auto &key : QStringList{"addLayer", "groupSelection"})
        footer->addWidget(toolButton(actions_[key], 34));
    auto *maskMenuButton = new QToolButton;
    maskMenuButton->setIcon(icon("mask"));
    maskMenuButton->setIconSize(QSize(22, 22));
    maskMenuButton->setFixedSize(34, 34);
    maskMenuButton->setObjectName("maskMenuButton");
    connect(maskMenuButton, &QToolButton::clicked, this, &MainWindow::maskMenu);
    footer->addWidget(maskMenuButton);
    footer->addWidget(toolButton(actions_["layerEffects"], 34));
    auto *adjustButton = new QToolButton;
    adjustButton->setIcon(icon("adjustments"));
    adjustButton->setIconSize(QSize(22, 22));
    adjustButton->setFixedSize(34, 34);
    adjustButton->setMenu(adjustLayer);
    adjustButton->setPopupMode(QToolButton::InstantPopup);
    adjustButton->setObjectName("adjustmentButton");
    footer->addWidget(adjustButton);
    footer->addStretch();
    footer->addWidget(toolButton(actions_["delete"], 34));
    panelLayout->addLayout(footer);
    split->addWidget(panel);
    split->setSizes({1000, QSettings().value("panelWidth", 272).toInt()});
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 0);
    connect(split, &QSplitter::splitterMoved, this,
            [split](int, int) { QSettings().setValue("panelWidth", split->sizes().last()); });
    body->addWidget(split, 1);
    zoom_ = new QLabel("100%");
    zoom_->setMinimumWidth(54);
    dimensions_ = new QLabel;
    status_ = new QLabel;
    hint_ = new QLabel;
    hint_->setStyleSheet("color:#989898;");
    statusBar()->addWidget(zoom_);
    statusBar()->addWidget(dimensions_);
    statusBar()->addWidget(hint_, 1);
    statusBar()->addPermanentWidget(status_);
    statusBar()->setSizeGripEnabled(true);
    for (QWidget *target : QList<QWidget *>{canvas_, layers_}) {
        auto *shortcut = new QShortcut(QKeySequence("Delete"), target);
        shortcut->setContext(Qt::WidgetShortcut);
        connect(shortcut, &QShortcut::activated, this, [this] { actions_["delete"]->trigger(); });
    }
    buildWelcome();
}
void MainWindow::retranslate() {
    QFont face(localization_.language() == "zh-CN"   ? "Microsoft YaHei UI"
               : localization_.language() == "zh-TW" ? "Microsoft JhengHei UI"
                                                     : "Segoe UI");
    QApplication::setFont(face);
    for (auto it = actions_.begin(); it != actions_.end(); ++it) {
        it.value()->setText(t(it.key()));
        it.value()->setToolTip(t(it.key()) + (it.value()->shortcut().isEmpty()
                                                  ? QString()
                                                  : " (" + it.value()->shortcut().toString() + ")"));
        if (QStringList{"new", "open", "save", "addLayer", "addGroup", "groupSelection", "delete", "duplicate", "flipX",
                        "flipY", "zoomIn", "zoomOut", "addMask", "layerEffects"}
                .contains(it.key()))
            it.value()->setIcon(icon(it.key() == "groupSelection" ? "addGroup"
                                     : it.key() == "layerEffects" ? "effects"
                                                                  : it.key()));
    }
    for (int i = 0; i < ToolKeys.size(); ++i) {
        auto *a = tools_[i];
        a->setText(t(ToolKeys[i]));
        a->setIcon(icon(ToolKeys[i]));
        a->setToolTip(t(ToolKeys[i]) + " (" + a->shortcut().toString() + ")");
        auto *button = findChild<QToolButton *>(ToolKeys[i] + "Button");
        if (button)
            button->setAccessibleName(t(ToolKeys[i]));
    }
    for (const auto &key :
         QStringList{"file", "edit", "image", "layer", "select", "adjustments", "view", "help", "adjustmentLayer"}) {
        if (auto *menu = findChild<QMenu *>(key + "Menu"))
            menu->setTitle(t(key));
    }
    for (const auto &l : labels_)
        l.first->setText(l.second == "x" ? "X" : l.second == "y" ? "Y" : t(l.second));
    for (const auto &b : buttons_)
        b.first->setText(t(b.second));
    {
        QSignalBlocker block(language_);
        language_->setCurrentIndex(language_->findData(localization_.language()));
    }
    for (int i = 0; i < blend_->count(); ++i)
        blend_->setItemText(i, t("blend." + blend_->itemData(i).toString()));
    wandMode_->setItemText(0, t("wand"));
    wandMode_->setItemText(1, t("objectSelection"));
    retouchMode_->setItemText(0, t("blur"));
    retouchMode_->setItemText(1, t("smudge"));
    retouchMode_->setItemText(2, t("liquify"));
    lassoMode_->setItemText(0, t("freehand"));
    lassoMode_->setItemText(1, t("polygonal"));
    brushMode_->setItemText(0, t("brush"));
    brushMode_->setItemText(1, t("eraser"));
    shapeMode_->setItemText(0, t("rectangle"));
    shapeMode_->setItemText(1, t("ellipse"));
    marqueeShape_->setItemText(0, t("rectangle"));
    marqueeShape_->setItemText(1, t("ellipse"));
    int i = 0;
    for (const auto &key : QStringList{"replaceSelection", "addSelection", "subtractSelection", "intersectSelection"})
        selectionModeBox_->setItemText(i++, t(key));
    gradientShape_->setItemText(0, t("linear"));
    gradientShape_->setItemText(1, t("radial"));
    gradientStyle_->setItemText(0, t("foregroundTransparent"));
    gradientStyle_->setItemText(1, t("foregroundBackground"));
    cropRatioBox_->setItemText(0, t("freeRatio"));
    autoSelect_->setText(t("autoSelect"));
    contiguous_->setText(t("contiguous"));
    mask_->setText(t("mask"));
    canvas_->setShapeNames(t("rectangle"), t("ellipse"));
    color_->setToolTip(t("color"));
    backgroundColor_->setToolTip(t("background"));
    opacity_->setToolTip(t("opacity"));
    findChild<QToolButton *>("maskMenuButton")->setToolTip(t("mask"));
    findChild<QToolButton *>("adjustmentButton")->setToolTip(t("adjustmentLayer"));
    setTool(canvas_->tool());
    refreshSelection();
}
void MainWindow::setTool(Tool tool) {
    canvas_->setTool(tool);
    tools_[int(tool)]->setChecked(true);
    toolTitle_->setText(t(ToolKeys[int(tool)]));
    int page = 8;
    QString hint = "toolHintMove";
    switch (tool) {
    case Tool::Move:
        page = 0;
        break;
    case Tool::Brush:
    case Tool::Eraser:
    case Tool::Clone:
    case Tool::Blur:
    case Tool::Healing:
        page = 1;
        hint = tool == Tool::Clone ? "toolHintClone" : "toolHintBrush";
        break;
    case Tool::Marquee:
    case Tool::Lasso:
    case Tool::Wand:
        page = 2;
        hint = "toolHintSelection";
        break;
    case Tool::Rectangle:
    case Tool::Ellipse:
        page = 3;
        hint = "toolHintShape";
        break;
    case Tool::Text:
        page = 4;
        hint = "toolHintText";
        break;
    case Tool::Gradient:
        page = 5;
        hint = "toolHintGradient";
        break;
    case Tool::Crop:
        page = 6;
        hint = "toolHintCrop";
        break;
    case Tool::Hand:
    case Tool::Zoom:
        page = 7;
        hint = "toolHintHand";
        break;
    default:
        hint = "toolHintEyedropper";
    }
    toolOptions_->setCurrentIndex(page);
    {
        QSignalBlocker b(brushMode_), s(shapeMode_);
        brushMode_->setCurrentIndex(tool == Tool::Eraser ? 1 : 0);
        shapeMode_->setCurrentIndex(tool == Tool::Ellipse ? 1 : 0);
    }
    brushMode_->setVisible(tool == Tool::Brush || tool == Tool::Eraser);
    if (tool == Tool::Eraser)
        findChild<QToolButton *>("brushButton")->setChecked(true);
    if (tool == Tool::Ellipse)
        findChild<QToolButton *>("rectangleButton")->setChecked(true);
    wandMode_->setVisible(tool == Tool::Wand);
    retouchMode_->setVisible(tool == Tool::Blur);
    lassoMode_->setVisible(tool == Tool::Lasso);
    marqueeShape_->setVisible(tool == Tool::Marquee);
    tolerance_->setVisible(tool == Tool::Wand);
    findChild<QLabel *>("toleranceLabel")->setVisible(tool == Tool::Wand);
    contiguous_->setVisible(tool == Tool::Wand);
    hint_->setText(t(hint));
    mask_->setVisible(page == 1 || page == 5);
}
QStringList MainWindow::selectedLayers() const {
    QStringList ids;
    for (auto *item : layers_->selectedItems())
        ids.append(item->data(0, Qt::UserRole).toString());
    if (ids.isEmpty() && !document_.state().active.isEmpty())
        ids.append(document_.state().active);
    return ids;
}
void MainWindow::refresh() {
    refreshing_ = true;
    const auto selected = selectedLayers();
    QSet<QString> collapsed;
    QTreeWidgetItemIterator previous(layers_);
    while (*previous) {
        if (!(*previous)->isExpanded())
            collapsed.insert((*previous)->data(0, Qt::UserRole).toString());
        ++previous;
    }
    const int scroll = layers_->verticalScrollBar()->value();
    layers_->clear();
    QHash<QString, QTreeWidgetItem *> items;
    const auto &s = document_.state();
    auto order = s.drawOrder();
    std::reverse(order.begin(), order.end());
    auto thumbnail = [](const QImage &image) {
        QPixmap result(84, 68);
        result.setDevicePixelRatio(2);
        result.fill(QColor("#4b4b4b"));
        QPainter p(&result);
        for (int y = 0; y < 34; y += 6)
            for (int x = 0; x < 42; x += 6)
                if ((x / 6 + y / 6) % 2)
                    p.fillRect(x, y, 6, 6, QColor("#626262"));
        if (!image.isNull()) {
            QSizeF size = image.size();
            size.scale(40, 32, Qt::KeepAspectRatio);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.drawImage(QRectF(QPointF((42 - size.width()) / 2, (34 - size.height()) / 2), size), image);
        }
        p.setPen(QColor("#737373"));
        p.drawRect(QRectF(.5, .5, 41, 33));
        return QIcon(result);
    };
    std::function<QTreeWidgetItem *(int)> itemFor = [&](int i) -> QTreeWidgetItem * {
        const auto &l = s.layers[i];
        if (items.contains(l.id))
            return items[l.id];
        QTreeWidgetItem *parent = nullptr;
        if (!l.parent.isEmpty()) {
            int p = s.index(l.parent);
            if (p >= 0)
                parent = itemFor(p);
        }
        auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(layers_);
        items[l.id] = item;
        item->setData(0, Qt::UserRole, l.id);
        item->setData(0, Qt::UserRole + 1, l.group);
        item->setText(1, l.name);
        item->setIcon(0, l.visible ? QIcon(icon("eye").pixmap(18, 18)) : QIcon());
        item->setSizeHint(1, QSize(0, 52));
        item->setFlags(item->flags() | Qt::ItemIsEditable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
        item->setExpanded(!collapsed.contains(l.id));
        item->setIcon(1, l.group                             ? icon("group")
                         : l.metadata.contains("adjustment") ? icon("adjustments")
                                                             : thumbnail(l.image));
        if (!l.mask.isNull())
            item->setIcon(2, thumbnail(l.mask));
        item->setToolTip(1, l.name + (l.maskSource.isEmpty() ? QString() : " · " + t("clipMask")));
        item->setToolTip(2, t("mask"));
        if (selected.contains(l.id))
            item->setSelected(true);
        return item;
    };
    for (int i : order)
        itemFor(i);
    if (items.contains(s.active)) {
        layers_->setCurrentItem(items[s.active], 1, QItemSelectionModel::NoUpdate);
        items[s.active]->setSelected(true);
    }
    layers_->verticalScrollBar()->setValue(scroll);
    layerCount_->setText(QString::number(s.layers.size()));
    refreshing_ = false;
    canvas_->setSelectedLayers(selectedLayers());
    stack_->setCurrentWidget(document_.hasDocument() ? static_cast<QWidget *>(canvas_) : welcome_);
    refreshSelection();
}
void MainWindow::refreshSelection() {
    const auto &s = document_.state();
    const int i = s.index(s.active);
    const bool active = i >= 0, doc = document_.hasDocument(),
               pixels = active && !s.layers[i].group && !s.layers[i].metadata["adjustment"].isObject();
    const bool hasMask = active && !s.layers[i].mask.isNull();
    actions_["undo"]->setEnabled(!busy_ && document_.canUndo());
    actions_["redo"]->setEnabled(!busy_ && document_.canRedo());
    for (const auto &key :
         QStringList{"save", "saveAs", "export", "addLayer", "addGroup", "imageSize", "canvasSize", "trim",
                     "flipCanvasX", "flipCanvasY", "selectSubject", "removeBackground", "selectAll", "invertSelection",
                     "addGuide", "clearGuides", "closeDocument", "paste", "copyMerged"})
        actions_[key]->setEnabled(!busy_ && doc);
    for (const auto &key :
         QStringList{"duplicate", "delete", "rename", "transformLayers", "freeDistort", "moveUp", "moveDown",
                     "groupSelection", "merge", "addMask", "hideMask", "copy", "cut", "selectPixels"})
        actions_[key]->setEnabled(!busy_ && active);
    for (const auto &key : QStringList{"flipX", "flipY", "clipMask", "fillForeground", "fillBackground", "clearPixels"})
        actions_[key]->setEnabled(!busy_ && pixels);
    for (const auto &key : QStringList{"invertMask", "enableMask", "linkMask", "applyMask", "deleteMask", "selectMask"})
        actions_[key]->setEnabled(!busy_ && hasMask);
    for (const auto &key :
         QStringList{"deselect", "expandSelection", "contractSelection", "featherSelection", "contentFill"})
        actions_[key]->setEnabled(!busy_ && s.selectionActive);
    for (auto it = actions_.begin(); it != actions_.end(); ++it) {
        if (it.key().startsWith("filter."))
            it.value()->setEnabled(!busy_ && pixels);
        if (it.key().startsWith("adjustment."))
            it.value()->setEnabled(!busy_ && doc);
    }
    actions_["layerEffects"]->setEnabled(!busy_ && active);
    actions_["ungroup"]->setEnabled(!busy_ && active && s.layers[i].group);
    actions_["editText"]->setEnabled(!busy_ && pixels && s.layers[i].metadata.contains("text"));
    for (auto *box : {x_, y_, width_, height_, rotation_})
        box->setEnabled(!busy_ && active);
    for (auto *box : {width_, height_, rotation_})
        box->setEnabled(!busy_ && pixels);
    blend_->setEnabled(!busy_ && pixels);
    opacity_->setEnabled(!busy_ && active);
    mask_->setEnabled(!busy_ && active);
    if (!active || (!hasMask && maskLayer_ != s.active))
        mask_->setChecked(false);
    maskLayer_ = s.active;
    if (!refreshing_ && active &&
        (!layers_->currentItem() || layers_->currentItem()->data(0, Qt::UserRole).toString() != s.active)) {
        QSignalBlocker block(layers_);
        layers_->clearSelection();
        QTreeWidgetItemIterator it(layers_);
        while (*it) {
            if ((*it)->data(0, Qt::UserRole).toString() == s.active) {
                layers_->setCurrentItem(*it, 1);
                (*it)->setSelected(true);
                break;
            }
            ++it;
        }
        canvas_->setSelectedLayers(selectedLayers());
    }
    if (active) {
        const auto &l = s.layers[i];
        QSignalBlocker b(blend_), o(opacity_);
        blend_->setCurrentIndex(blend_->findData(l.blend));
        opacity_->setValue(qRound(l.opacity * 100));
        for (auto pair : QList<QPair<QDoubleSpinBox *, double>>{{x_, l.placement.origin.x()},
                                                                {y_, l.placement.origin.y()},
                                                                {width_, l.placement.size.width()},
                                                                {height_, l.placement.size.height()},
                                                                {rotation_, l.placement.rotation}})
            if (!pair.first->hasFocus())
                pair.first->setValue(pair.second);
    }
    dimensions_->setText(
        doc ? QString::number(s.size.width()) + " × " + QString::number(s.size.height()) + " px   ·   sRGB" : "");
    status_->setText(busy_ ? t("busy") : doc ? t(document_.dirty() ? "unsaved" : "saved") : t("ready"));
    tabs_->setEnabled(!busy_);
    updateTitle();
}
} // namespace comp
