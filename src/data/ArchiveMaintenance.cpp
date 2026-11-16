#include "data/ArchiveMaintenance.h"
#include "common/AgcUtils.h"
#include "common/AppLogger.h"
#include <QDir>
#include <QDirIterator>
#include <QRegularExpression>
#include <QFileInfo>
#include <QFile>
#include <QSet>
#include <QSqlQuery>
#include <QSqlError>
#include <QSqlRecord>
#include <QHash>
#include <QStandardPaths>
#include <QCoreApplication>

namespace {
constexpr int kBatchSize = 500;

bool safeFile(const QString &path, const QStringList &roots)
{
    const QString absolute = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (!AgcUtils::isImageFile(absolute)) return false;
    for (const QString &raw : roots) {
        const QFileInfo info(raw);
        const QString root = info.canonicalFilePath();
        if (root.isEmpty() || info.isSymbolicLink() || root == QDir::rootPath()
            || root == QDir::homePath() || root == QCoreApplication::applicationDirPath()) continue;
        const QString relative = QDir(root).relativeFilePath(absolute);
        if (relative == ".." || relative.startsWith("../") || QDir::isAbsolutePath(relative)) continue;
        QString current = root;
        bool safe = true;
        for (const QString &component : relative.split('/')) {
            current = QDir(current).filePath(component);
            if (QFileInfo(current).isSymbolicLink()) { safe = false; break; }
        }
        if (safe) return true;
    }
    return false;
}

// 文件是否仍被记录引用：按实际路径，或按图片名（仅限尚未关联实际路径的记录）
bool referenced(QSqlDatabase &db, const QString &path, const QString &name, bool &ok)
{
    QSqlQuery refs(db);
    refs.prepare("SELECT 1 FROM record WHERE image_path=:path "
                 "UNION ALL SELECT 1 FROM record WHERE imagename=:name AND (image_path IS NULL OR image_path='') "
                 "LIMIT 1");
    refs.bindValue(":path", path);
    refs.bindValue(":name", name);
    ok = refs.exec();
    const bool found = ok && refs.next();
    refs.finish();
    return found;
}

QString csvField(const QVariant &value)
{
    if (value.isNull()) return QString();
    QString text = value.toString();
    if (text.contains(u',') || text.contains(u'"') || text.contains(u'\n') || text.contains(u'\r')) {
        text.replace(QStringLiteral("\""), QStringLiteral("\"\""));
        text = u'"' + text + u'"';
    }
    return text;
}

// 待删除记录按月份追加到 CSV；删除事务提交前 flush，写入失败则回滚删除，保证不丢数据
class RecordCsvArchive {
public:
    explicit RecordCsvArchive(const QString &dir) : m_dir(dir) {}
    ~RecordCsvArchive() { qDeleteAll(m_files); }
    RecordCsvArchive(const RecordCsvArchive &) = delete;
    RecordCsvArchive &operator=(const RecordCsvArchive &) = delete;
    bool enabled() const { return !m_dir.isEmpty(); }

    bool append(QSqlDatabase &db, const QString &where, const QList<QPair<QString, QVariant>> &binds,
                int &rows, QString &error)
    {
        QSqlQuery q(db);
        if (!q.prepare(QStringLiteral("SELECT * FROM record WHERE ") + where + QStringLiteral(" ORDER BY createtime"))) {
            error = q.lastError().text();
            return false;
        }
        for (const auto &bind : binds) q.bindValue(bind.first, bind.second);
        if (!q.exec()) { error = q.lastError().text(); return false; }
        const QSqlRecord columns = q.record();
        const int createIndex = columns.indexOf(QStringLiteral("createtime"));
        QStringList names;
        for (int i = 0; i < columns.count(); ++i) names << columns.fieldName(i);
        const QString header = names.join(u',');
        while (q.next()) {
            QString month = q.value(createIndex).toString().left(7);
            if (month.size() != 7 || month.at(4) != u'-') month = QStringLiteral("unknown");
            QFile *file = fileFor(month, header, error);
            if (!file) return false;
            QStringList fields;
            fields.reserve(columns.count());
            for (int i = 0; i < columns.count(); ++i) fields << csvField(q.value(i));
            const QByteArray line = (fields.join(u',') + QStringLiteral("\r\n")).toUtf8();
            if (file->write(line) != line.size()) { error = file->errorString(); return false; }
            ++rows;
        }
        return true;
    }

