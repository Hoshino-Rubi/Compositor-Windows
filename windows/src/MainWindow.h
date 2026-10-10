#pragma once
#include "Canvas.h"
#include "Localization.h"
#include <QMainWindow>
#include <QFutureWatcher>
#include <QLabel>
#include <QTreeWidget>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QSlider>
#include <QToolButton>
#include <QStackedWidget>
#include <QActionGroup>
#include <functional>
#include <QPushButton>
#include <exception>
#include <QTabBar>
#include <QCheckBox>
#include <QJsonObject>
namespace comp {
class MainWindow : public QMainWindow {
    Q_OBJECT
  public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
    Document *document() { return &document_; }
    Localization *localization() { return &localization_; }
    Canvas *canvas() { return canvas_; }
    void openProject(const QString &path);
    void sample();
    void showError(const QString &key, const QString &detail = {});

  protected:
    void closeEvent(QCloseEvent *) override;

  private:
    Document document_;
    Localization localization_;
    Canvas *canvas_;
    QStackedWidget *stack_;
    QWidget *welcome_, *editor_;
    QTreeWidget *layers_;
    QComboBox *blend_, *language_;
    QDoubleSpinBox *x_, *y_, *width_, *height_, *rotation_;
    QSpinBox *opacity_, *brushSize_;
    QSlider *flow_;
    QToolButton *color_, *mask_;
    QLabel *status_, *zoom_, *hint_, *dimensions_;
    QHash<QString, QAction *> actions_;
    QHash<int, QAction *> tools_;
    QVector<QPair<QLabel *, QString>> labels_;
    QVector<QPair<QPushButton *, QString>> buttons_;
    QString path_, name_;
    QByteArray diskDigest_;
    quint64 savedRevision_ = 0;
    bool busy_ = false, refreshing_ = false;
    QFutureWatcher<void> io_;
    QString pendingPath_;
    QByteArray pendingDigest_;
    std::exception_ptr pendingError_;
    std::function<void()> completion_;
    QString t(const QString &key) const { return localization_.text(key); }
    void commandPalette();
    QJsonObject lastFilter_;
    void buildActions();
    void buildUi();
    void retranslate();
    void refresh();
    void refreshSelection();
    void updateTitle();
    void chooseColor();
    void newDialog();
    void addText(QPointF point);
    void importImages(QStringList paths = {});
    void exportImage();
    bool save(bool as = false, std::function<void(bool)> after = {});
    bool maybeSave();
    void runIO(std::function<void()> work, std::function<void()> done);
    void setBusy(bool busy);
    void remember(const QString &path);
    void invoke(const std::function<void()> &action);
    void applyPlacement();
    void setTool(Tool tool);
    void buildWelcome();
    void buildEditingActions();
    void filterDialog(const QString &kind, bool adjustment = false, QString editLayer = {});
    void textDialog(QPointF point, QString layer = {});
    void sizeDialog(bool image);
    void editText();
    void maskMenu();
    void effectsDialog();
    void cameraRawDialog();
    void selectSubject(QPoint point = QPoint(-1, -1), bool removeBackground = false);
    void shortcutsDialog();
    void transformDialog(bool distort = false);
    void watchProjects();
    QTimer projectWatchTimer_;
    bool checkingProjects_ = false;
    QFutureWatcher<QVector<LoadedProject>> projectWatcher_;
    QVector<QString> watchPaths_;
    QHash<QString, QByteArray> watchStamps_;
    QSet<QString> changedProjects_;
    void importNext(QStringList paths);
    void importPhotoshop(const QString &path);
    void importRaw(const QString &path, QStringList remaining);
    void insertImported(const QString &name, const QImage &image);
    QStringList selectedLayers() const;
    void stashTab();
    void adoptDocument(State state, QString name, QString path = {}, QByteArray digest = {}, bool saved = false);
    void switchTab(int index);
    bool closeTab(int index);
    struct Tab {
        Document::Session session;
        QString path, name;
        QByteArray digest;
        double zoom = 1;
    };
    QVector<Tab> documents_;
    int currentTab_ = -1;
    bool switchingTab_ = false;
    QTabBar *tabs_ = nullptr;
    QStackedWidget *toolOptions_ = nullptr;
    QLabel *toolTitle_ = nullptr, *layerCount_ = nullptr;
    QComboBox *brushMode_ = nullptr, *shapeMode_ = nullptr, *retouchMode_ = nullptr, *lassoMode_ = nullptr,
              *wandMode_ = nullptr;
    QComboBox *selectionModeBox_ = nullptr, *marqueeShape_ = nullptr, *gradientShape_ = nullptr,
              *gradientStyle_ = nullptr, *cropRatioBox_ = nullptr;
    QSpinBox *hardness_ = nullptr, *tolerance_ = nullptr;
    QCheckBox *autoSelect_ = nullptr, *contiguous_ = nullptr;
    QToolButton *backgroundColor_ = nullptr;
    QColor backgroundColorValue_{Qt::white};
    QString maskLayer_;
};
} // namespace comp
