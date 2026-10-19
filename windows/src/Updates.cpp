#include "Updates.h"
#include "MainWindow.h"
#include "UpdateInstaller.h"
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QJsonObject>
#include <QUuid>
#include <QFutureWatcher>
#include <QtConcurrent>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <Windows.h>
#endif
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <array>
#include <memory>
namespace comp {
namespace {
using Version = std::array<int, 6>;
std::optional<Version> version(const QString &text) {
    static const QRegularExpression re(R"(^(\d{1,5})\.(\d{1,5})\.(\d{1,5})(?:a(\d{1,5})(?:p(\d{1,5}))?)?$)");
    const auto m = re.match(text);
    if (!m.hasMatch())
        return {};
    return Version{m.captured(1).toInt(),           m.captured(2).toInt(), m.captured(3).toInt(),
                   m.captured(4).isEmpty() ? 1 : 0, m.captured(4).toInt(), m.captured(5).toInt()};
}
QNetworkRequest request(const QUrl &url) {
    QNetworkRequest r(url);
    r.setRawHeader("User-Agent", "Compositor-Windows-Update-Check");
    r.setRawHeader("Accept", "application/vnd.github+json");
    r.setTransferTimeout(15000);
    r.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    return r;
}
bool launchInstaller(MainWindow *window, const QString &jobPath) {
#ifdef Q_OS_WIN
    QStringList reopen;
    if (!window->prepareForUpdate(reopen)) return false;
    auto job = readUpdateJob(jobPath);
    job["reopen"] = QJsonArray::fromStringList(reopen);
    job["failureMessage"] = window->localization()->text("updateInstallFailure");
    QSaveFile file(jobPath);
    const auto bytes = QJsonDocument(job).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error("Cannot save update restart information.");
    const auto name = "Local\\Compositor-Update-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto cancelName = name + "-cancel";
    const auto wide = [](const QString &s) { return reinterpret_cast<LPCWSTR>(s.utf16()); };
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, wide(name));
    HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, wide(cancelName));
    if (!ready || !cancel) {
        if (ready) CloseHandle(ready);
        if (cancel) CloseHandle(cancel);
        throw std::runtime_error("Cannot create updater handshake.");
    }
    const auto control = QFileInfo(jobPath).absolutePath() + "/control";
    QProcess helper;
    helper.setProgram(control + "/CompositorUpdater.exe");
    helper.setArguments({"--apply-update", jobPath, QString::number(QCoreApplication::applicationPid()), name});
    helper.setWorkingDirectory(control);
    helper.setProcessChannelMode(QProcess::ForwardedChannels);
    const bool started = helper.startDetached();
    const bool prepared = started && WaitForSingleObject(ready, 10000) == WAIT_OBJECT_0;
    if (!prepared) SetEvent(cancel);
    CloseHandle(ready); CloseHandle(cancel);
    if (!prepared) throw std::runtime_error("The updater did not become ready; the application was not closed.");
    window->exitForUpdate();
    return true;
#else
    Q_UNUSED(window); Q_UNUSED(jobPath);
    return false;
#endif
}
void installDownload(MainWindow *window, const UpdateInfo &info, const QString &path) {
    auto progress = new QProgressDialog(window->localization()->text("updatePreparing"), {}, 0, 0, window);
    progress->setWindowModality(Qt::ApplicationModal);
    progress->setCancelButton(nullptr);
    progress->show();
    struct Prepared { QString job, error; };
    auto *watcher = new QFutureWatcher<Prepared>(window);
    QObject::connect(watcher, &QFutureWatcher<Prepared>::finished, window, [window, watcher, progress] {
        progress->close(); progress->deleteLater();
        const auto result = watcher->result();
        watcher->deleteLater();
        if (!result.error.isEmpty()) { window->showError("updateFailure", result.error); return; }
        try {
            if (!launchInstaller(window, result.job)) discardPreparedUpdate(result.job);
        } catch (const std::exception &e) {
            window->showError("updateFailure", QString::fromUtf8(e.what()));
        }
    });
    const auto installation = QCoreApplication::applicationDirPath();
    watcher->setFuture(QtConcurrent::run([info, path, installation] {
        Prepared result;
        try { result.job = prepareUpdateArchive(path, info.size, info.sha256, installation); }
        catch (const std::exception &e) { result.error = QString::fromUtf8(e.what()); }
        return result;
    }));
}
void download(MainWindow *window, const UpdateInfo &info) {
    auto t = [window](const QString &key) { return window->localization()->text(key); };
    const auto path = QFileDialog::getSaveFileName(
        window, t("downloadUpdate"),
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) + "/" + info.filename, "ZIP (*.zip)");
    if (path.isEmpty())
        return;
    auto file = std::make_shared<QSaveFile>(path);
    if (!file->open(QIODevice::WriteOnly)) {
        window->showError("fileError", file->errorString());
        return;
    }
    auto hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
    auto count = std::make_shared<qint64>(0);
    auto failure = std::make_shared<QString>();
    auto manager = new QNetworkAccessManager(window);
    auto reply = manager->get(request(info.download));
    auto progress = new QProgressDialog(t("downloadUpdate"), t("cancel"), 0, 100, window);
    progress->setAttribute(Qt::WA_DeleteOnClose);
    progress->setAutoClose(false);
    progress->setWindowModality(Qt::WindowModal);
    const QPointer<QProgressDialog> dialog(progress);
    QObject::connect(progress, &QProgressDialog::canceled, reply, &QNetworkReply::abort);
    QObject::connect(reply, &QNetworkReply::downloadProgress, progress, [progress](qint64 received, qint64 total) {
        if (total > 0)
            progress->setValue(int(received * 100 / total));
    });
    QObject::connect(reply, &QIODevice::readyRead, reply, [reply, file, hash, count, failure, info] {
        const auto bytes = reply->readAll();
        *count += bytes.size();
        if (*count > info.size || file->write(bytes) != bytes.size()) {
            *failure = *count > info.size ? QStringLiteral("updateIntegrity") : file->errorString();
            reply->abort();
            return;
        }
        hash->addData(bytes);
    });
    QObject::connect(
        reply, &QNetworkReply::finished, window,
        [window, reply, manager, dialog, file, hash, count, failure, info, path] {
            if (dialog)
                dialog->close();
            const auto error = reply->error();
            const auto detail =
                failure->isEmpty()
                    ? reply->errorString()
                    : (*failure == "updateIntegrity" ? window->localization()->text("updateIntegrity") : *failure);
            const bool valid =
                error == QNetworkReply::NoError && *count == info.size && hash->result().toHex() == info.sha256;
            if (valid && file->commit()) {
                QMessageBox question(QMessageBox::Information, window->localization()->text("downloadUpdate"),
                    window->localization()->text("updateReadyToInstall").arg(path), QMessageBox::NoButton, window);
                auto *install = question.addButton(window->localization()->text("installAndRestart"), QMessageBox::AcceptRole);
                question.addButton(window->localization()->text("installLater"), QMessageBox::RejectRole);
                question.exec();
                if (question.clickedButton() == install) installDownload(window, info, path);
            } else {
                file->cancelWriting();
                if (error != QNetworkReply::OperationCanceledError || !failure->isEmpty())
                    window->showError("updateFailure", error == QNetworkReply::NoError
                                                           ? window->localization()->text("updateIntegrity")
                                                           : detail);
            }
            manager->deleteLater();
        });
    progress->show();
}
} // namespace
std::optional<UpdateInfo> findUpdate(const QByteArray &json, const QString &currentVersion) {
    const auto current = version(currentVersion);
    if (!current)
        return {};
    const auto doc = QJsonDocument::fromJson(json);
    if (!doc.isArray())
        return {};
    Version newest = *current;
    std::optional<UpdateInfo> result;
    for (const auto &item : doc.array()) {
        const auto release = item.toObject();
        if (release["draft"].toBool())
            continue;
        const auto tag = release["tag_name"].toString();
        if (!tag.startsWith("windows-v"))
            continue;
        const auto number = tag.mid(9);
        const auto parsed = version(number);
        if (!parsed || *parsed <= newest ||
            ((*current)[3] == 1 && ((*parsed)[3] == 0 || release["prerelease"].toBool())))
            continue;
        const auto expected = "Compositor-" + number + "-win-x64.zip";
        for (const auto &assetValue : release["assets"].toArray()) {
            const auto asset = assetValue.toObject();
            if (asset["name"].toString() != expected)
                continue;
            const QUrl url(asset["browser_download_url"].toString());
            const auto path = "/Hoshino-Rubi/Compositor-Windows/releases/download/" + tag + "/" + expected;
            const auto digest = asset["digest"].toString();
            static const QRegularExpression sha("^sha256:[0-9a-fA-F]{64}$");
            const auto size = asset["size"].toInteger();
            if (url.scheme() != "https" || url.host() != "github.com" || url.path() != path || url.port() != -1 ||
                !url.userInfo().isEmpty() || !url.query().isEmpty() || !url.fragment().isEmpty() ||
                !sha.match(digest).hasMatch() || size <= 0 || size > 256 * 1024 * 1024)
                continue;
            result = UpdateInfo{number, expected, url, digest.mid(7).toLatin1().toLower(), size};
            newest = *parsed;
            break;
        }
    }
    return result;
}
void checkForUpdates(MainWindow *window, bool interactive) {
    if (window->property("updateCheckRunning").toBool())
        return;
    window->setProperty("updateCheckRunning", true);
    auto manager = new QNetworkAccessManager(window);
    auto reply = manager->get(
        request(QUrl("https://api.github.com/repos/Hoshino-Rubi/Compositor-Windows/releases?per_page=100")));
    auto bytes = std::make_shared<QByteArray>();
    QObject::connect(reply, &QIODevice::readyRead, reply, [reply, bytes] {
        bytes->append(reply->readAll());
        if (bytes->size() > 4 * 1024 * 1024)
            reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::finished, window, [window, reply, manager, bytes, interactive] {
        window->setProperty("updateCheckRunning", false);
        auto t = [window](const QString &key) { return window->localization()->text(key); };
        if (reply->error() != QNetworkReply::NoError || !QJsonDocument::fromJson(*bytes).isArray()) {
            if (interactive) {
                const auto message = QJsonDocument::fromJson(*bytes).object()["message"].toString();
                window->showError("updateFailure", message.isEmpty() ? reply->errorString() : message.left(512));
            }
        } else if (const auto info = findUpdate(*bytes, QCoreApplication::applicationVersion())) {
            if (QMessageBox::question(window, t("checkUpdates"),
                                      t("updateAvailable").arg(info->version) + "\n\n" + t("updateManualNote"),
                                      QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
                download(window, *info);
        } else if (interactive)
            QMessageBox::information(window, t("checkUpdates"), t("updateCurrent"));
        manager->deleteLater();
    });
}
} // namespace comp