    bool flush(QString &error)
    {
        for (QFile *file : std::as_const(m_files)) {
            if (!file->flush()) { error = file->errorString(); return false; }
        }
        return true;
    }

private:
    QFile *fileFor(const QString &month, const QString &header, QString &error)
    {
        const QString key = month + u'|' + header;
        if (QFile *open = m_files.value(key)) return open;
        if (!QDir().mkpath(m_dir)) { error = QStringLiteral("无法创建归档目录 %1").arg(m_dir); return nullptr; }
        const QString path = ArchiveMaintenance::archiveFileFor(m_dir, month, header);
        auto *file = new QFile(path);
        const bool fresh = !file->exists() || file->size() == 0;
        if (!file->open(QIODevice::WriteOnly | QIODevice::Append)) {
            error = QStringLiteral("无法写入归档文件 %1: %2").arg(path, file->errorString());
            delete file;
            return nullptr;
        }
        if (fresh) {
            const QByteArray head = QByteArray("\xEF\xBB\xBF") + (header + QStringLiteral("\r\n")).toUtf8();
            if (file->write(head) != head.size()) { error = file->errorString(); delete file; return nullptr; }
        }
        m_files.insert(key, file);
        return file;
    }

    QString m_dir;
    QHash<QString, QFile *> m_files;
};
}

QString ArchiveMaintenance::archiveFileFor(const QString &csvArchiveDir, const QString &month, const QString &header)
{
    const QDir dir(csvArchiveDir);
    for (int n = 1; n < 100; ++n) {
        const QString path = dir.filePath(n == 1 ? QStringLiteral("record_%1.csv").arg(month)
                                                 : QStringLiteral("record_%1_%2.csv").arg(month).arg(n));
        QFile file(path);
        if (!file.exists() || file.size() == 0) return path;
        if (!file.open(QIODevice::ReadOnly)) continue;
        QByteArray first = file.readLine();
        if (first.startsWith("\xEF\xBB\xBF")) first.remove(0, 3);
        if (QString::fromUtf8(first).trimmed() == header) return path;
    }
    return dir.filePath(QStringLiteral("record_%1_overflow.csv").arg(month));
}

