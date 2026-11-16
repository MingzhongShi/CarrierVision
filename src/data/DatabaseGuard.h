#pragma once

#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QVariantMap>

// 数据库完整性检查、在线备份与损坏恢复。
namespace DatabaseGuard {

// PRAGMA quick_check；不通过时 detail 给出前几条问题
bool quickCheck(QSqlDatabase &db, QString &detail);

// 用 VACUUM INTO 在 backupDir 下生成 dataAgc-yyyyMMdd-HHmmss.db，只保留最近 keep 份
bool backup(QSqlDatabase &db, const QString &backupDir, int keep, QString &error, QString *createdPath = nullptr);

// backupDir 中的备份，按时间从新到旧
QStringList backups(const QString &backupDir);

// 在打开主连接之前调用：检查 dbPath，损坏时把原库（含 -wal/-shm）改名保留，
// 再用最近一份通过检查的备份恢复；没有可用备份时让程序以新库启动。
// 返回给用户的说明；数据库正常时返回空串。
QString recoverIfCorrupt(const QString &dbPath, const QString &backupDir);

// 运行标记：启动时创建、正常退出时删除；启动时仍存在说明上次异常退出
QString runningMarkerPath(const QString &dbPath);
// 日常检查发现损坏时创建，下次启动强制检查并恢复
QString corruptMarkerPath(const QString &dbPath);

// ---- 空间回收与监控 ----

struct SpaceInfo {
    qint64 pageSize = 0;
    qint64 pageCount = 0;
    qint64 freePages = 0;
    int autoVacuum = 0; // 0 关闭，1 完全，2 增量
};
bool spaceInfo(QSqlDatabase &db, SpaceInfo &info, QString &error);

// 启动时、其他连接打开之前调用。新库设为增量回收（auto_vacuum=INCREMENTAL）；
// 已有数据的旧库需要一次完整 VACUUM 才能切换，只在库文件不超过 maxConvertBytes 时执行（耗时与库大小成正比）。
// detail 说明执行了什么（无动作时为空）。
bool enableIncrementalVacuum(QSqlDatabase &db, qint64 maxConvertBytes, QString &detail);

struct CompactResult {
    bool checkpointed = false;   // WAL 已全部写回并截断
    qint64 walBytesBefore = 0;
    qint64 walBytesAfter = 0;
    qint64 freePagesBefore = 0;
    qint64 freePagesAfter = 0;
    qint64 fileBytesBefore = 0;
    qint64 fileBytesAfter = 0;
};
// 每日维护：wal_checkpoint(TRUNCATE) 把 WAL 写回主库并截断；增量回收模式下分步 incremental_vacuum
// 释放空闲页（每步一个短写事务，步间让出写锁，不长时间阻塞入库）。
bool compact(QSqlDatabase &db, const QString &dbPath, CompactResult &result, QString &error,
             int pagesPerStep = 2048, int maxSteps = 512);

// 数据库监控：文件/WAL 大小、页与空闲页、记录数与时间范围、CSV 归档大小
QVariantMap databaseInfo(QSqlDatabase &db, const QString &dbPath, const QString &csvArchiveDir);

} // namespace DatabaseGuard
