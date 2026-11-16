#include "common/WheelIdentity.h"
#include "data/SearchService.h"

#include "common/AgcUtils.h"
#include "common/AppLogger.h"
#include "ingest/IngestCoordinator.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QSqlError>
#include <QUrl>

#include <limits>

namespace SearchService {

namespace {

// 图片查找的两级缓存，所有线程共用：
// 1. 目录可用性：根目录或日期子目录被移走、盘符或网络共享断开时，每次访问都可能很慢（网络路径可达数秒），
//    同一目录 60 秒内只向文件系统确认一次，不存在的目录直接跳过其下的所有候选文件。
// 2. 未找到的图片：历史图片目录丢失后，每次翻页、每次刷新热力图都会对同一批记录重复全套查找；
//    同一图片在同样的查找条件下 2 分钟内不再重复查找。FTP 收到同名图片时立即作废（noteImageStored）。
constexpr qint64 kDirTtlMs = 60'000;
constexpr qint64 kMissTtlMs = 120'000;
constexpr int kMaxDirEntries = 4096;
constexpr int kMaxMissEntries = 20000;

struct DirState { bool exists = false; qint64 expiresAt = 0; };
struct MissState { QString signature; qint64 expiresAt = 0; };

QMutex s_cacheMutex;
QHash<QString, DirState> s_dirs;
QHash<QString, MissState> s_misses;

QString dirKey(const QString &dir)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(dir)).toLower();
}

QString missKey(const QString &imageName)
{
    return QFileInfo(QDir::fromNativeSeparators(imageName.trimmed())).fileName().toLower();
}

bool dirAvailable(const QString &dir)
{
    if (dir.isEmpty()) return false;
    const QString key = dirKey(dir);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    {
        QMutexLocker locker(&s_cacheMutex);
        const auto it = s_dirs.constFind(key);
        if (it != s_dirs.cend() && it->expiresAt > now) return it->exists;
    }
    const bool exists = QFileInfo(dir).isDir(); // 锁外访问文件系统，慢目录不阻塞其他线程
    QMutexLocker locker(&s_cacheMutex);
    if (s_dirs.size() >= kMaxDirEntries) s_dirs.clear();
    s_dirs.insert(key, {exists, now + kDirTtlMs});
    return exists;
}

// 候选文件所在目录可用时才访问文件本身
QString existingFile(const QString &path)
{
    if (path.isEmpty()) return QString();
    const QFileInfo info(path);
    if (!dirAvailable(info.absolutePath())) return QString();
    return info.isFile() ? info.absoluteFilePath() : QString();
}

bool recentlyMissing(const QString &key, const QString &signature)
{
    if (key.isEmpty()) return false;
    QMutexLocker locker(&s_cacheMutex);
    const auto it = s_misses.constFind(key);
    return it != s_misses.cend() && it->signature == signature
        && it->expiresAt > QDateTime::currentMSecsSinceEpoch();
}

void rememberMissing(const QString &key, const QString &signature)
{
    if (key.isEmpty()) return;
    QMutexLocker locker(&s_cacheMutex);
    if (s_misses.size() >= kMaxMissEntries) s_misses.clear();
    s_misses.insert(key, {signature, QDateTime::currentMSecsSinceEpoch() + kMissTtlMs});
}

} // namespace

void noteImageStored(const QString &filePath)
{
    const QFileInfo info(filePath);
    const QString key = missKey(info.fileName());
    QMutexLocker locker(&s_cacheMutex);
    if (!key.isEmpty()) s_misses.remove(key);
    // 新建的日期子目录可能刚被记为"不存在"
    s_dirs.insert(dirKey(info.absolutePath()), {true, QDateTime::currentMSecsSinceEpoch() + kDirTtlMs});
}

void clearImageLookupCache()
{
    QMutexLocker locker(&s_cacheMutex);
    s_dirs.clear();
    s_misses.clear();
}

