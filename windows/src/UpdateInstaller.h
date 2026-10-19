#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QString>
namespace comp {
// All work is confined to a fresh sibling of the installed portable directory.
QString prepareUpdateArchive(const QString &archive, qint64 expectedSize, const QByteArray &sha256,
                             const QString &installation);
QJsonObject readUpdateJob(const QString &jobPath);
void validateUpdateJob(const QString &jobPath);
bool applyUpdateJob(const QString &jobPath, QString &error);
bool rollbackUpdateJob(const QString &jobPath, QString &error);
void discardPreparedUpdate(const QString &jobPath);
}
