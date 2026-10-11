#pragma once
#include "Localization.h"
#include <QWidget>
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
    bool rememberWindow = true, autoReload = true;
    static Preferences load();
    void save() const;
};
class PreferencesPage : public QWidget {
    Q_OBJECT
  public:
    PreferencesPage(Localization *localization, QList<QAction *> actions, QWidget *parent = nullptr);
    void refresh();
    void showShortcuts();
  signals:
    void settingsChanged();
    void shortcutsChanged();
    void returnRequested();

  private:
    Localization *localization_;
    QTabWidget *sections_;
    QComboBox *language_, *depth_, *resolution_;
    QSpinBox *spacing_;
    QCheckBox *remember_, *reload_, *rulers_, *grid_, *guides_, *snap_;
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