QString resolveImagePath(const QString &rawName, const QString &archiveDir, const QString &sourceDir)
{
    // 只在图像根目录内查找：绝对路径、盘符路径或带 .. 的名称只取文件名，不访问根目录之外的文件
    const QString normalized = QDir::fromNativeSeparators(rawName.trimmed());
    if (normalized.isEmpty()) return QString();
    const bool unsafe = QDir::isAbsolutePath(normalized)
        || (normalized.size() > 1 && normalized.at(1) == u':')
        || normalized.split(u'/').contains(QStringLiteral(".."));
    const QString trimmed = unsafe ? QFileInfo(normalized).fileName() : normalized;
    if (trimmed.isEmpty()) return QString();

    // 缓存上次命中的有效根路径（线程局部），在连续解析同一批次图像时几乎 100% 首测命中
    static thread_local QString s_lastHitRoot;

    // 剥离历史前缀提取相对子路径
    QString subPath = trimmed;
    int idx = subPath.indexOf(QStringLiteral("images/"));
    if (idx >= 0) {
        subPath = subPath.mid(idx + 7);
    } else {
        idx = subPath.indexOf(QStringLiteral("archive/"));
        if (idx >= 0) subPath = subPath.mid(idx + 8);
    }

    auto tryRoot = [&](const QString &root) -> QString {
        if (root.isEmpty() || !dirAvailable(root)) return QString();
        const QDir dir(root);
        QString hit = existingFile(dir.filePath(trimmed));
        if (hit.isEmpty() && subPath != trimmed) hit = existingFile(dir.filePath(subPath));
        if (!hit.isEmpty()) s_lastHitRoot = root;
        return hit;
    };

    // 1. 上次命中的根目录；2. 当前配置的图像目录与源目录
    QStringList tried;
    for (const QString &root : {s_lastHitRoot, archiveDir, sourceDir}) {
        if (root.isEmpty() || tried.contains(root)) continue;
        tried << root;
        const QString hit = tryRoot(root);
        if (!hit.isEmpty()) return hit;
    }

    // 3. 程序目录下的默认图像目录与旧版归档目录（未配置图像目录或配置被改动时的兜底）
    static const QString appDir = QCoreApplication::applicationDirPath();
    static const QStringList fallbackRoots = {
        QDir(appDir).filePath(QStringLiteral("images")),
        QDir(appDir).filePath(QStringLiteral("archive"))
    };
    for (const QString &root : fallbackRoots) {
        if (tried.contains(root)) continue;
        const QString hit = tryRoot(root);
        if (!hit.isEmpty()) return hit;
    }

    return QString();
}

QString resolveRecordImage(const QString &imagePath, const QString &imageName,
                           const QString &archiveDir, const QString &sourceDir,
                           const QString &createTime)
{
    // 1. 记录中的实际落盘路径：新记录为相对图像根目录的路径，旧记录为绝对路径（所在目录已不存在时不再访问文件本身）
    const QString recorded = AgcUtils::imagePathFromRecord(imagePath, archiveDir);
    if (!recorded.isEmpty()) {
        const QString stored = existingFile(recorded);
        if (!stored.isEmpty()) return stored;
    }

    const QString key = missKey(imageName);
    const QDateTime at = createTime.isEmpty() ? QDateTime() : AgcUtils::parseFlexibleDateTime(createTime, false);
    const QString signature = archiveDir + u'|' + sourceDir + u'|' + imagePath + u'|'
        + (at.isValid() ? at.date().toString(Qt::ISODate) : QString());
    if (recentlyMissing(key, signature)) return QString();

    // 2. FTP 按接收日期存入的子目录（当前默认存储方式）：按记录日期及前后一天查找
    const QString fileName = QFileInfo(QDir::fromNativeSeparators(imageName.trimmed())).fileName();
    if (!archiveDir.isEmpty() && !fileName.isEmpty() && at.isValid()) {
        for (int offset : {0, -1, 1}) {
            const QString hit = existingFile(QDir(archiveDir).filePath(
                at.date().addDays(offset).toString(QStringLiteral("yyyy-MM-dd")) + QLatin1Char('/') + fileName));
            if (!hit.isEmpty()) return hit;
        }
    }

    // 3. 按图片名在各图像根目录查找（旧版平铺存放、客户端自建子目录）
    const QString found = resolveImagePath(imageName, archiveDir, sourceDir);
    if (found.isEmpty()) rememberMissing(key, signature);
    return found;
}

