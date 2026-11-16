#include "data/DatabaseGuard.h"

#include "common/AppLogger.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QDirIterator>
#include <QUuid>
#include <QElapsedTimer>

namespace DatabaseGuard {

bool quickCheck(QSqlDatabase &db, QString &detail)
{
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("PRAGMA quick_check(5)"))) {
        detail = q.lastError().text();
        return false;
    }
    QStringList problems;
    while (q.next()) problems << q.value(0).toString();
    if (problems == QStringList{QStringLiteral("ok")}) return true;
    detail = problems.join(QStringLiteral("; ")).left(512);
    return false;
}

QStringList backups(const QString &backupDir)
{
    QDir dir(backupDir);
    QStringList result;
    // 文件名中的时间戳按字典序即时间序
    const QStringList names = dir.entryList({QStringLiteral("dataAgc-*.db")}, QDir::Files, QDir::Name | QDir::Reversed);
    for (const QString &name : names) result << dir.filePath(name);
    return result;
}

bool backup(QSqlDatabase &db, const QString &backupDir, int keep, QString &error, QString *createdPath)
{
    if (!QDir().mkpath(backupDir)) {
        error = QStringLiteral("无法创建备份目录 %1").arg(backupDir);
        return false;
    }
    const QString target = QDir(backupDir).filePath(
        QStringLiteral("dataAgc-%1.db").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));
    // 先写临时文件，完成后改名，避免恢复时选中写了一半的备份
    const QString temp = target + QStringLiteral(".partial");
    QFile::remove(temp);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("VACUUM INTO ?"));
    q.addBindValue(temp);
    if (!q.exec()) {
        error = q.lastError().text();
        QFile::remove(temp);
        return false;
    }
    QFile::remove(target);
    if (!QFile::rename(temp, target)) {
        error = QStringLiteral("备份改名失败: %1").arg(target);
        QFile::remove(temp);
        return false;
    }
    if (createdPath) *createdPath = target;
    const QStringList all = backups(backupDir);
    for (int i = qMax(1, keep); i < all.size(); ++i) QFile::remove(all.at(i));
    return true;
}

namespace {

bool checkFile(const QString &path, QString &detail)
{
    const QString name = QStringLiteral("integrity_%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        if (!db.open()) {
            detail = db.lastError().text();
        } else {
            ok = quickCheck(db, detail);
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(name);
    return ok;
}

} // namespace

QString recoverIfCorrupt(const QString &dbPath, const QString &backupDir)
{
    if (!QFileInfo::exists(dbPath)) return QString();
    QString detail;
    if (checkFile(dbPath, detail)) return QString();

    LOG_CRITICAL("[DB] 数据库完整性检查失败: {} ({})", dbPath.toStdString(), detail.toStdString());
    const QString suffix = QStringLiteral(".corrupt-%1").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    for (const QString &ext : {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")}) {
        if (QFileInfo::exists(dbPath + ext) && !QFile::rename(dbPath + ext, dbPath + suffix + ext)) {
            return QStringLiteral("数据库已损坏，且无法改名保留原文件 %1，请人工处理").arg(dbPath + ext);
        }
    }

    for (const QString &candidate : backups(backupDir)) {
        QString backupDetail;
        if (!checkFile(candidate, backupDetail)) {
            LOG_ERROR("[DB] 备份不可用，跳过: {} ({})", candidate.toStdString(), backupDetail.toStdString());
            continue;
        }
        if (QFile::copy(candidate, dbPath)) {
            const QString message = QStringLiteral("数据库已损坏，已从备份 %1 恢复；原文件保留为 %2。备份之后写入的数据已丢失。")
                                        .arg(QFileInfo(candidate).fileName(), QFileInfo(dbPath + suffix).fileName());
            LOG_CRITICAL("[DB] {}", message.toStdString());
            return message;
        }
    }
    const QString message = QStringLiteral("数据库已损坏且没有可用备份，已以新数据库启动；原文件保留为 %1。")
                                .arg(QFileInfo(dbPath + suffix).fileName());
    LOG_CRITICAL("[DB] {}", message.toStdString());
    return message;
}

QString runningMarkerPath(const QString &dbPath)
{
    return QFileInfo(dbPath).absolutePath() + QStringLiteral("/.running");
}

QString corruptMarkerPath(const QString &dbPath)
{
    return QFileInfo(dbPath).absolutePath() + QStringLiteral("/.db-check-required");
}

namespace {

bool pragmaInt(QSqlDatabase &db, const QString &name, qint64 &value, QString &error)
{
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("PRAGMA ") + name) || !q.next()) {
        error = QStringLiteral("PRAGMA %1 失败: %2").arg(name, q.lastError().text());
        return false;
    }
    value = q.value(0).toLongLong();
    return true;
}

qint64 fileSize(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() ? info.size() : 0;
}

} // namespace