bool ArchiveMaintenance::cleanup(QSqlDatabase db, const QString &archiveRoot,
    const QStringList &roots, const QDateTime &cutoff, int &removed, bool &more, QString &error,
    CleanupStats *stats, const QString &csvArchiveDir)
{
    removed = 0; more = false;
    CleanupStats local;
    CleanupStats &s = stats ? *stats : local;
    s = {};
    if (!cutoff.isValid() || !db.isOpen()) { error = "数据库不可用"; return false; }
    const QString cutoffText = AgcUtils::formatDateTime(cutoff);

    // 候选组的扫描（整表分组，记录越多越慢）在写事务之外执行。若放在延迟事务（BEGIN）里，
    // 扫描期间主线程只要写入一批 TCP 数据，随后的首条写语句就会因读快照过期立即返回
    // "database is locked"（busy_timeout 不生效），整轮清理中止，数据越积越多。
    // 扫描与删除之间新到的记录不受影响：删除语句只删 cutoff 之前的记录，文件删除前还会再查引用。
    QSqlQuery select(db);
    select.prepare("SELECT COALESCE(NULLIF(image_path,''), imagename) AS image_key, "
                   "MAX(image_path IS NOT NULL AND image_path<>'') FROM record GROUP BY image_key "
                   "HAVING COUNT(*)=COUNT(datetime(createtime)) AND MAX(datetime(createtime)) < datetime(:cutoff) LIMIT 500");
    select.bindValue(":cutoff", cutoffText);
    if (!select.exec()) { error = select.lastError().text(); return false; }
    QStringList keys;
    QSet<QString> pathKeys; // 来自 image_path 列（实际落盘路径）的组
    while (select.next()) {
        keys.append(select.value(0).toString());
        if (select.value(1).toInt()) pathKeys.insert(keys.last());
    }
    select.finish();
    s.selected = keys.size();

    // 路径安全检查要访问文件系统，同样放在写事务之前，缩短持有写锁的时间（主线程入库会等这把锁）
    struct Group { QString key; QString path; bool safe; };
    QList<Group> groups;
    groups.reserve(keys.size());
    for (const auto &key : keys) {
        const bool pathKey = pathKeys.contains(key);
        const QString path = QDir::cleanPath(pathKey ? key : QDir(archiveRoot).absoluteFilePath(key));
        if (QFileInfo::exists(path + ".cv-pending")) continue; // 上传提交中，下轮再处理
        groups.append({key, path, !key.isEmpty() && safeFile(path, roots)});
    }

    // BEGIN IMMEDIATE 先拿写锁（受 busy_timeout 等待），之后的写入不会再因快照过期失败
    QSqlQuery begin(db);
    if (!begin.exec(QStringLiteral("BEGIN IMMEDIATE"))) { error = begin.lastError().text(); return false; }
    begin.finish();
    auto fail = [&](const QString &reason) { error = reason; db.rollback(); return false; };

    RecordCsvArchive archive(csvArchiveDir);
    const QString groupWhere = QStringLiteral(
        "(image_path=:key OR (imagename=:key AND (image_path IS NULL OR image_path=''))) "
        "AND datetime(createtime)<datetime(:cutoff)");
    QSqlQuery queue(db);
    queue.prepare("INSERT OR IGNORE INTO cleanup_files(path,imagename) VALUES(:path,:name)");
    QSqlQuery erase(db);
    erase.prepare(QStringLiteral("DELETE FROM record WHERE ") + groupWhere);
    for (const Group &group : std::as_const(groups)) {
        if (group.safe) {
            queue.bindValue(":path", group.path); queue.bindValue(":name", group.key);
            if (!queue.exec()) return fail(queue.lastError().text());
        } else {
            // 路径不在图像根目录内（历史数据中的绝对路径、../ 或非图片名）：保留文件，但必须删除记录，
            // 否则这些组每轮都会被重新选中，累计满一批后清理将永久停滞
            if (s.skippedUnsafe < 5)
                LOG_WARN("[CLEANUP] 图片路径不在图像根目录内，仅删除记录、保留文件: '{}'", group.key.left(256).toStdString());
            ++s.skippedUnsafe;
        }
        if (archive.enabled()) {
            QString archiveError;
            if (!archive.append(db, groupWhere, {{QStringLiteral(":key"), group.key}, {QStringLiteral(":cutoff"), cutoffText}},
                                s.recordsArchived, archiveError))
                return fail(QStringLiteral("归档过期记录失败，本轮未删除: %1").arg(archiveError));
        }
        erase.bindValue(":key", group.key); erase.bindValue(":cutoff", cutoffText);
        if (!erase.exec()) return fail(erase.lastError().text());
        s.recordsDeleted += erase.numRowsAffected();
    }
    queue.finish();
    erase.finish();
    QSqlQuery alerts(db);
    alerts.prepare("DELETE FROM alertrecord WHERE datetime(createtime)<datetime(:cutoff)");
    alerts.bindValue(":cutoff", cutoffText);
    if (!alerts.exec()) return fail(alerts.lastError().text());
    // TCP 重发去重表此前从不清理，只增不减；记录过期后其 requestId 也不再需要
    QSqlQuery requests(db);
    requests.prepare("DELETE FROM ingest_requests WHERE committed_at < :cutoff");
    requests.bindValue(":cutoff", cutoffText);
    if (!requests.exec()) return fail(requests.lastError().text());
    s.requestsDeleted = requests.numRowsAffected();
    // 归档文件先落盘再提交删除：提交失败时下轮会重复归档同一批记录（可接受），反之会丢数据
    {
        QString flushError;
        if (archive.enabled() && !archive.flush(flushError))
            return fail(QStringLiteral("归档文件写入失败，本轮未删除: %1").arg(flushError));
    }
    if (!db.commit()) return fail(db.lastError().text());
    if (s.skippedUnsafe > 5)
        LOG_WARN("[CLEANUP] 本轮共 {} 组图片路径不在图像根目录内，仅删除了记录", s.skippedUnsafe);

    // The committed queue survives failures between DB commit and file removal.
    // Recheck references on every retry, including records that arrived after a crash.
    QSqlQuery pending(db);
    if (!pending.exec("SELECT path,imagename FROM cleanup_files LIMIT 500")) { error = pending.lastError().text(); return false; }
    QList<QPair<QString,QString>> files;
    while (pending.next()) files.append({pending.value(0).toString(), pending.value(1).toString()});
    pending.finish();
    bool success = true;
    QStringList finished;
    for (const auto &file : files) {
        const auto &path = file.first;
        if (!safeFile(path, roots) || QFileInfo::exists(path + ".cv-pending")) { success = false; continue; }
        bool refsOk = false;
        const bool isReferenced = referenced(db, path, file.second, refsOk);
        if (!refsOk) { success = false; continue; }
        if (!isReferenced && QFileInfo::exists(path) && !QFile::remove(path)) { success = false; continue; }
        finished << path;
        if (!isReferenced) { ++removed; ++s.filesDeleted; }
    }
    // 已处理的队列项在一个事务中移除；若此前崩溃，残留项下一轮会重新检查（文件已不存在则直接移除）
    if (!finished.isEmpty()) {
        if (!db.transaction()) {
            success = false;
        } else {
            QSqlQuery done(db);
            done.prepare("DELETE FROM cleanup_files WHERE path=:path");
            for (const QString &path : std::as_const(finished)) {
                done.bindValue(":path", path);
                if (!done.exec()) { success = false; break; }
            }
            if (!success || !db.commit()) { db.rollback(); success = false; }
        }
    }
    more = success && (keys.size() == kBatchSize || files.size() == kBatchSize);
    if (!success) error = "部分文件未删除，已保留恢复队列";
    return success;
}

