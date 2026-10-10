#include "Localization.h"
#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QLibraryInfo>
#include <QLocale>
#include <QSettings>
namespace comp {
static QJsonObject dictionary(const QString &locale) {
    QFile file(":/translations/" + locale + ".json");
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
Localization::Localization(QObject *parent) : QObject(parent), english_(dictionary("en")) {
    setLanguage(QSettings().value("language", QLocale::system().name()).toString());
}
QString Localization::normalize(const QString &language) {
    const auto name = language.toLower();
    if (name.startsWith("zh"))
        return name.contains("tw") || name.contains("hk") || name.contains("mo") || name.contains("hant") ? "zh-TW"
                                                                                                          : "zh-CN";
    return "en";
}
QString Localization::text(const QString &key) const {
    return current_.value(key).toString(english_.value(key).toString(key));
}
void Localization::setLanguage(const QString &language) {
    const auto normalized = normalize(language);
    if (language_ == normalized)
        return;
    language_ = normalized;
    current_ = dictionary(language_);
    qApp->removeTranslator(&qt_);
    const QString suffix = language_ == "zh-TW" ? "zh_TW" : language_ == "zh-CN" ? "zh_CN" : "en";
    const QString directory = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
    if (qt_.load("qtbase_" + suffix, directory) || qt_.load("qt_" + suffix, directory))
        qApp->installTranslator(&qt_);
    QLocale::setDefault(QLocale(language_));
    if (!qEnvironmentVariableIsSet("COMPOSITOR_HEADLESS_TEST"))
        QSettings().setValue("language", language_);
    emit changed();
}
} // namespace comp