bool spaceInfo(QSqlDatabase &db, SpaceInfo &info, QString &error)
{
    qint64 autoVacuum = 0;
    if (!pragmaInt(db, QStringLiteral("page_size"), info.pageSize, error)
        || !pragmaInt(db, QStringLiteral("page_count"), info.pageCount, error)
        || !pragmaInt(db, QStringLiteral("freelist_count"), info.freePages, error)
        || !pragmaInt(db, QStringLiteral("auto_vacuum"), autoVacuum, error))
        return false;
    info.autoVacuum = static_cast<int>(autoVacuum);
    return true;
}

bool enableIncrementalVacuum(QSqlDatabase &db, qint64 maxConvertBytes, QString &detail)
{
    detail.clear();
    SpaceInfo info;
    QString error;
    if (!spaceInfo(db, info, error)) { detail = error; return false; }
    if (info.autoVacuum == 2) return true;

    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("PRAGMA auto_vacuum = INCREMENTAL"))) {
        detail = q.lastError().text();
        return false;
    }
    q.finish();
    // 全新的空库设置后立即生效；已初始化过的库（包括已切到 WAL 的空库）需要 VACUUM 才能切换
    {
        SpaceInfo now;
        if (spaceInfo(db, now, error) && now.autoVacuum == 2) return true;
    }

    const qint64 bytes = info.pageCount * info.pageSize;
    if (bytes > maxConvertBytes) {
        detail = QStringLiteral("数据库 %1 MB 超过启动时自动转换上限 %2 MB，暂不切换增量回收；删除后的空间由新数据复用")
                     .arg(bytes / (1024 * 1024)).arg(maxConvertBytes / (1024 * 1024));
        return true;
    }
    QElapsedTimer timer;
    timer.start();
    if (!q.exec(QStringLiteral("VACUUM"))) {
        detail = QStringLiteral("切换增量回收的 VACUUM 失败: %1").arg(q.lastError().text());
        return false;
    }
    SpaceInfo after;
    spaceInfo(db, after, error);
    detail = QStringLiteral("已切换为增量空间回收：VACUUM 用时 %1 ms，库 %2 MB -> %3 MB")
                 .arg(timer.elapsed())
                 .arg(bytes / (1024 * 1024))
                 .arg(after.pageCount * after.pageSize / (1024 * 1024));
    return after.autoVacuum == 2;
}

bool compact(QSqlDatabase &db, const QString &dbPath, CompactResult &result, QString &error,
             int pagesPerStep, int maxSteps)
{
    result = {};
    result.walBytesBefore = fileSize(dbPath + QStringLiteral("-wal"));
    result.fileBytesBefore = fileSize(dbPath);
    SpaceInfo info;
    if (!spaceInfo(db, info, error)) return false;
    result.freePagesBefore = info.freePages;

    if (info.autoVacuum == 2) {
        for (int step = 0; step < maxSteps && info.freePages > 0; ++step) {
            QSqlQuery q(db);
            if (!q.exec(QStringLiteral("PRAGMA incremental_vacuum(%1)").arg(qMax(1, pagesPerStep)))) {
                error = QStringLiteral("incremental_vacuum 失败: %1").arg(q.lastError().text());
                return false;
            }
            while (q.next()) {}
            q.finish();
            if (!spaceInfo(db, info, error)) return false;
            QThread::msleep(20); // 让出写锁，入库不必长时间等待
        }
    }
    result.freePagesAfter = info.freePages;

    // TRUNCATE 需要等读者结束；有长查询时返回 busy，退回 PASSIVE，明天再试
    QSqlQuery checkpoint(db);
    if (checkpoint.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)")) && checkpoint.next())
        result.checkpointed = checkpoint.value(0).toInt() == 0;
    checkpoint.finish();
    if (!result.checkpointed) {
        QSqlQuery passive(db);
        passive.exec(QStringLiteral("PRAGMA wal_checkpoint(PASSIVE)"));
    }
    result.walBytesAfter = fileSize(dbPath + QStringLiteral("-wal"));
    result.fileBytesAfter = fileSize(dbPath);
    return true;
}

