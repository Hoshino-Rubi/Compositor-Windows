#pragma once
#include <QObject>
#include <QJsonObject>
#include <QTranslator>
namespace comp {
class Localization : public QObject {
    Q_OBJECT
  public:
    explicit Localization(QObject *parent = nullptr);
    QString text(const QString &key) const;
    QString language() const { return language_; }
    void setLanguage(const QString &language);
    static QString normalize(const QString &language);
  signals:
    void changed();

  private:
    QString language_;
    QJsonObject english_, current_;
    QTranslator qt_;
};
} // namespace comp