QVariantList latestRoundImages(const QSqlDatabase &db, const QString &archiveDir, const QString &sourceDir)
{
    using IngestCoordinator::kStationCount;
    constexpr int kRackCount = 50;
    // 一轮最多两批、每批最多 32 轮，留余量
    constexpr int kRoundRowLimit = 128;

    struct Camera {
        bool hasRecord = false;
        int result = 1;
        QString time;
        QString imageName;
        QString imagePath;
        QStringList wheels;
        QVariantList wheelIds;
    };

    QVariantList out;
    const bool usable = db.isValid() && db.isOpen();
    QSqlQuery rows(db);
    if (usable) {
        // 走 idx_record_carrier_time 索引，每架只读最近的少量记录
        rows.prepare(QStringLiteral(
            "SELECT camera_id, wheel_id, result, createtime, imagename, image_path, distance FROM record "
            "WHERE carrier_id = :rack ORDER BY createtime DESC, rowid DESC LIMIT %1").arg(kRoundRowLimit));
    }

    for (int rack = 1; rack <= kRackCount; ++rack) {
        QVector<Camera> cameras(kStationCount);
        QString latestText;
        if (usable) {
            rows.bindValue(QStringLiteral(":rack"), rack);
            if (!rows.exec()) {
                LOG_ERROR("热力图像：查询架号 #{} 本轮记录失败: {}", rack, rows.lastError().text().toStdString());
            } else {
                QDateTime newest;
                while (rows.next()) {
                    const QString time = rows.value(3).toString();
                    const QDateTime at = AgcUtils::parseFlexibleDateTime(time, false);
                    if (latestText.isEmpty()) {
                        latestText = time;
                        newest = at;
                    }
                    if (newest.isValid() && at.isValid()
                        && at.secsTo(newest) > IngestCoordinator::kSameCarrierMergeSecs) {
                        break; // 按时间倒序，之后都是上一轮
                    }
                    const int cameraId = rows.value(0).toInt();
                    if (cameraId < 1 || cameraId > kStationCount) continue;
                    Camera &camera = cameras[cameraId - 1];
                    const QString imageName = rows.value(4).toString().trimmed();
                    if (!camera.hasRecord) {
                        camera.hasRecord = true;
                        camera.time = time;
                        camera.imageName = imageName;
                        camera.imagePath = rows.value(5).toString();
                    } else if (IngestCoordinator::imageKey(imageName) != IngestCoordinator::imageKey(camera.imageName)) {
                        continue; // 同一相机本轮较早的另一张图，不计入当前显示的图像
                    } else if (camera.imagePath.isEmpty()) {
                        camera.imagePath = rows.value(5).toString(); // 同一张图的其他轮可能已补写落盘路径
                    }
                    const int result = rows.value(2).toInt();
                    if (result == 0) camera.result = 0;
                    camera.wheelIds.prepend(rows.value(1));
                    camera.wheels.prepend(QStringLiteral("%1 %2 %3mm")
                                              .arg(WheelIdentity::label(rows.value(1)))
                                              .arg(result ? QStringLiteral("OK") : QStringLiteral("NG"))
                                              .arg(rows.value(6).toDouble(), 0, 'f', 1));
                }
                rows.finish();
            }
        }

        QVariantList images;
        int cameraCount = 0;
        int imageCount = 0;
        int ngCount = 0;
        for (int i = 0; i < kStationCount; ++i) {
            const Camera &camera = cameras[i];
            QVariantMap image;
            image.insert(QStringLiteral("cameraId"), i + 1);
            image.insert(QStringLiteral("hasRecord"), camera.hasRecord);
            image.insert(QStringLiteral("result"), camera.result);
            image.insert(QStringLiteral("time"), camera.time);
            image.insert(QStringLiteral("fileName"), QFileInfo(camera.imageName).fileName());
            image.insert(QStringLiteral("wheels"), camera.wheels);
            image.insert(QStringLiteral("wheelIds"), camera.wheelIds);
            QString path;
            if (camera.hasRecord) {
                ++cameraCount;
                if (camera.result == 0) ++ngCount;
                path = resolveRecordImage(camera.imagePath, camera.imageName, archiveDir, sourceDir, camera.time);
                if (!path.isEmpty()) ++imageCount;
            }
            image.insert(QStringLiteral("hasImage"), !path.isEmpty());
            image.insert(QStringLiteral("filePath"), path);
            image.insert(QStringLiteral("fileUrl"), path.isEmpty() ? QString() : QUrl::fromLocalFile(path).toString());
            images.append(image);
        }

        QVariantMap group;
        group.insert(QStringLiteral("rack"), rack);
        group.insert(QStringLiteral("time"), latestText);
        group.insert(QStringLiteral("status"), cameraCount == 0 ? QStringLiteral("none")
                                               : (ngCount > 0 ? QStringLiteral("ng") : QStringLiteral("ok")));
        group.insert(QStringLiteral("cameraCount"), cameraCount);
        group.insert(QStringLiteral("imageCount"), imageCount);
        group.insert(QStringLiteral("ngCount"), ngCount);
        group.insert(QStringLiteral("images"), images);
        out.append(group);
    }
    return out;
}