QVariantMap databaseInfo(QSqlDatabase &db, const QString &dbPath, const QString &csvArchiveDir)
{
    QVariantMap out;
    out.insert(QStringLiteral("path"), QDir::toNativeSeparators(dbPath));
    out.insert(QStringLiteral("fileBytes"), fileSize(dbPath));
    out.insert(QStringLiteral("walBytes"), fileSize(dbPath + QStringLiteral("-wal")));

    qint64 backupBytes = 0;
    const QStringList backupFiles = backups(QFileInfo(dbPath).absolutePath() + QStringLiteral("/backup"));
    for (const QString &file : backupFiles) backupBytes += fileSize(file);
    out.insert(QStringLiteral("backupCount"), backupFiles.size());
    out.insert(QStringLiteral("backupBytes"), backupBytes);

    qint64 archiveBytes = 0;
    int archiveFiles = 0;
    QString oldestArchive;
    if (!csvArchiveDir.isEmpty() && QFileInfo(csvArchiveDir).isDir()) {
        QDirIterator it(csvArchiveDir, {QStringLiteral("record_*.csv")}, QDir::Files);
        while (it.hasNext()) {
            it.next();
            ++archiveFiles;
            archiveBytes += it.fileInfo().size();
            const QString name = it.fileName();
            if (oldestArchive.isEmpty() || name < oldestArchive) oldestArchive = name;
        }
    }
    out.insert(QStringLiteral("archiveDir"), QDir::toNativeSeparators(csvArchiveDir));
    out.insert(QStringLiteral("archiveFiles"), archiveFiles);
    out.insert(QStringLiteral("archiveBytes"), archiveBytes);
    out.insert(QStringLiteral("oldestArchiveMonth"), oldestArchive.mid(7, 7));

    if (!db.isValid() || !db.isOpen()) {
        out.insert(QStringLiteral("ok"), false);
        out.insert(QStringLiteral("error"), QStringLiteral("数据库未打开"));
        return out;
    }
    SpaceInfo info;
    QString error;
    if (!spaceInfo(db, info, error)) {
        out.insert(QStringLiteral("ok"), false);
        out.insert(QStringLiteral("error"), error);
        return out;
    }
    out.insert(QStringLiteral("pageSize"), info.pageSize);
    out.insert(QStringLiteral("pageCount"), info.pageCount);
    out.insert(QStringLiteral("freePages"), info.freePages);
    out.insert(QStringLiteral("freePercent"), info.pageCount > 0 ? info.freePages * 100.0 / info.pageCount : 0.0);
    out.insert(QStringLiteral("autoVacuum"), info.autoVacuum == 2 ? QStringLiteral("incremental")
                                             : info.autoVacuum == 1 ? QStringLiteral("full") : QStringLiteral("none"));

    // COUNT 走覆盖索引，MIN/MAX 走时间索引两端，库很大时也只需几百毫秒
    QSqlQuery q(db);
    if (q.exec(QStringLiteral("SELECT COUNT(*), MIN(createtime), MAX(createtime) FROM record")) && q.next()) {
        out.insert(QStringLiteral("recordCount"), q.value(0).toLongLong());
        out.insert(QStringLiteral("oldestRecord"), q.value(1).toString());
        out.insert(QStringLiteral("newestRecord"), q.value(2).toString());
    }
    q.finish();
    if (q.exec(QStringLiteral("SELECT COUNT(*) FROM record WHERE createtime >= datetime('now','localtime','-1 day')")) && q.next())
        out.insert(QStringLiteral("recordsLastDay"), q.value(0).toLongLong());
    q.finish();
    out.insert(QStringLiteral("ok"), true);
    return out;
}

} // namespace DatabaseGuard
