#pragma once
#include "Localization.h"
#include <QDialog>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QKeySequenceEdit>
namespace comp {
struct Preferences {
    int depth = 8, resolution = 72, gridSpacing = 100;
    bool rulers = false, grid = false, guides = true, snap = true;
    bool snapGuides = true, snapGrid = false, snapLayers = true, snapBounds = true;
    bool lockGuides = false, pixelGrid = false, transformControls = true;
    bool rememberWindow = true, autoReload = true;
    bool gpuEditingExperimental = false;
    static Preferences load();
    static bool gpuEditingEnabled();
    void save() const;
};
class PreferencesDialog : public QDialog {
    Q_OBJECT
  public:
    PreferencesDialog(Localization *localization, QList<QAction *> actions, QWidget *parent = nullptr);
    void refresh();
    void showShortcuts();
  signals:
    void settingsChanged();
    void shortcutsChanged();

  private:
    Localization *localization_;
    QTabWidget *sections_;
    QComboBox *language_, *depth_, *resolution_;
    QSpinBox *spacing_;
    QCheckBox *remember_, *reload_, *gpuEditingExperimental_, *rulers_, *grid_, *guides_, *snap_;
    QCheckBox *snapGuides_, *snapGrid_, *snapLayers_, *snapBounds_, *lockGuides_, *pixelGrid_, *transformControls_;
    QTableWidget *shortcuts_;
    QLabel *shortcutStatus_;
    QPushButton *apply_;
    QList<QAction *> actions_;
    QList<QKeySequenceEdit *> editors_;
    QVector<QPair<QLabel *, QString>> labels_;
    QVector<QPair<QPushButton *, QString>> buttons_;
    bool refreshing_ = false, shortcutDirty_ = false;
    QString conflict_;
    QString t(const QString &key) const { return localization_->text(key); }
    void retranslate();
    void saveSettings();
    void resetSection();
    void applyShortcuts();
    void updateShortcutStatus();
};
} // namespace comp