int ArchiveMaintenance::sweepOrphans(QSqlDatabase db, const QStringList &managedRoots, const QDateTime &olderThan,
                                     int maxFiles, QString &error)
{
    if (!db.isOpen() || !olderThan.isValid() || maxFiles <= 0) { error = "数据库不可用"; return 0; }

    // 去重并去掉嵌套的根目录，避免同一文件被扫描两次
    QStringList roots;
    for (const QString &raw : managedRoots) {
        const QString canonical = QFileInfo(raw).canonicalFilePath();
        if (!canonical.isEmpty() && !roots.contains(canonical)) roots << canonical;
    }
    QStringList topRoots;
    for (const QString &root : roots) {
        bool nested = false;
        for (const QString &other : roots)
            if (other != root && root.startsWith(other + '/')) nested = true;
        if (!nested) topRoots << root;
    }

    // FTP 按接收日期把图片存进 yyyy-MM-dd 子目录：晚于 olderThan 当天的日期目录里不可能有超期文件，
    // 整个跳过，每天只需遍历超期日期的目录和旧版平铺存放的文件，而不是整个图像目录
    const QDate newestScannedDay = olderThan.date();
    static const QRegularExpression dayDir(QStringLiteral("^\\d{4}-\\d{2}-\\d{2}$"));
    int deleted = 0;
    for (const QString &root : std::as_const(topRoots)) {
        QStringList pendingDirs{root};
        while (!pendingDirs.isEmpty() && deleted < maxFiles) {
            const QString current = pendingDirs.takeLast();
            QDirIterator dirs(current, QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
            while (dirs.hasNext()) {
                const QString sub = dirs.next();
                const QString name = dirs.fileName();
                if (dayDir.match(name).hasMatch()) {
                    const QDate day = QDate::fromString(name, QStringLiteral("yyyy-MM-dd"));
                    if (day.isValid() && day > newestScannedDay) continue;
                }
                pendingDirs << sub;
            }
            QDirIterator it(current, QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks);
            while (it.hasNext() && deleted < maxFiles) {
                const QString path = QDir::cleanPath(it.next());
                const QFileInfo info = it.fileInfo();
                if (info.fileName().startsWith('.') || !AgcUtils::isImageFile(path)) continue;
                if (info.lastModified() >= olderThan || QFileInfo::exists(path + ".cv-pending")) continue;
                if (!safeFile(path, {root})) continue;
                // 按实际路径或文件名任一被引用都保留（文件名匹配比清理更保守，覆盖未关联路径的历史记录）
                QSqlQuery refs(db);
                refs.prepare("SELECT 1 FROM record WHERE image_path=:path UNION ALL "
                             "SELECT 1 FROM record WHERE imagename=:name LIMIT 1");
                refs.bindValue(":path", path);
                refs.bindValue(":name", info.fileName());
                if (!refs.exec()) { error = refs.lastError().text(); return deleted; }
                const bool used = refs.next();
                refs.finish();
                if (used) continue;
                if (QFile::remove(path)) {
                    ++deleted;
                    LOG_INFO("[CLEANUP] 删除超期未被引用的孤儿图片: {}", path.toStdString());
                }
            }
        }
    }
    return deleted;
}