RecordFilter buildRecordFilter(const QString &startText, const QString &endText, int rackNumber, const QString &wheelNo)
{
    RecordFilter filter;
    filter.where << QStringLiteral("wheel_id BETWEEN 1 AND 16");
    const QDateTime startTime = AgcUtils::parseFlexibleDateTime(startText.trimmed(), false);
    const QDateTime endTime = AgcUtils::parseFlexibleDateTime(endText.trimmed(), true);
    if (startTime.isValid()) {
        filter.where << QStringLiteral("createtime >= :start");
        filter.binds.append({QStringLiteral(":start"), AgcUtils::formatDateTime(startTime)});
    }
    if (endTime.isValid()) {
        filter.where << QStringLiteral("createtime <= :end");
        filter.binds.append({QStringLiteral(":end"), AgcUtils::formatDateTime(endTime)});
    }
    if (rackNumber > 0) {
        filter.where << QStringLiteral("carrier_id = :rack");
        filter.binds.append({QStringLiteral(":rack"), rackNumber});
    }

    const QString wheelToken = wheelNo.trimmed();
    if (wheelToken == QStringLiteral("DRIVE")) {
        filter.where << QStringLiteral("wheel_id BETWEEN 9 AND 16");
    } else if (wheelToken == QStringLiteral("WALK")) {
        filter.where << QStringLiteral("wheel_id BETWEEN 1 AND 8");
    } else if (wheelToken.contains(',')) {
        QStringList wheelNumbers;
        for (const QString &part : wheelToken.split(',', Qt::SkipEmptyParts)) {
            bool ok = false;
            const int wheel = part.trimmed().toInt(&ok);
            if (ok && wheel >= 1 && wheel <= 16) wheelNumbers << QString::number(wheel);
        }
        if (!wheelNumbers.isEmpty())
            filter.where << QStringLiteral("wheel_id IN (%1)").arg(wheelNumbers.join(','));
    } else if (!wheelToken.isEmpty() && wheelToken != QStringLiteral("0")) {
        bool ok = false;
        const int wheel = wheelToken.toInt(&ok);
        if (ok && wheel >= 1 && wheel <= 16) {
            filter.where << QStringLiteral("wheel_id = :wheel");
            filter.binds.append({QStringLiteral(":wheel"), wheel});
        } else {
            filter.where << QStringLiteral("1 = 0");
        }
    }
    return filter;
}

