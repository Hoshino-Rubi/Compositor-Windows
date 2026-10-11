#include "Preferences.h"
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QScrollArea>
#include <QSettings>
#include <QLineEdit>
#include <QSet>
#include <QSignalBlocker>
#include <algorithm>
namespace comp {
Preferences Preferences::load() {
    QSettings s;
    Preferences p;
    p.depth = s.value("preferences/defaultDepth", 8).toInt() == 16 ? 16 : 8;
    p.resolution = s.value("preferences/defaultResolution", 72).toInt() == 300 ? 300 : 72;
    p.gridSpacing = std::clamp(s.value("preferences/gridSpacing", 100).toInt(), 4, 2000);
    p.rulers = s.value("preferences/rulers", false).toBool();
    p.grid = s.value("preferences/grid", false).toBool();
    p.guides = s.value("preferences/guides", true).toBool();
    p.snap = s.value("preferences/snap", true).toBool();
    p.rememberWindow = s.value("preferences/rememberWindow", true).toBool();
    p.autoReload = s.value("preferences/autoReload", true).toBool();
    return p;
}
void Preferences::save() const {
    QSettings s;
    s.setValue("preferences/defaultDepth", depth);
    s.setValue("preferences/defaultResolution", resolution);
    s.setValue("preferences/gridSpacing", gridSpacing);
    s.setValue("preferences/rulers", rulers);
    s.setValue("preferences/grid", grid);
    s.setValue("preferences/guides", guides);
    s.setValue("preferences/snap", snap);
    s.setValue("preferences/rememberWindow", rememberWindow);
    s.setValue("preferences/autoReload", autoReload);
}
PreferencesPage::PreferencesPage(Localization *localization, QList<QAction *> actions, QWidget *parent)
    : QWidget(parent), localization_(localization), actions_(std::move(actions)) {
    setObjectName("preferencesPage");
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(28, 22, 28, 22);
    outer->setSpacing(16);
    auto label = [&](const QString &key) {
        auto *l = new QLabel;
        l->setWordWrap(true);
        labels_.append({l, key});
        return l;
    };
    auto *title = label("preferences");
    title->setStyleSheet("font-size:24px;font-weight:600;");
    outer->addWidget(title);
    outer->addWidget(label("preferencesNote"));
    sections_ = new QTabWidget;
    sections_->setObjectName("preferenceSections");
    outer->addWidget(sections_, 1);
    auto section = [&] {
        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        auto *body = new QWidget;
        auto *layout = new QVBoxLayout(body);
        layout->setContentsMargins(24, 24, 24, 24);
        layout->setSpacing(18);
        scroll->setWidget(body);
        sections_->addTab(scroll, "");
        return layout;
    };
    auto *general = section();
    auto *form = new QFormLayout;
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setVerticalSpacing(16);
    language_ = new QComboBox;
    language_->setObjectName("languageSelector");
    language_->addItem("", "system");
    for (const auto &language : Localization::languages())
        language_->addItem(language.second, language.first);
    language_->setMinimumWidth(230);
    form->addRow(label("language"), language_);
    general->addLayout(form);
    general->addWidget(label("languageNote"));
    remember_ = new QCheckBox;
    remember_->setObjectName("rememberWindowPreference");
    reload_ = new QCheckBox;
    reload_->setObjectName("autoReloadPreference");
    general->addWidget(remember_);
    general->addWidget(reload_);
    general->addWidget(label("autoReloadNote"));
    general->addStretch();
    auto *canvas = section();
    auto *canvasForm = new QFormLayout;
    canvasForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    canvasForm->setVerticalSpacing(16);
    depth_ = new QComboBox;
    depth_->setObjectName("defaultDepthPreference");
    depth_->addItem("", 8);
    depth_->addItem("", 16);
    resolution_ = new QComboBox;
    resolution_->setObjectName("defaultResolutionPreference");
    resolution_->addItem("72 DPI", 72);
    resolution_->addItem("300 DPI", 300);
    spacing_ = new QSpinBox;
    spacing_->setObjectName("gridSpacingPreference");
    spacing_->setRange(4, 2000);
    canvasForm->addRow(label("defaultBitDepth"), depth_);
    canvasForm->addRow(label("defaultResolution"), resolution_);
    canvasForm->addRow(label("gridSpacing"), spacing_);
    canvas->addLayout(canvasForm);
    canvas->addWidget(label("newDocumentDefaultsNote"));
    rulers_ = new QCheckBox;
    grid_ = new QCheckBox;
    guides_ = new QCheckBox;
    snap_ = new QCheckBox;
    rulers_->setObjectName("rulersPreference");
    grid_->setObjectName("gridPreference");
    guides_->setObjectName("guidesPreference");
    snap_->setObjectName("snapPreference");
    for (auto *box : {rulers_, grid_, guides_, snap_})
        canvas->addWidget(box);
    canvas->addStretch();
    auto *shortcutPage = new QWidget;
    auto *shortcutLayout = new QVBoxLayout(shortcutPage);
    shortcutLayout->setContentsMargins(18, 18, 18, 18);
    shortcuts_ = new QTableWidget;
    shortcuts_->setObjectName("shortcutTable");
    shortcuts_->setColumnCount(2);
    shortcuts_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    shortcuts_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    shortcuts_->verticalHeader()->hide();
    shortcuts_->setAlternatingRowColors(false);
    std::sort(actions_.begin(), actions_.end(), [](auto *a, auto *b) { return a->objectName() < b->objectName(); });
    shortcuts_->setRowCount(actions_.size());
    for (int i = 0; i < actions_.size(); ++i) {
        auto *item = new QTableWidgetItem;
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        shortcuts_->setItem(i, 0, item);
        auto *editor = new QKeySequenceEdit(actions_[i]->shortcut());
        editor->setObjectName(actions_[i]->objectName() + "Shortcut");
        editor->setMaximumSequenceLength(1);
        editor->setClearButtonEnabled(true);
        shortcuts_->setCellWidget(i, 1, editor);
        shortcuts_->setRowHeight(i, 42);
        editors_.append(editor);
        connect(editor, &QKeySequenceEdit::keySequenceChanged, this, [this] {
            if (!refreshing_) {
                shortcutDirty_ = true;
                conflict_.clear();
                updateShortcutStatus();
            }
        });
    }
    shortcutLayout->addWidget(shortcuts_, 1);
    auto *shortcutFooter = new QHBoxLayout;
    shortcutStatus_ = new QLabel;
    shortcutStatus_->setObjectName("shortcutStatus");
    shortcutStatus_->setWordWrap(true);
    shortcutFooter->addWidget(shortcutStatus_, 1);
    apply_ = new QPushButton;
    apply_->setObjectName("applyShortcuts");
    buttons_.append({apply_, "apply"});
    shortcutFooter->addWidget(apply_);
    shortcutLayout->addLayout(shortcutFooter);
    sections_->addTab(shortcutPage, "");
    auto *footer = new QHBoxLayout;
    auto *reset = new QPushButton;
    reset->setObjectName("resetPreferences");
    buttons_.append({reset, "resetSection"});
    footer->addWidget(reset);
    footer->addStretch();
    auto *back = new QPushButton;
    back->setObjectName("returnToWorkspace");
    buttons_.append({back, "backToWorkspace"});
    footer->addWidget(back);
    outer->addLayout(footer);
    connect(back, &QPushButton::clicked, this, &PreferencesPage::returnRequested);
    connect(reset, &QPushButton::clicked, this, &PreferencesPage::resetSection);
    connect(apply_, &QPushButton::clicked, this, &PreferencesPage::applyShortcuts);
    connect(language_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        if (!refreshing_) {
            localization_->setLanguage(language_->currentData().toString());
            QSettings().setValue("language", localization_->selection());
        }
    });
    for (auto *box : {remember_, reload_, rulers_, grid_, guides_, snap_})
        connect(box, &QCheckBox::toggled, this, &PreferencesPage::saveSettings);
    for (auto *box : {depth_, resolution_})
        connect(box, qOverload<int>(&QComboBox::currentIndexChanged), this, &PreferencesPage::saveSettings);
    connect(spacing_, qOverload<int>(&QSpinBox::valueChanged), this, &PreferencesPage::saveSettings);
    connect(localization_, &Localization::changed, this, &PreferencesPage::retranslate);
    refresh();
}
void PreferencesPage::refresh() {
    refreshing_ = true;
    const auto p = Preferences::load();
    language_->setCurrentIndex(language_->findData(localization_->selection()));
    depth_->setCurrentIndex(depth_->findData(p.depth));
    resolution_->setCurrentIndex(resolution_->findData(p.resolution));
    spacing_->setValue(p.gridSpacing);
    remember_->setChecked(p.rememberWindow);
    reload_->setChecked(p.autoReload);
    rulers_->setChecked(p.rulers);
    grid_->setChecked(p.grid);
    guides_->setChecked(p.guides);
    snap_->setChecked(p.snap);
    if (!shortcutDirty_)
        for (int i = 0; i < actions_.size(); ++i)
            editors_[i]->setKeySequence(actions_[i]->shortcut());
    refreshing_ = false;
    retranslate();
}
void PreferencesPage::showShortcuts() {
    sections_->setCurrentIndex(2);
}
void PreferencesPage::saveSettings() {
    if (refreshing_)
        return;
    Preferences p;
    p.depth = depth_->currentData().toInt();
    p.resolution = resolution_->currentData().toInt();
    p.gridSpacing = spacing_->value();
    p.rememberWindow = remember_->isChecked();
    p.autoReload = reload_->isChecked();
    p.rulers = rulers_->isChecked();
    p.grid = grid_->isChecked();
    p.guides = guides_->isChecked();
    p.snap = snap_->isChecked();
    p.save();
    emit settingsChanged();
}
void PreferencesPage::resetSection() {
    if (sections_->currentIndex() == 2) {
        for (int i = 0; i < actions_.size(); ++i)
            editors_[i]->setKeySequence(
                QKeySequence(actions_[i]->property("originalShortcut").toString(), QKeySequence::PortableText));
        return;
    }
    auto p = Preferences::load();
    const Preferences defaults;
    if (sections_->currentIndex() == 0) {
        p.rememberWindow = defaults.rememberWindow;
        p.autoReload = defaults.autoReload;
        localization_->setLanguage("system");
        QSettings().setValue("language", "system");
    } else {
        p.depth = defaults.depth;
        p.resolution = defaults.resolution;
        p.gridSpacing = defaults.gridSpacing;
        p.rulers = defaults.rulers;
        p.grid = defaults.grid;
        p.guides = defaults.guides;
        p.snap = defaults.snap;
    }
    p.save();
    refresh();
    emit settingsChanged();
}
void PreferencesPage::applyShortcuts() {
    // Escape, Delete and Ctrl+Y are contextual/fixed workspace shortcuts.
    QSet<QString> used{"Esc", "Del", "Ctrl+Y"};
    for (auto *editor : editors_) {
        const auto key = editor->keySequence().toString(QKeySequence::PortableText);
        if (key.isEmpty())
            continue;
        if (used.contains(key)) {
            conflict_ = key;
            updateShortcutStatus();
            editor->setFocus();
            return;
        }
        used.insert(key);
    }
    QSettings s;
    for (int i = 0; i < actions_.size(); ++i) {
        actions_[i]->setShortcut(editors_[i]->keySequence());
        const QString value = editors_[i]->keySequence().toString(QKeySequence::PortableText);
        const QString key = "shortcuts/" + actions_[i]->objectName();
        if (value == actions_[i]->property("originalShortcut").toString())
            s.remove(key);
        else
            s.setValue(key, value);
    }
    shortcutDirty_ = false;
    conflict_.clear();
    emit shortcutsChanged();
    updateShortcutStatus();
}
void PreferencesPage::updateShortcutStatus() {
    shortcutStatus_->setText(!conflict_.isEmpty() ? t("shortcutConflict").replace("{detail}", conflict_)
                             : shortcutDirty_     ? t("shortcutPending")
                                                  : t("shortcutNote"));
    shortcutStatus_->setStyleSheet(conflict_.isEmpty() ? "" : "color:#ffb3a7;");
    apply_->setEnabled(shortcutDirty_);
}
void PreferencesPage::retranslate() {
    for (const auto &entry : labels_)
        entry.first->setText(t(entry.second));
    for (const auto &entry : buttons_)
        entry.first->setText(t(entry.second));
    sections_->setTabText(0, t("general"));
    sections_->setTabText(1, t("canvasPreferences"));
    sections_->setTabText(2, t("shortcuts"));
    language_->setItemText(0, t("followSystem"));
    {
        QSignalBlocker block(language_);
        language_->setCurrentIndex(language_->findData(localization_->selection()));
    }
    depth_->setItemText(0, t("depth8"));
    depth_->setItemText(1, t("depth16"));
    remember_->setText(t("rememberWindow"));
    reload_->setText(t("autoReload"));
    rulers_->setText(t("showRulers"));
    grid_->setText(t("showGrid"));
    guides_->setText(t("showGuides"));
    snap_->setText(t("snap"));
    shortcuts_->setHorizontalHeaderLabels({t("command"), t("shortcut")});
    for (int i = 0; i < actions_.size(); ++i) {
        QString key = actions_[i]->objectName();
        if (key.endsWith("Action"))
            key.chop(6);
        else if (key.endsWith("Tool"))
            key.chop(4);
        shortcuts_->item(i, 0)->setText(t(key));
        if (auto *line = editors_[i]->findChild<QLineEdit *>()) {
            line->setPlaceholderText(t("pressShortcut"));
            line->setText(editors_[i]->keySequence().toString(QKeySequence::NativeText));
            if (auto *clear = line->findChild<QAction *>("_q_qlineeditclearaction"))
                clear->setToolTip(t("clearShortcut"));
        }
    }
    updateShortcutStatus();
}
} // namespace comp
