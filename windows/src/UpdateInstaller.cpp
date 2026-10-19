#include "UpdateInstaller.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>
#include <QSet>
#include <algorithm>
#include <QtEndian>
#include <zlib.h>
#include <stdexcept>
namespace comp {
namespace {
[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toUtf8().constData()); }
void check(bool ok, const QString &message) { if (!ok) fail(message); }
QString absolute(const QString &path) { return QDir::cleanPath(QFileInfo(path).absoluteFilePath()); }
void noLinks(const QString &path) {
    QFileInfo info(absolute(path));
    while (true) {
        check(!info.isSymLink() && !info.isJunction(), "Linked update paths are not supported.");
        const auto parent = info.absolutePath();
        if (info.absoluteFilePath() == parent) break;
        info = QFileInfo(parent);
    }
}
QString relative(QString path) {
    path.replace('\\', '/');
    check(!path.isEmpty() && !path.startsWith('/') && path.size() <= 220, "Invalid archive path.");
    const auto parts = path.split('/');
    static const QRegularExpression reserved("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)",
                                              QRegularExpression::CaseInsensitiveOption);
    for (const auto &part : parts) {
        check(!part.isEmpty() && part != "." && part != ".." && !part.endsWith(' ') && !part.endsWith('.') &&
              !reserved.match(part).hasMatch(), "Invalid archive path.");
        for (QChar c : part)
            check(c.unicode() >= 32 && !QString(":*?\"<>|").contains(c), "Invalid archive path.");
    }
    return parts.join('/');
}
QByteArray read(const QString &path, qint64 limit) {
    noLinks(path);
    QFile file(path);
    check(file.open(QIODevice::ReadOnly) && file.size() <= limit, "Cannot read update file: " + path);
    return file.readAll();
}
void write(const QString &path, const QByteArray &bytes) {
    check(QDir().mkpath(QFileInfo(path).absolutePath()), "Cannot create update directory.");
    QSaveFile file(path);
    check(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit(),
          "Cannot write update file: " + path);
}
QByteArray digest(const QString &path) {
    noLinks(path);
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "Cannot read update file: " + path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    check(hash.addData(&file), "Cannot hash update file.");
    return hash.result().toHex();
}
quint16 u16(const QByteArray &bytes, qsizetype at) {
    check(at >= 0 && at + 2 <= bytes.size(), "Truncated ZIP.");
    return qFromLittleEndian<quint16>(bytes.constData() + at);
}
quint32 u32(const QByteArray &bytes, qsizetype at) {
    check(at >= 0 && at + 4 <= bytes.size(), "Truncated ZIP.");
    return qFromLittleEndian<quint32>(bytes.constData() + at);
}
struct Entry { QString path; quint32 crc, packed, size, offset; quint16 method, flags; QByteArray name; };
QVector<Entry> entries(const QByteArray &zip) {
    qsizetype end = -1;
    for (qsizetype p = zip.size() - 22; p >= qMax<qsizetype>(0, zip.size() - 65557); --p)
        if (u32(zip, p) == 0x06054b50 && p + 22 + u16(zip, p + 20) == zip.size()) { end = p; break; }
    check(end >= 0, "Missing ZIP directory.");
    check(u16(zip, end + 4) == 0 && u16(zip, end + 6) == 0 &&
          u16(zip, end + 8) == u16(zip, end + 10), "Split ZIP is not supported.");
    const auto count = u16(zip, end + 10);
    check(count > 0 && count < 10000, "Invalid ZIP file count.");
    qsizetype p = u32(zip, end + 16), stop = p + u32(zip, end + 12);
    check(stop == end, "Invalid ZIP directory.");
    QVector<Entry> result;
    QSet<QString> names;
    quint64 total = 0;
    for (int i = 0; i < count; ++i) {
        check(p + 46 <= stop && u32(zip, p) == 0x02014b50, "Invalid ZIP entry.");
        const auto flags = u16(zip, p + 8), method = u16(zip, p + 10);
        const auto n = u16(zip, p + 28), extra = u16(zip, p + 30), comment = u16(zip, p + 32);
        check((flags & ~quint16(0x080e)) == 0 && (method == 0 || method == 8) &&
              u16(zip, p + 34) == 0 && p + 46 + n + extra + comment <= stop, "Unsupported ZIP entry.");
        check(((u32(zip, p + 38) >> 16) & 0170000) != 0120000, "Linked ZIP entries are not supported.");
        const auto name = zip.mid(p + 46, n);
        QString path = QString::fromUtf8(name);
        check(!path.contains(QChar::ReplacementCharacter), "Invalid ZIP filename.");
        const bool directory = path.endsWith('/') || path.endsWith('\\');
        if (directory) path.chop(1);
        path = relative(path);
        check(!names.contains(path.toCaseFolded()), "Duplicate ZIP filename.");
        names.insert(path.toCaseFolded());
        Entry e{path, u32(zip, p + 16), u32(zip, p + 20), u32(zip, p + 24),
                u32(zip, p + 42), method, flags, name};
        check(e.size <= 128 * 1024 * 1024 && (total += e.size) <= 1024ull * 1024 * 1024, "Update is too large.");
        if (!directory) result.append(e);
        p += 46 + n + extra + comment;
    }
    check(p == stop && !result.isEmpty(), "Invalid ZIP directory size.");
    return result;
}
QByteArray unpack(const QByteArray &zip, const Entry &e) {
    const qsizetype at = e.offset;
    check(u32(zip, at) == 0x04034b50 && u16(zip, at + 6) == e.flags &&
          u16(zip, at + 8) == e.method, "Invalid ZIP local entry.");
    const auto name = u16(zip, at + 26), extra = u16(zip, at + 28);
    const qsizetype start = at + 30 + name + extra;
    check(zip.mid(at + 30, name) == e.name && start + e.packed <= zip.size(), "Truncated ZIP entry.");
    QByteArray out;
    if (e.method == 0) {
        check(e.size == e.packed, "Invalid stored ZIP entry.");
        out = zip.mid(start, e.size);
    } else {
        out.resize(qMax<quint32>(e.size, 1));
        z_stream stream{};
        stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(zip.constData() + start));
        stream.avail_in = e.packed;
        stream.next_out = reinterpret_cast<Bytef *>(out.data());
        stream.avail_out = out.size();
        check(inflateInit2(&stream, -MAX_WBITS) == Z_OK, "ZIP inflater failed.");
        const int status = inflate(&stream, Z_FINISH);
        const auto length = stream.total_out, consumed = stream.total_in;
        inflateEnd(&stream);
        check(status == Z_STREAM_END && length == e.size && consumed == e.packed, "Invalid deflated ZIP entry.");
        out.resize(e.size);
    }
    check(crc32(0, reinterpret_cast<const Bytef *>(out.constData()), uInt(out.size())) == e.crc, "ZIP checksum mismatch.");
    return out;
}
QString workDirectory(const QString &jobPath, const QJsonObject &job) {
    const auto work = QFileInfo(jobPath).absolutePath(), target = absolute(job.value("installation").toString());
    const QFileInfo info(work);
    check(info.fileName().startsWith(".compositor-update-") &&
          info.absolutePath() == QFileInfo(target).absolutePath() &&
          QFileInfo(jobPath).fileName() == "job.json" && job.value("version") == 1,
          "Invalid update job location.");
    noLinks(work); noLinks(target);
    return work;
}
}
QString prepareUpdateArchive(const QString &archive, qint64 expectedSize, const QByteArray &sha256,
                             const QString &installation) {
    const auto target = absolute(installation);
    noLinks(target);
    check(QFileInfo(target + "/Compositor.exe").isFile(), "Missing installed application.");
    const auto zip = read(archive, 256 * 1024 * 1024);
    check(zip.size() == expectedSize && QCryptographicHash::hash(zip, QCryptographicHash::Sha256).toHex() == sha256,
          "Update archive checksum mismatch.");
    auto files = entries(zip);
    QString prefix;
    if (files.first().path.contains('/')) prefix = files.first().path.section('/', 0, 0) + "/";
    const bool direct = std::any_of(files.cbegin(), files.cend(), [](const auto &e) { return e.path == "Compositor.exe"; });
    if (direct) prefix.clear();
    const auto work = QFileInfo(target).absolutePath() + "/.compositor-update-" +
                      QUuid::createUuid().toString(QUuid::WithoutBraces);
    check(QDir().mkpath(work + "/payload"), "The application directory is not writable.");
    try {
        QJsonArray manifest;
        QSet<QString> found;
        for (auto &entry : files) {
            check(entry.path.startsWith(prefix), "ZIP has multiple package roots.");
            const auto path = relative(entry.path.mid(prefix.size()));
            const auto bytes = unpack(zip, entry);
            write(work + "/payload/" + path, bytes);
            manifest.append(QJsonObject{{"path", path},
                {"sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}});
            found.insert(path);
        }
        for (const auto &required : {"Compositor.exe", "CompositorUpdater.exe", "Qt6Core.dll", "platforms/qwindows.dll"})
            check(found.contains(required), "Incomplete update package.");
        // The currently installed helper runs outside the directory it will replace.
        for (const auto &name : {"CompositorUpdater.exe", "Qt6Core.dll", "z.dll",
                                "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll"}) {
            const QString path = target + "/" + name;
            check(QFileInfo(path).isFile(), "Missing updater runtime: " + QString(name));
            write(work + "/control/" + name, read(path, 128 * 1024 * 1024));
        }
        // Extra CRT libraries shipped by windeployqt can be dependencies of QtCore.
        for (const auto &info : QDir(target).entryInfoList({"msvcp140_*.dll", "vcruntime140_*.dll", "concrt140.dll"}, QDir::Files))
            if (!QFileInfo::exists(work + "/control/" + info.fileName()))
                write(work + "/control/" + info.fileName(), read(info.absoluteFilePath(), 16 * 1024 * 1024));
        const QJsonObject job{{"version", 1}, {"installation", target}, {"files", manifest}};
        write(work + "/job.json", QJsonDocument(job).toJson());
        validateUpdateJob(work + "/job.json");
        return work + "/job.json";
    } catch (...) {
        QDir(work).removeRecursively();
        throw;
    }
}
QJsonObject readUpdateJob(const QString &jobPath) {
    const auto doc = QJsonDocument::fromJson(read(jobPath, 4 * 1024 * 1024));
    check(doc.isObject(), "Invalid update job.");
    return doc.object();
}
void validateUpdateJob(const QString &jobPath) {
    const auto job = readUpdateJob(jobPath);
    const auto work = workDirectory(jobPath, job), target = job.value("installation").toString();
    const auto files = job.value("files").toArray();
    check(!files.isEmpty() && files.size() < 10000, "Invalid update manifest.");
    QSet<QString> found;
    for (const auto &value : files) {
        const auto file = value.toObject();
        const auto path = relative(file.value("path").toString());
        check(!found.contains(path.toCaseFolded()), "Duplicate update filename.");
        found.insert(path.toCaseFolded());
        noLinks(target + "/" + path);
        check(!QFileInfo(target + "/" + path).isDir(), "A directory conflicts with an update file.");
        check(digest(work + "/payload/" + path) == file.value("sha256").toString().toLatin1(), "Staged update checksum mismatch.");
    }
    check(found.contains("compositor.exe") && found.contains("compositorupdater.exe"), "Missing update executables.");
}
bool rollbackUpdateJob(const QString &jobPath, QString &error) {
    try {
        const auto job = readUpdateJob(jobPath);
        const auto work = workDirectory(jobPath, job), target = job.value("installation").toString();
        const auto journal = QJsonDocument::fromJson(read(work + "/transaction.json", 4 * 1024 * 1024)).array();
        for (auto i = journal.size(); i > 0; --i) {
            const auto record = journal[i - 1].toObject();
            const auto path = relative(record.value("path").toString());
            const auto dest = target + "/" + path, backup = work + "/backup/" + path;
            noLinks(dest); noLinks(backup);
            if (QFileInfo::exists(backup)) {
                check(!QFileInfo::exists(dest) || QFile::remove(dest), "Cannot remove failed update file: " + path);
                check(QFile::rename(backup, dest), "Cannot restore original file: " + path);
            } else if (!record.value("existed").toBool())
                check(!QFileInfo::exists(dest) || QFile::remove(dest), "Cannot remove new update file: " + path);
        }
        return true;
    } catch (const std::exception &e) { error = QString::fromUtf8(e.what()); return false; }
}
bool applyUpdateJob(const QString &jobPath, QString &error) {
    bool started = false;
    try {
        validateUpdateJob(jobPath);
        const auto job = readUpdateJob(jobPath);
        const auto work = workDirectory(jobPath, job), target = job.value("installation").toString();
        check(!QFileInfo::exists(work + "/transaction.json"), "This update job has already been attempted.");
        QJsonArray journal;
        for (const auto &value : job.value("files").toArray()) {
            const auto path = relative(value.toObject().value("path").toString());
            const auto dest = target + "/" + path, backup = work + "/backup/" + path;
            const bool existed = QFileInfo::exists(dest);
            journal.append(QJsonObject{{"path", path}, {"existed", existed}});
            write(work + "/transaction.json", QJsonDocument(journal).toJson());
            started = true;
            check(QDir().mkpath(QFileInfo(backup).absolutePath()) && QDir().mkpath(QFileInfo(dest).absolutePath()),
                  "Cannot prepare update paths.");
            if (existed) check(QFile::rename(dest, backup), "Cannot replace a locked application file: " + path);
            check(QFile::copy(work + "/payload/" + path, dest), "Cannot install application file: " + path);
        }
        return true;
    } catch (const std::exception &e) {
        error = QString::fromUtf8(e.what());
        if (started) {
            QString rollback;
            if (!rollbackUpdateJob(jobPath, rollback)) error += "\nRollback: " + rollback;
        }
        return false;
    }
}
void discardPreparedUpdate(const QString &jobPath) {
    const auto job = readUpdateJob(jobPath);
    const auto work = workDirectory(jobPath, job);
    check(!QFileInfo::exists(work + "/transaction.json"), "Cannot discard an attempted update.");
    check(QDir(work).removeRecursively(), "Cannot remove prepared update.");
}
}
