#include "UpdateInstaller.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <Windows.h>
#endif
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 5 || args[1] != "--apply-update") return 2;
    const auto jobPath = args[2];
    QString error;
    try {
        comp::validateUpdateJob(jobPath);
        const auto job = comp::readUpdateJob(jobPath);
        const auto target = job.value("installation").toString();
#ifdef Q_OS_WIN
        const auto wide = [](const QString &s) { return reinterpret_cast<LPCWSTR>(s.utf16()); };
        HANDLE parent = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, args[3].toUInt());
        if (!parent) return 3;
        wchar_t parentPath[32768]{};
        DWORD length = 32768;
        const bool validParent = QueryFullProcessImageNameW(parent, 0, parentPath, &length) &&
            QDir::cleanPath(QString::fromWCharArray(parentPath)).compare(
                QDir::cleanPath(target + "/Compositor.exe"), Qt::CaseInsensitive) == 0;
        if (!validParent) { CloseHandle(parent); return 3; }
        const auto cancelName = args[4] + "-cancel";
        HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, wide(args[4]));
        HANDLE cancel = OpenEventW(SYNCHRONIZE, FALSE, wide(cancelName));
        if (!ready || !cancel) { if (ready) CloseHandle(ready); if (cancel) CloseHandle(cancel); CloseHandle(parent); return 3; }
        SetEvent(ready); CloseHandle(ready);
        HANDLE waits[] = {cancel, parent};
        const auto wait = WaitForMultipleObjects(2, waits, FALSE, 120000);
        CloseHandle(cancel); CloseHandle(parent);
        if (wait != WAIT_OBJECT_0 + 1) return 4;
#else
        return 3;
#endif
        const bool installed = comp::applyUpdateJob(jobPath, error);
        bool safeToRestart = installed;
        if (!installed) {
            if (!QFile::exists(QFileInfo(jobPath).absolutePath() + "/transaction.json")) safeToRestart = true;
            else {
                QString rollback;
                safeToRestart = comp::rollbackUpdateJob(jobPath, rollback);
                if (!safeToRestart) error += "\n" + rollback;
            }
        }
        QStringList reopen;
        for (const auto &path : job.value("reopen").toArray())
            if (QFileInfo(path.toString()).isDir()) reopen.append(path.toString());
        bool restarted = safeToRestart && QProcess::startDetached(target + "/Compositor.exe", reopen, target);
        if (!restarted && installed) {
            QString rollback;
            if (comp::rollbackUpdateJob(jobPath, rollback))
                restarted = QProcess::startDetached(target + "/Compositor.exe", reopen, target);
            error = "The updated application could not be started.\n" + rollback;
        }
        QFile log(QFileInfo(jobPath).absolutePath() + "/result.txt");
        if (log.open(QIODevice::WriteOnly))
            log.write((error.isEmpty() ? QString("Installed and restarted.\nBackup retained here.\n") : error).toUtf8());
        if (!error.isEmpty() || !restarted) {
#ifdef Q_OS_WIN
            const auto message = job.value("failureMessage").toString("The update could not be completed.") +
                "\n\n" + error + "\n\n" + QFileInfo(jobPath).absolutePath();
            MessageBoxW(nullptr, reinterpret_cast<LPCWSTR>(message.utf16()), L"Compositor", MB_OK | MB_ICONWARNING);
#endif
        }
        return error.isEmpty() && restarted ? 0 : 1;
    } catch (const std::exception &e) {
        QFile log(QFileInfo(jobPath).absolutePath() + "/result.txt");
        if (log.open(QIODevice::WriteOnly)) log.write(e.what());
        return 1;
    }
}
