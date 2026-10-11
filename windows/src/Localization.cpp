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
    setLanguage(QSettings().value("language", "system").toString());
}
QList<QPair<QString, QString>> Localization::languages() {
    return {{"en", "English"}, {"zh-CN", "简体中文"}, {"zh-TW", "繁體中文"}, {"ja", "日本語"},
            {"ko", "한국어"},  {"de", "Deutsch"},     {"fr", "Français"},    {"es", "Español"}};
}
QString Localization::fontFamily(const QString &language) {
    if (language == "zh-CN")
        return "Microsoft YaHei UI";
    if (language == "zh-TW")
        return "Microsoft JhengHei UI";
    if (language == "ja")
        return "Yu Gothic UI";
    if (language == "ko")
        return "Malgun Gothic";
    return "Segoe UI";
}
QString Localization::normalize(const QString &language) {
    const auto name = language.trimmed().toLower().replace('_', '-');
    if (name == "system" || name.isEmpty()) {
        for (const auto &candidate : QLocale::system().uiLanguages()) {
            const auto base = candidate.toLower().section('-', 0, 0);
            if (base == "zh" || base == "en" || base == "ja" || base == "ko" || base == "de" || base == "fr" ||
                base == "es")
                return normalize(candidate);
        }
        return "en";
    }
    if (name == "zh" || name.startsWith("zh-"))
        return name.contains("tw") || name.contains("hk") || name.contains("mo") || name.contains("hant") ? "zh-TW"
                                                                                                          : "zh-CN";
    for (const auto &entry : languages())
        if (name == entry.first || name.startsWith(entry.first + "-"))
            return entry.first;
    return "en";
}
QString Localization::text(const QString &key) const {
    return current_.value(key).toString(english_.value(key).toString(key));
}
void Localization::setLanguage(const QString &language) {
    const auto normalized = normalize(language);
    const QString selection =
        language.trimmed().isEmpty() || language.compare("system", Qt::CaseInsensitive) == 0 ? "system" : normalized;
    if (language_ == normalized && selection_ == selection)
        return;
    selection_ = selection;
    language_ = normalized;
    current_ = dictionary(language_);
    qApp->removeTranslator(&qt_);
    const QString suffix = language_ == "zh-TW" ? "zh_TW" : language_ == "zh-CN" ? "zh_CN" : language_;
    const QString bundled = QCoreApplication::applicationDirPath() + "/translations";
    const QString directory = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
    if (qt_.load("qtbase_" + suffix, bundled) || qt_.load("qt_" + suffix, bundled) ||
        qt_.load("qtbase_" + suffix, directory) || qt_.load("qt_" + suffix, directory))
        qApp->installTranslator(&qt_);
    QLocale::setDefault(QLocale(language_));
    if (!qEnvironmentVariableIsSet("COMPOSITOR_HEADLESS_TEST"))
        QSettings().setValue("language", selection_);
    emit changed();
}
} // namespace comp
