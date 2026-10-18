#pragma once
#include <QByteArray>
#include <QString>
#include <QUrl>
#include <optional>
namespace comp {
struct UpdateInfo {
    QString version, filename;
    QUrl download;
    QByteArray sha256;
    qint64 size = 0;
};
// Untrusted GitHub metadata is validated before offering any download.
std::optional<UpdateInfo> findUpdate(const QByteArray &json, const QString &currentVersion);
class MainWindow;
void checkForUpdates(MainWindow *window, bool interactive);
} // namespace comp
