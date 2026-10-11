#pragma once
#include <QObject>
#include <QJsonObject>
#include <QTranslator>
#include <QList>
#include <QPair>
namespace comp {
class Localization : public QObject {
    Q_OBJECT
  public:
    explicit Localization(QObject *parent = nullptr);
    QString text(const QString &key) const;
    QString language() const { return language_; }
    QString selection() const { return selection_; }
    static QList<QPair<QString, QString>> languages();
    static QString fontFamily(const QString &language);
    void setLanguage(const QString &language);
    static QString normalize(const QString &language);
  signals:
    void changed();

  private:
    QString language_, selection_;
    QJsonObject english_, current_;
    QTranslator qt_;
};
} // namespace comp