bool queryRecordPage(const QSqlDatabase &db, const QString &table, const RecordFilter &filter,
                     int page, int pageSize, const QString &archiveDir, const QString &sourceDir,
                     int &totalCount, QVector<ImageItem> &items)
{
    QSqlQuery countQuery(db);
    if (!countQuery.prepare(QStringLiteral("SELECT COUNT(*) FROM ") + table + filter.clause())) {
        LOG_ERROR("分页查询 COUNT 预处理失败({}): {}", table.toStdString(), countQuery.lastError().text().toStdString());
        return false;
    }
    filter.bind(countQuery);
    if (!countQuery.exec()) {
        LOG_ERROR("分页查询统计失败({}): {}", table.toStdString(), countQuery.lastError().text().toStdString());
        return false;
    }
    totalCount = countQuery.next()
        ? static_cast<int>(qMin<qint64>(countQuery.value(0).toLongLong(), std::numeric_limits<int>::max()))
        : 0;

    const qint64 offset = static_cast<qint64>(page - 1) * pageSize;
    QSqlQuery dataQuery(db);
    if (!dataQuery.prepare(QStringLiteral(
            "SELECT createtime, carrier_id, wheel_id, camera_id, result, imagename, "
            "COALESCE(min_value, distance), COALESCE(max_value, distance), dist_norm, deviation, %1 FROM ")
            .arg(table == QLatin1String("record") ? QStringLiteral("image_path") : QStringLiteral("NULL"))
            + table + filter.clause()
            + QStringLiteral(" ORDER BY createtime DESC LIMIT %1 OFFSET %2").arg(pageSize).arg(offset))) {
        LOG_ERROR("分页查询数据预处理失败({}): {}", table.toStdString(), dataQuery.lastError().text().toStdString());
        return false;
    }
    filter.bind(dataQuery);
    if (!dataQuery.exec()) {
        LOG_ERROR("分页查询数据执行失败({}): {}", table.toStdString(), dataQuery.lastError().text().toStdString());
        return true; // 保持原行为：数据查询失败时仍返回总数与空页
    }
    while (dataQuery.next()) {
        ImageItem item;
        const QString timeText = dataQuery.value(0).toString();
        item.receivedAt = QDateTime::fromString(timeText, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        if (!item.receivedAt.isValid()) item.receivedAt = QDateTime::fromString(timeText, Qt::ISODate);
        item.carrierId = dataQuery.value(1).toInt();
        item.rack = item.carrierId;
        item.wheelId = dataQuery.value(2).toInt();
        item.slot = item.wheelId;
        item.cameraId = dataQuery.value(3).toInt();
        item.result = dataQuery.value(4).toInt();
        const QString rawName = dataQuery.value(5).toString().trimmed();
        // 旧记录没有 min/max/deviation 列值：最小/最大回退到 distance，偏差值保持缺失
        item.min_value = dataQuery.value(6).toDouble();
        item.max_value = dataQuery.value(7).toDouble();
        item.dist_norm = dataQuery.value(8).toDouble();
        if (!dataQuery.value(9).isNull()) item.deviation = dataQuery.value(9).toDouble();
        item.fileName = QFileInfo(rawName).fileName();
        // resolveRecordImage 只返回已确认存在的路径，无需再次检查
        const QString resolvedPath = resolveRecordImage(dataQuery.value(10).toString(), rawName, archiveDir, sourceDir, timeText);
        if (!resolvedPath.isEmpty()) {
            item.filePath = resolvedPath;
            item.fileUrl = QUrl::fromLocalFile(resolvedPath).toString();
        }
        items.append(item);
    }
    if (totalCount == 0 && !items.isEmpty()) totalCount = items.size();
    return true;
}

QVariantMap alertOperation(const QSqlDatabase &db, const QString &kind,
                          const QString &start, const QString &end, int rack, int wheel,
                          int page, const QString &archiveDir, const QString &sourceDir,
                          const QString &operatorName)
{
    const auto fail = [](const QString &message) {
        return QVariantMap{{QStringLiteral("ok"), false}, {QStringLiteral("error"), message}};
    };
    const QDateTime from = AgcUtils::parseFlexibleDateTime(start, false);
    const QDateTime to = AgcUtils::parseFlexibleDateTime(end, true);
    const bool rackSummary = kind == QLatin1String("racks");
    const bool allWheels = rackSummary || (kind == QLatin1String("groups") && wheel == 0);
    if (!db.isOpen() || !from.isValid() || !to.isValid() || from > to
        || rack < (rackSummary ? 0 : 1) || rack > 50
        || wheel < (allWheels ? 0 : 1) || wheel > 16 || page < 1)
        return fail(QStringLiteral("报警操作参数无效，请检查日期、架号和轮位"));
    auto filter = buildRecordFilter(start, end, rack, allWheels ? QString() : kind == QLatin1String("groups")
                                    ? (wheel <= 8 ? QStringLiteral("WALK") : QStringLiteral("DRIVE"))
                                    : QString::number(wheel));
    filter.where << QStringLiteral("result = 0");
    QSqlQuery query(db);
    auto execute = [&](const QString &sql) {
        if (query.prepare(sql)) {
            filter.bind(query);
            if (query.exec()) return true;
        }
        LOG_ERROR("报警操作查询失败，架号={}，轮位={}，操作={}: {}",
                  rack, wheel, kind.toStdString(), query.lastError().text().toStdString());
        return false;
    };
    if (kind == QLatin1String("handled") || kind == QLatin1String("falseAlarm")) {
        filter.where << QStringLiteral("is_handled = 0 AND is_false_alarm = 0");
        const QString column = kind == QLatin1String("handled") ? QStringLiteral("is_handled") : QStringLiteral("is_false_alarm");
        if (!execute(QStringLiteral("UPDATE record SET %1 = 1").arg(column) + filter.clause())) {
            LOG_ERROR("报警处置失败，架号={}，轮位={}: {}", rack, wheel, query.lastError().text().toStdString());
            return fail(QStringLiteral("保存处置状态失败，请查看日志"));
        }
        const qint64 affected = query.numRowsAffected();
        LOG_INFO("报警处置完成，操作员={}，架号={}，轮位={}，操作={}，日期={} 至 {}，更新记录={}",
                 operatorName.isEmpty() ? std::string("未填写") : operatorName.toStdString(),
                 rack, wheel, kind == QLatin1String("handled") ? "已处理（已换轮）" : "误报",
                 start.toStdString(), end.toStdString(), affected);
        return {{QStringLiteral("ok"), true}, {QStringLiteral("affected"), affected}};
    }
    if (rackSummary || kind == QLatin1String("groups")) {
        const QString groupColumn = rackSummary ? QStringLiteral("carrier_id") : QStringLiteral("wheel_id");
        if (!execute(QStringLiteral(
                "SELECT %1, COUNT(*), SUM(is_handled), SUM(is_false_alarm), "
                "SUM(CASE WHEN is_handled=0 AND is_false_alarm=0 THEN 1 ELSE 0 END) FROM record").arg(groupColumn)
                + filter.clause() + QStringLiteral(" GROUP BY %1 ORDER BY %2%1")
                    .arg(groupColumn, rackSummary ? QStringLiteral("COUNT(*) DESC, ") : QString())))
            return fail(QStringLiteral("读取轮位处置状态失败"));
        QVariantList rows;
        while (query.next()) {
            rows.append(QVariantMap{{rackSummary ? QStringLiteral("rack") : QStringLiteral("wheel"), query.value(0)},
                                    {QStringLiteral("count"), query.value(1)},
                                    {QStringLiteral("handled"), query.value(2)},
                                    {QStringLiteral("falseAlarm"), query.value(3)},
                                    {QStringLiteral("pending"), query.value(4)}});
        }
        return {{QStringLiteral("ok"), true}, {QStringLiteral("rows"), rows}};
    }
    if (kind != QLatin1String("image")) return fail(QStringLiteral("未知报警操作"));
    // 按实际图片路径区分 FTP 同名不同内容的版本；缺路径时按文件名（不区分大小写）去重。
    const QString grouped = QStringLiteral(
        " FROM record") + filter.clause() + QStringLiteral(
        " GROUP BY lower(COALESCE(NULLIF(image_path, ''), imagename))");
    // COUNT 和取图处于同一只读快照，避免后台新增/清理记录导致页号不一致。
    QSqlQuery transaction(db);
    if (!transaction.exec(QStringLiteral("BEGIN"))) return fail(QStringLiteral("读取图片失败"));
    auto rollback = [&]() { transaction.exec(QStringLiteral("ROLLBACK")); };
    if (!execute(QStringLiteral("SELECT COUNT(*) FROM (SELECT 1") + grouped + QStringLiteral(")")) || !query.next()) {
        rollback();
        return fail(QStringLiteral("读取图片数量失败"));
    }
    const qint64 total = query.value(0).toLongLong();
    const qint64 current = qMin<qint64>(page, qMax<qint64>(1, total));
    query.finish();
    if (!execute(QStringLiteral("SELECT MAX(createtime), imagename, image_path") + grouped
                 + QStringLiteral(" ORDER BY MAX(createtime) DESC, lower(COALESCE(NULLIF(image_path, ''), imagename)) ASC LIMIT 1 OFFSET %1").arg(current - 1))) {
        rollback();
        return fail(QStringLiteral("读取图片失败"));
    }
    QVariantMap image;
    if (query.next()) {
        image.insert(QStringLiteral("time"), query.value(0));
        image.insert(QStringLiteral("name"), query.value(1));
        const QString path = resolveRecordImage(query.value(2).toString(), query.value(1).toString(), archiveDir, sourceDir,
                                                query.value(0).toString());
        image.insert(QStringLiteral("url"), path.isEmpty() ? QString() : QUrl::fromLocalFile(path).toString());
    }
    query.finish();
    if (!transaction.exec(QStringLiteral("COMMIT"))) {
        rollback();
        return fail(QStringLiteral("读取图片快照失败"));
    }
    return {{QStringLiteral("ok"), true}, {QStringLiteral("total"), total},
            {QStringLiteral("page"), current}, {QStringLiteral("image"), image}};
}

bool parseResultFilter(const QString &text, int &result)
{
    const QString token = text.trimmed();
    if (token.isEmpty()) return false;
    if (token.compare(QStringLiteral("OK"), Qt::CaseInsensitive) == 0 || token == QStringLiteral("1")) {
        result = 1;
        return true;
    }
    if (token.compare(QStringLiteral("NG"), Qt::CaseInsensitive) == 0 || token == QStringLiteral("0")) {
        result = 0;
        return true;
    }
    return false;
}

QVariantMap latestWheelImage(const QSqlDatabase &db, int rackNumber, int wheelNumber,
                             const QString &archiveDir, const QString &sourceDir)
{
    QVariantMap image;
    if (rackNumber < 1 || rackNumber > 50 || wheelNumber < 1 || wheelNumber > 16) return image;
    if (!db.isValid() || !db.isOpen()) return image;

    // 走 idx_record_carrier_wheel_cover(carrier_id, wheel_id, createtime DESC, ...)
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT imagename, createtime, result, image_path FROM record "
        "WHERE carrier_id = :rack AND wheel_id = :wheel "
        "ORDER BY createtime DESC, rowid DESC LIMIT 1"));
    query.bindValue(QStringLiteral(":rack"), rackNumber);
    query.bindValue(QStringLiteral(":wheel"), wheelNumber);
    if (!query.exec()) {
        LOG_ERROR("最新轮位图像查询失败: 架号={}, 轮位={}, {}", rackNumber, wheelNumber,
                  query.lastError().text().toStdString());
        return image;
    }
    if (!query.next()) return image;

    const QString imageName = query.value(0).toString().trimmed();
    QString imagePath = resolveRecordImage(query.value(3).toString(), imageName, archiveDir, sourceDir,
                                           query.value(1).toString());
    if (imagePath.isEmpty()) imagePath = imageName;
    image.insert(QStringLiteral("filePath"), imagePath);
    image.insert(QStringLiteral("fileName"), QFileInfo(imagePath).fileName());
    image.insert(QStringLiteral("time"), query.value(1).toString());
    image.insert(QStringLiteral("result"), query.value(2).toInt());
    return image;
}

} // namespace SearchService
