#pragma once
#include <QSqlDatabase>
#include <QStringList>
#include <QDateTime>
#include <atomic>
namespace ArchiveMaintenance {

struct CleanupStats {
    int selected = 0;        // 本轮选中的过期图片组
    int recordsDeleted = 0;  // 删除的检测记录
    int filesDeleted = 0;    // 删除的图片文件
    int skippedUnsafe = 0;   // 路径不在图像根目录内：只删记录、保留文件
    int requestsDeleted = 0; // 清除的过期 TCP 重发去重记录（ingest_requests）
    int recordsArchived = 0; // 删除前写入 CSV 归档的检测记录
};

// Deletes only DB-referenced, expired image files. Never deletes a directory.
// 记录按实际落盘路径 image_path 分组（为空时按 imagename）；路径不安全的组只删除记录，保证每轮都有进展。
bool cleanup(QSqlDatabase db, const QString &archiveRoot, const QStringList &managedRoots,
             const QDateTime &cutoff, int &removed, bool &more, QString &error,
             CleanupStats *stats = nullptr, const QString &csvArchiveDir = QString());

// csvArchiveDir 非空时，过期记录删除前按记录月份追加到 csvArchiveDir/record_yyyy-MM.csv
// （UTF-8 带 BOM，Excel 可直接打开）。列取 record 表当前全部列；表结构变化导致表头不同时
// 写入 record_yyyy-MM_2.csv 等新文件，不改动旧文件。返回某月应写入的文件路径。
QString archiveFileFor(const QString &csvArchiveDir, const QString &month, const QString &header);

// 删除图像根目录下修改时间早于 olderThan、且没有任何记录引用的图片（孤儿文件）。
// 每次最多删除 maxFiles 个；返回删除数量。不删除目录、隐藏文件与仍有 .cv-pending 标记的文件。
int sweepOrphans(QSqlDatabase db, const QStringList &managedRoots, const QDateTime &olderThan,
                 int maxFiles, QString &error);

// 旧库迁移：record.image_path 中位于 imageRoot 内的绝对路径改写为相对图像根目录的路径
// （AgcUtils::toImageRootRelative，不在根目录内的路径保持不变）。按 rowid 区间分批，每批一个短写事务，
// 断点与完成标记随改写在同一事务内写入 system_config，中断（退出、断电、出错）后下次从断点继续，不会重复或遗漏。
// stop 置位时在批次之间停止。调用方负责迁移前备份数据库（见 AppController::runScheduledTasks）。
inline constexpr const char *kImagePathMigrationDoneKey = "db/imagePathRelativeDone";
inline constexpr const char *kImagePathMigrationRowKey = "db/imagePathRelativeRowId";
struct ImagePathMigration {
    bool ok = true;
    bool done = false;     // 已处理到表尾（或此前已完成）
    qint64 converted = 0;  // 本次改写的行数
    int batches = 0;
    QString error;
};
ImagePathMigration migrateImagePathsToRelative(QSqlDatabase db, const QString &imageRoot,
                                               const std::atomic_bool *stop = nullptr,
                                               int batchRows = 5000, int pauseMs = 20);
}
