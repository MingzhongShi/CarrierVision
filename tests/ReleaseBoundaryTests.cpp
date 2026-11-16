#include <QtTest>
#include <QTemporaryDir>
#include <QImage>
#include <QBuffer>
#include <QSqlQuery>
#include <QSqlError>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include "data/DbSchema.h"
#include "common/AgcUtils.h"
#include "ingest/ImageIngest.h"
#include "ingest/IngestCoordinator.h"
#include <cmath>
#include "data/ArchiveMaintenance.h"
#include "data/DatabaseGuard.h"
#include "network/FtpServer.h"
#include "network/FtpIngestService.h"
#include "network/TcpDataReceiver.h"
#include "common/AppLogger.h"
#include "app/ThumbnailCache.h"
#include "common/ImageExport.h"
#include "common/LogExport.h"
#include "common/ZipWriter.h"
#include <QXmlStreamReader>
#include <QtEndian>

class ReleaseBoundaryTests : public QObject {
    Q_OBJECT
    QSqlDatabase db;
    static QString reply(QTcpSocket &socket) {
        QElapsedTimer timer; timer.start();
        while (!socket.canReadLine() && timer.elapsed() < 3000) QTest::qWait(1);
        return QString::fromUtf8(socket.readLine()).trimmed();
    }
    static QString command(QTcpSocket &socket, const QByteArray &cmd) {
        socket.write(cmd + "\r\n"); return reply(socket);
    }
    static QString imageName(const QString &root) { return root + "/upload-sample.png"; }
    static bool makeImage(const QString &path, Qt::GlobalColor color = Qt::red) {
        QImage image(8,8,QImage::Format_RGB32); image.fill(color); return image.save(path,"PNG");
    }
private slots:
    // 放大窗口“另存为”：同格式按字节复制，异格式转码，拒绝覆盖原图与无效输入
    void imageExportCopiesOrTranscodesWithoutTouchingSource()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString source = dir.filePath(QStringLiteral("CAM01_20261112.png"));
        QImage image(64, 48, QImage::Format_RGB32);
        image.fill(Qt::darkCyan);
        QVERIFY(image.save(source, "PNG"));
        QFile sourceFile(source);
        QVERIFY(sourceFile.open(QIODevice::ReadOnly));
        const QByteArray sourceBytes = sourceFile.readAll();
        sourceFile.close();

        const QVariantMap info = ImageExport::probe(QUrl::fromLocalFile(source).toString());
        QCOMPARE(info.value(QStringLiteral("exists")).toBool(), true);
        QCOMPARE(info.value(QStringLiteral("width")).toInt(), 64);
        QCOMPARE(info.value(QStringLiteral("height")).toInt(), 48);
        QCOMPARE(info.value(QStringLiteral("bytes")).toLongLong(), sourceBytes.size());

        // 同格式：字节完全一致；目标目录不存在时自动创建
        const QString copyTarget = dir.filePath(QStringLiteral("export/sub/copy.png"));
        ImageExport::Result copied = ImageExport::exportImage(QUrl::fromLocalFile(source).toString(),
                                                              QUrl::fromLocalFile(copyTarget).toString());
        QVERIFY2(copied.ok, qPrintable(copied.message));
        QFile copyFile(copyTarget);
        QVERIFY(copyFile.open(QIODevice::ReadOnly));
        QCOMPARE(copyFile.readAll(), sourceBytes);
        copyFile.close();

        // 未写扩展名时沿用原图扩展名
        const ImageExport::Result noSuffix = ImageExport::exportImage(source, dir.filePath(QStringLiteral("plain")));
        QVERIFY2(noSuffix.ok, qPrintable(noSuffix.message));
        QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("plain.png"))));

        // 异格式：转码后尺寸不变
        const QString bmpTarget = dir.filePath(QStringLiteral("converted.bmp"));
        const ImageExport::Result converted = ImageExport::exportImage(source, bmpTarget);
        QVERIFY2(converted.ok, qPrintable(converted.message));
        const QImage reread(bmpTarget);
        QCOMPARE(reread.size(), image.size());

        // 覆盖原图、原图缺失、相对或非本地目标、未知格式都拒绝，原图保持不变
        QVERIFY(!ImageExport::exportImage(source, source).ok);
        QVERIFY(!ImageExport::exportImage(dir.filePath(QStringLiteral("missing.png")), copyTarget).ok);
        QVERIFY(!ImageExport::exportImage(source, QStringLiteral("http://example.com/a.png")).ok);
        QVERIFY(!ImageExport::exportImage(source, dir.filePath(QStringLiteral("bad.xyz"))).ok);
        QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("bad.xyz"))));
        QVERIFY(sourceFile.open(QIODevice::ReadOnly));
        QCOMPARE(sourceFile.readAll(), sourceBytes);

        // Windows 盘符路径不被当成 URL 协议
        QCOMPARE(ImageExport::localPathFromSource(QStringLiteral("D:\\images\\a.png")), QStringLiteral("D:/images/a.png"));
        QCOMPARE(ImageExport::localPathFromSource(QStringLiteral("qrc:/a.png")), QString());
        QCOMPARE(ImageExport::supportedTargetSuffixes(QStringLiteral("JPEG")).value(0), QStringLiteral("jpg"));
    }

    void init() {
        db = QSqlDatabase::addDatabase("QSQLITE", "release-test");
        db.setDatabaseName(":memory:"); QVERIFY(db.open()); QVERIFY(DBSchema::ensureAllTables(db));
    }
    void cleanup() { db.close(); db = {}; QSqlDatabase::removeDatabase("release-test"); }
    void transactionRollsBackBothWheels() {
        ImageIngest::BatchData batch;
        batch.carrierId = 1;
        batch.timestamp = QDateTime::fromString("2026-09-05 10:00:00", "yyyy-MM-dd HH:mm:ss");
        batch.batchId = "BATCH_ROLLBACK_TEST";
        ImageIngest::WheelItem w1;
        w1.wheelId = 1;
        w1.cameraId = 1;
        w1.result = 0; // NG
        w1.imageName = "rollback_w1.png";
        ImageIngest::WheelItem w2;
        w2.wheelId = 2;
        w2.cameraId = 1;
        w2.result = 0; // NG
        w2.imageName = "rollback_w2.png";
        batch.wheels = {w1, w2};

        QSqlQuery q(db);
        QVERIFY(q.exec("CREATE TRIGGER fail_second BEFORE INSERT ON record WHEN NEW.wheel_id=2 BEGIN SELECT RAISE(ABORT,'injected'); END"));
        QString error;
        QVERIFY(!ImageIngest::recordBatch(db, batch, error));
        for (const auto &table : {"record","alertrecord"}) {
            QVERIFY(q.exec(QString("SELECT COUNT(*) FROM %1").arg(table))); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),0);
        }
        QVERIFY(q.exec("DROP TRIGGER fail_second"));
        QVERIFY(ImageIngest::recordBatch(db, batch, error));
        QVERIFY(q.exec("SELECT COUNT(*) FROM record WHERE batch_id='BATCH_ROLLBACK_TEST'"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 2);
    }
    void batchJsonRejectsInvalidAndOversizedPayloads() {
        ImageIngest::BatchData batch;
        QString error;
        QJsonObject wheel{{"wheelId", 1}, {"cameraId", 1}, {"minValue", 118.0}, {"maxValue", 121.0},
                          {"baseValue", 120.0}, {"deviation", 1.0}, {"result", "OK"}, {"imageName", "cam01.png"}};
        QJsonObject root{{"carrierId", 1}, {"wheels", QJsonArray{wheel}}};
        QVERIFY(ImageIngest::parseBatchJson(root, batch, error));

        // imageName：带路径时只取文件名；空名、. 开头、非图片扩展名拒收
        auto imageNameResult = [&](const QString &name) {
            QJsonObject w = wheel; w["imageName"] = name;
            QJsonObject r = root; r["wheels"] = QJsonArray{w};
            return ImageIngest::parseBatchJson(r, batch, error) ? batch.wheels.first().imageName : QStringLiteral("<rejected>");
        };
        QCOMPARE(imageNameResult("D:\\cam\\A_1.png"), QStringLiteral("A_1.png"));
        QCOMPARE(imageNameResult("/data/cam/../A_2.JPG"), QStringLiteral("A_2.JPG"));
        QCOMPARE(imageNameResult(""), QStringLiteral("<rejected>"));
        QCOMPARE(imageNameResult("cam/"), QStringLiteral("<rejected>"));
        QCOMPARE(imageNameResult("../.hidden.png"), QStringLiteral("<rejected>"));
        QCOMPARE(imageNameResult("report.txt"), QStringLiteral("<rejected>"));
        // 文件名超过 240 字符（NTFS 255 减去 .cv-pending 标记与 _vN 余量）拒收
        QCOMPARE(imageNameResult(QString(236, u'a') + ".jpg"), QString(236, u'a') + ".jpg");
        QCOMPARE(imageNameResult(QString(237, u'a') + ".jpg"), QStringLiteral("<rejected>"));
        QVERIFY(error.contains(QStringLiteral("文件名过长")));

        // 时间戳范围：未来不超过 10 分钟，过去不超过保留天数；未提供限制时不检查
        const QDateTime now = QDateTime::fromString("2026-10-20 12:00:00", "yyyy-MM-dd HH:mm:ss");
        ImageIngest::ParseLimits limits;
        limits.now = now;
        limits.maxAgeDays = 90;
        auto timestampOk = [&](const QDateTime &time) {
            QJsonObject r = root; r["timestamp"] = time.toString("yyyyMMdd_HHmmss");
            return ImageIngest::parseBatchJson(r, batch, error, limits);
        };
        QVERIFY(timestampOk(now.addSecs(300)));
        QVERIFY(!timestampOk(now.addSecs(900)));
        QVERIFY(timestampOk(now.addDays(-89)));
        QVERIFY(!timestampOk(now.addDays(-91)));
        QJsonObject future = root; future["timestamp"] = "20990101_000000";
        QVERIFY(ImageIngest::parseBatchJson(future, batch, error));
        QVERIFY(!ImageIngest::parseBatchJson(future, batch, error, limits));
        QVERIFY(error.contains(QStringLiteral("客户端时钟")));

        for (const int invalidWheel : {0, 17, 18}) {
            wheel["wheelId"] = invalidWheel;
            root["wheels"] = QJsonArray{wheel};
            QVERIFY(!ImageIngest::parseBatchJson(root, batch, error));
        }
        wheel["wheelId"] = 1;

        root["carrierId"] = 1.5;
        QVERIFY(!ImageIngest::parseBatchJson(root, batch, error));
        root["carrierId"] = 1;
        wheel["result"] = "UNKNOWN";
        root["wheels"] = QJsonArray{wheel};
        QVERIFY(!ImageIngest::parseBatchJson(root, batch, error));
        wheel["result"] = "OK";
        wheel["cameraId"] = 13;
        root["wheels"] = QJsonArray{wheel};
        QVERIFY(!ImageIngest::parseBatchJson(root, batch, error));
        QJsonArray tooMany;
        wheel["cameraId"] = 1;
        for (int i = 0; i < 33; ++i) {
            QJsonObject entry = wheel;
            entry["wheelId"] = i % 16 + 1;
            entry["cameraId"] = i % 12 + 1;
            tooMany.append(entry);
        }
        root["wheels"] = tooMany;
        QVERIFY(!ImageIngest::parseBatchJson(root, batch, error));
    }
    void requestIdMakesBatchInsertIdempotent() {
        ImageIngest::BatchData batch;
        batch.carrierId = 2;
        batch.timestamp = QDateTime::currentDateTime();
        batch.batchId = "batch-idempotent";
        batch.requestId = "request-uuid-1";
        ImageIngest::WheelItem wheel;
        wheel.wheelId = 1;
        wheel.cameraId = 1;
        wheel.result = 1;
        wheel.imageName = "one.png";
        batch.wheels = {wheel};
        QString error;
        QVERIFY2(ImageIngest::recordBatch(db, batch, error), qPrintable(error));
        QVERIFY2(ImageIngest::recordBatch(db, batch, error), qPrintable(error));
        QSqlQuery q(db);
        QVERIFY(q.exec("SELECT COUNT(*) FROM record WHERE batch_id='batch-idempotent'"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 1);

        batch.wheels.append(wheel);
        QVERIFY(!ImageIngest::recordBatch(db, batch, error));
    }
    void lateNgCannotResurrectOldAlert() {
        QSqlQuery q(db);
        QVERIFY(q.exec("INSERT INTO record(createtime,carrier_id,camera_id,wheel_id,result,imagename) VALUES('2026-09-05 11:00:00',1,1,1,1,'new.png')"));
        QVERIFY(q.exec("INSERT INTO record(createtime,carrier_id,camera_id,wheel_id,result,imagename) VALUES('2026-09-05T10:00:00',1,1,1,0,'old.png')"));
        QVERIFY(q.exec("SELECT COUNT(*) FROM alertrecord")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),0);
        q.finish(); QVERIFY(DBSchema::ensureAllTables(db)); // existing installation migration is repeatable
    }
    void immutableUploadAndRetryJournal() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); QString error;
        const QString target = imageName(dir.path()), staged = dir.filePath("staged");
        QVERIFY(makeImage(staged));
        QVERIFY(!ImageIngest::accept(staged,target,[](const QString&){return false;},error));
        QVERIFY(QFile::exists(target)); QVERIFY(QFile::exists(target+".cv-pending"));
        QVERIFY(makeImage(staged,Qt::blue));
        QVERIFY(!ImageIngest::accept(staged,target,[](const QString&){return true;},error));
        QCOMPARE(QImage(target).pixelColor(0,0),QColor(Qt::red));
        QVERIFY(makeImage(staged));
        QVERIFY(ImageIngest::accept(staged,target,[](const QString&){return true;},error));
        QVERIFY(!QFile::exists(target+".cv-pending"));
    }
    void corruptImageNeverReplacesHistory() {
        QTemporaryDir dir; QString error;
        const QString target = imageName(dir.path()), staged = dir.filePath("broken");
        QVERIFY(makeImage(target)); QFile f(staged); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("not an image"); f.close();
        QVERIFY(!ImageIngest::accept(staged,target,[](const QString&){return true;},error));
        QCOMPARE(QImage(target).pixelColor(0,0),QColor(Qt::red));
    }
    void cleanupKeepsNewFilesInOldParent() {
        QTemporaryDir dir; QVERIFY(QDir().mkpath(dir.filePath("camera/deep")));
        const QString old = "camera/deep/old.png", fresh = "camera/deep/new.png";
        QVERIFY(makeImage(dir.filePath(old))); QVERIFY(makeImage(dir.filePath(fresh)));
        QSqlQuery q(db);
        q.prepare("INSERT INTO record(createtime,carrier_id,camera_id,wheel_id,result,imagename) VALUES(:t,1,1,1,1,:n)");
        q.bindValue(":t","2026-06-07 00:00:00"); q.bindValue(":n",old); QVERIFY(q.exec());
        q.bindValue(":t","2026-06-07 23:59:59"); q.bindValue(":n",fresh); QVERIFY(q.exec()); q.finish();
        int removed; bool more; QString error;
        QVERIFY2(ArchiveMaintenance::cleanup(db,dir.path(),{dir.path()},QDateTime::fromString("2026-06-07T01:00:00",Qt::ISODate),removed,more,error),qPrintable(error));
        QCOMPARE(removed,1); QVERIFY(!QFile::exists(dir.filePath(old))); QVERIFY(QFile::exists(dir.filePath(fresh)));
        QVERIFY(QDir(dir.filePath("camera/deep")).exists());
        QVERIFY(q.exec("SELECT COUNT(*) FROM record")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),1);
    }
    void cleanupPrunesExpiredIngestRequests() {
        // TCP 重发去重表随清理一起删除过期项，否则只增不减
        QTemporaryDir dir; QSqlQuery q(db);
        QVERIFY(q.exec("INSERT INTO ingest_requests(request_id,carrier_id,item_count,committed_at) VALUES('old',1,16,'2026-06-01 08:00:00')"));
        QVERIFY(q.exec("INSERT INTO ingest_requests(request_id,carrier_id,item_count,committed_at) VALUES('new',1,16,'2026-06-07 08:00:00')"));
        int removed; bool more; QString error; ArchiveMaintenance::CleanupStats stats;
        QVERIFY2(ArchiveMaintenance::cleanup(db,dir.path(),{dir.path()},QDateTime::fromString("2026-06-07T01:00:00",Qt::ISODate),removed,more,error,&stats),qPrintable(error));
        QCOMPARE(stats.requestsDeleted,1);
        QVERIFY(q.exec("SELECT request_id FROM ingest_requests")); QVERIFY(q.next()); QCOMPARE(q.value(0).toString(),QString("new")); QVERIFY(!q.next());
    }
    void cleanupRecoveryHonorsNewReference() {
        QTemporaryDir dir; const QString target = dir.filePath("keep.png"); QVERIFY(makeImage(target));
        QSqlQuery q(db);
        q.prepare("INSERT INTO cleanup_files(path,imagename) VALUES(:path,'keep.png')"); q.bindValue(":path",target); QVERIFY(q.exec());
        QVERIFY(q.exec("INSERT INTO record(createtime,carrier_id,camera_id,wheel_id,result,imagename) VALUES('2026-09-05 11:00:00',1,1,1,1,'keep.png')"));
        int removed; bool more; QString error;
        QVERIFY(ArchiveMaintenance::cleanup(db,dir.path(),{dir.path()},QDateTime::fromString("2026-06-07T01:00:00",Qt::ISODate),removed,more,error));
        QVERIFY(QFile::exists(target)); QCOMPARE(removed,0);
    }
    void cleanupProgressesPastUnsafePathsAndUsesImagePath() {
        QTemporaryDir dir; QVERIFY(QDir().mkpath(dir.filePath("cam1")));
        QSqlQuery q(db);
        q.prepare("INSERT INTO record(createtime,carrier_id,camera_id,wheel_id,result,imagename,image_path) VALUES(:t,1,1,1,1,:n,:p)");
        auto add = [&](const QString &time, const QString &name, const QVariant &path) {
            q.bindValue(":t", time); q.bindValue(":n", name); q.bindValue(":p", path); return q.exec();
        };
        // 500 组路径不安全的过期记录（历史数据中的绝对路径与 ../），曾导致清理永久停滞
        for (int i = 0; i < 500; ++i)
            QVERIFY(add("2025-01-01 00:00:00", (i % 2 ? "/data/cam/A_%1.png" : "../cam/A_%1.png") + QString::number(i), QVariant()));
        QVERIFY(makeImage(dir.filePath("normal.png")));
        QVERIFY(add("2025-01-01 00:00:00", "normal.png", QVariant()));
        // 实际落盘在子目录、且被版本化的文件：按 image_path 清理
        const QString versioned = dir.filePath("cam1/x_v2.png");
        QVERIFY(makeImage(versioned));
        QVERIFY(add("2025-01-01 00:00:00", "x.png", versioned));
        q.finish();

        const QDateTime cutoff = QDateTime::fromString("2026-01-01T00:00:00", Qt::ISODate);
        int removed = 0; bool more = false; QString error;
        ArchiveMaintenance::CleanupStats stats;
        int rounds = 0;
        do {
            QVERIFY2(ArchiveMaintenance::cleanup(db, dir.path(), {dir.path()}, cutoff, removed, more, error, &stats), qPrintable(error));
        } while (more && ++rounds < 5);
        QVERIFY(!QFile::exists(dir.filePath("normal.png")));
        QVERIFY(!QFile::exists(versioned));
        QVERIFY(q.exec("SELECT COUNT(*) FROM record")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(), 0);
    }
    void orphanSweepDeletesOnlyOldUnreferencedImages() {
        QTemporaryDir dir; QVERIFY(QDir().mkpath(dir.filePath("sub")));
        const QDateTime old = QDateTime::currentDateTime().addDays(-200);
        auto makeOld = [&](const QString &rel) {
            const QString path = dir.filePath(rel);
            if (!makeImage(path)) return false;
            QFile f(path);
            return f.open(QIODevice::ReadWrite) && f.setFileTime(old, QFileDevice::FileModificationTime);
        };
        QVERIFY(makeOld("orphan.png"));
        QVERIFY(makeOld("sub/orphan_v2.png"));
        QVERIFY(makeOld("by_name.png"));
        QVERIFY(makeOld("by_path.png"));
        QVERIFY(makeOld("pending.png"));
        QFile marker(dir.filePath("pending.png.cv-pending")); QVERIFY(marker.open(QIODevice::WriteOnly)); marker.close();
        QVERIFY(makeImage(dir.filePath("fresh.png")));
        QSqlQuery q(db);
        QVERIFY(q.exec("INSERT INTO record(createtime,carrier_id,camera_id,wheel_id,result,imagename) VALUES('2026-01-01 00:00:00',1,1,1,1,'by_name.png')"));
        q.prepare("INSERT INTO record(createtime,carrier_id,camera_id,wheel_id,result,imagename,image_path) VALUES('2026-01-01 00:00:00',1,1,2,1,'other.png',:p)");
        q.bindValue(":p", QFileInfo(dir.filePath("by_path.png")).canonicalFilePath()); QVERIFY(q.exec());
        QString error;
        const int deleted = ArchiveMaintenance::sweepOrphans(db, {dir.path(), dir.path()}, QDateTime::currentDateTime().addDays(-97), 100, error);
        QCOMPARE(deleted, 2);
        QVERIFY(!QFile::exists(dir.filePath("orphan.png")));
        QVERIFY(!QFile::exists(dir.filePath("sub/orphan_v2.png")));
        for (const char *kept : {"by_name.png", "by_path.png", "pending.png", "fresh.png"})
            QVERIFY2(QFile::exists(dir.filePath(kept)), kept);
        QVERIFY(QDir(dir.filePath("sub")).exists());
    }
    void validateRejectsTruncatedImages() {
        QTemporaryDir dir; QString error;
        QImage image(64, 48, QImage::Format_RGB32); image.fill(Qt::darkCyan);
        for (const char *fmt : {"jpg", "png", "bmp"}) {
            const QString path = dir.filePath(QString("full.%1").arg(fmt));
            QVERIFY(image.save(path, fmt));
            QVERIFY2(ImageIngest::validate(path, path, error), qPrintable(error));
            QFile f(path); QVERIFY(f.open(QIODevice::ReadOnly)); const QByteArray data = f.readAll(); f.close();
            const QString cut = dir.filePath(QString("cut.%1").arg(fmt));
            QFile c(cut); QVERIFY(c.open(QIODevice::WriteOnly)); c.write(data.left(data.size() * 2 / 3)); c.close();
            QVERIFY2(!ImageIngest::validate(cut, cut, error), fmt);
            if (QByteArray(fmt) == "jpg") {
                // 部分相机在 JPEG 结束标记后补零：仍应接受
                const QString padded = dir.filePath("padded.jpg");
                QFile p(padded); QVERIFY(p.open(QIODevice::WriteOnly)); p.write(data + QByteArray(64, '\0')); p.close();
                QVERIFY2(ImageIngest::validate(padded, padded, error), qPrintable(error));
            }
        }
    }
    void ftpLocksOutRepeatedFailuresAndUsesPassiveRange() {
        QTemporaryDir dir; FtpServer server;
        server.setPassivePortRange(52300, 52310);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0, dir.path(), QMap<QString,QString>{{"u","p"}}));
        // 被动端口落在配置范围内
        {
            QTcpSocket client; client.connectToHost(QHostAddress::LocalHost, server.port());
            QVERIFY(reply(client).startsWith("220"));
            QVERIFY(command(client, "USER u").startsWith("331"));
            QVERIFY(command(client, "PASS p").startsWith("230"));
            const quint16 port = command(client, "EPSV").section('|', 3, 3).toUShort();
            QVERIFY2(port >= 52300 && port <= 52310, qPrintable(QString::number(port)));
        }
        // 每个连接失败 5 次后被断开；同一来源累计 10 次后封禁，新连接直接 421
        for (int connection = 0; connection < 2; ++connection) {
            QTcpSocket client; client.connectToHost(QHostAddress::LocalHost, server.port());
            QVERIFY(reply(client).startsWith("220"));
            for (int i = 0; i < 5; ++i) {
                command(client, "USER u");
                command(client, "PASS wrong");
            }
            QTRY_VERIFY(client.state() != QAbstractSocket::ConnectedState || !client.isValid());
        }
        QTcpSocket banned; banned.connectToHost(QHostAddress::LocalHost, server.port());
        const QString bannedReply = reply(banned);
        QVERIFY2(bannedReply.startsWith("421"), qPrintable(bannedReply));
        server.stop();
    }
    void databaseBackupAndCorruptionRecovery() {
        QTemporaryDir dir;
        const QString dbPath = dir.filePath("dataAgc.db");
        const QString backupDir = dir.filePath("backup");
        {
            QSqlDatabase file = QSqlDatabase::addDatabase("QSQLITE", "guard-test");
            file.setDatabaseName(dbPath);
            QVERIFY(file.open());
            QVERIFY(DBSchema::ensureAllTables(file));
            QSqlQuery q(file);
            QVERIFY(q.exec("INSERT INTO record(createtime,carrier_id,camera_id,wheel_id,result,imagename) VALUES('2026-10-01 00:00:00',1,1,1,1,'kept.png')"));
            QVERIFY(file.transaction());
            for (int i = 0; i < 2000; ++i)
                QVERIFY(q.exec(QString("INSERT INTO record(createtime,carrier_id,camera_id,wheel_id,result,imagename) VALUES('2026-10-01 00:00:00',1,1,1,1,'pad_%1.png')").arg(i)));
            QVERIFY(file.commit());
            QString detail; QVERIFY(DatabaseGuard::quickCheck(file, detail));
            QString error, created;
            for (int i = 0; i < 4; ++i) {
                QVERIFY2(DatabaseGuard::backup(file, backupDir, 3, error, &created), qPrintable(error));
                QTest::qWait(1100); // 备份文件名精确到秒
            }
            QCOMPARE(DatabaseGuard::backups(backupDir).size(), 3);   // 只保留最近 3 份
            QCOMPARE(DatabaseGuard::backups(backupDir).first(), created);
            file.close();
        }
        QSqlDatabase::removeDatabase("guard-test");

        QVERIFY(DatabaseGuard::recoverIfCorrupt(dbPath, backupDir).isEmpty()); // 正常库不处理

        // 破坏数据页（保留文件头，使其仍能打开但检查失败）
        QFile f(dbPath); QVERIFY(f.open(QIODevice::ReadWrite));
        const qint64 size = f.size();
        QVERIFY(f.seek(size / 2)); QVERIFY(f.write(QByteArray(8192, '\x5a')) == 8192); f.close();

        const QString message = DatabaseGuard::recoverIfCorrupt(dbPath, backupDir);
        QVERIFY2(message.contains("已从备份"), qPrintable(message));
        QVERIFY(!QDir(dir.path()).entryList({"dataAgc.db.corrupt-*"}, QDir::Files).isEmpty()); // 原文件保留
        {
            QSqlDatabase restored = QSqlDatabase::addDatabase("QSQLITE", "guard-restored");
            restored.setDatabaseName(dbPath);
            QVERIFY(restored.open());
            QString detail; QVERIFY(DatabaseGuard::quickCheck(restored, detail));
            QSqlQuery q(restored);
            QVERIFY(q.exec("SELECT COUNT(*) FROM record WHERE imagename='kept.png'")); QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), 1);
            restored.close();
        }
        QSqlDatabase::removeDatabase("guard-restored");
    }
    void logsUseNumericVolumesAndBoundedPagination() {
        QTemporaryDir dir;
        AppLogger::init(dir.path()); AppLogger::shutdown();
        for (const auto &entry : QList<QPair<QString,QString>>{{"", "00"},{".2","02"},{".10","10"}}) {
            QFile file(dir.filePath("carrier_2025-01-01"+entry.first+".log"));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(("[2025-01-01 10:00:"+entry.second+".000] [info] volume"+entry.second+"\n").toUtf8());
        }
        auto result=AppLogger::queryLogs("2025-01-01","all","",1,2);
        QCOMPARE(result["total"].toInt(),3);
        auto items=result["items"].toList(); QCOMPARE(items.size(),2);
        QCOMPARE(items[0].toMap()["message"].toString(),QString("volume10"));
        QCOMPARE(items[1].toMap()["message"].toString(),QString("volume02"));
        result=AppLogger::queryLogs("2025-01-01","all","",2147483647,2147483647);
        QCOMPARE(result["pageSize"].toInt(),200); QVERIFY(result["items"].toList().isEmpty());
        QVERIFY(AppLogger::queryLogs("*","all","",1,2).contains("error"));
    }
    void logRangeQuerySpansDaysNewestFirst() {
        QTemporaryDir dir;
        AppLogger::init(dir.path()); AppLogger::shutdown();
        const QList<QPair<QString,QString>> files{
            {"carrier_2025-01-01.log", "[2025-01-01 09:00:00.000] [info] 报警处置完成 day1\n"},
            {"carrier_2025-01-02.log", "[2025-01-02 09:00:00.000] [warning] day2-a\n[2025-01-02 10:00:00.000] [info] 报警处置完成 day2-b\n"},
            {"carrier_2025-01-02.1.log", "[2025-01-02 11:00:00.000] [info] day2-c\n"},
            {"carrier_2025-01-04.log", "[2025-01-04 09:00:00.000] [info] day4\n"},
        };
        for (const auto &entry : files) {
            QFile file(dir.filePath(entry.first));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(entry.second.toUtf8());
        }
        auto result = AppLogger::queryLogRange("2025-01-01", "2025-01-03", "all", "", 1, 10);
        QCOMPARE(result["total"].toInt(), 4);
        QCOMPARE(result["startDate"].toString(), QString("2025-01-01"));
        QCOMPARE(result["endDate"].toString(), QString("2025-01-03"));
        auto items = result["items"].toList();
        QCOMPARE(items.size(), 4);
        QCOMPARE(items.first().toMap()["message"].toString(), QString("day2-c"));
        QCOMPARE(items.last().toMap()["message"].toString(), QString("报警处置完成 day1"));
        // 起止颠倒自动交换；分页跨文件
        result = AppLogger::queryLogRange("2025-01-04", "2025-01-01", "all", "", 2, 2);
        QCOMPARE(result["total"].toInt(), 5);
        items = result["items"].toList();
        QCOMPARE(items.size(), 2);
        QCOMPARE(items[0].toMap()["message"].toString(), QString("报警处置完成 day2-b"));
        QCOMPARE(items[1].toMap()["message"].toString(), QString("day2-a"));
        // 类型、级别筛选在跨天时同样生效
        QCOMPARE(AppLogger::queryLogRange("2025-01-01", "2025-01-04", "all", "", 1, 10, "alertDisposition")["total"].toInt(), 2);
        QCOMPARE(AppLogger::queryLogRange("2025-01-01", "2025-01-04", "warn", "", 1, 10)["total"].toInt(), 1);
        // 单日接口等价于起止相同的范围
        QCOMPARE(AppLogger::queryLogs("2025-01-02", "all", "", 1, 10)["total"].toInt(), 3);
        QVERIFY(AppLogger::queryLogRange("2025-01-01", "bad", "all", "", 1, 10).contains("error"));
        const auto tooLong = AppLogger::queryLogRange("2024-01-01", "2025-01-04", "all", "", 1, 10);
        QVERIFY(tooLong.contains("error"));
        QCOMPARE(tooLong["total"].toInt(), 0);
    }
    void logQueryReadsFileStillOpenForWriting() {
        // 日志文件由写入方一直打开；查询必须按打开句柄的实际长度读取，不能依赖目录项里的大小
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        AppLogger::init(dir.path());
        for (int i = 0; i < 20; ++i) LOG_INFO("持续写入的日志 {}", i);
        AppLogger::logger()->flush();
        const QString date = QDate::currentDate().toString("yyyy-MM-dd");
        QCOMPARE(AppLogger::queryLogs(date, "all", "持续写入的日志", 1, 50)["total"].toInt(), 20);
        LOG_INFO("持续写入的日志 {}", 20);
        AppLogger::logger()->flush();
        QCOMPARE(AppLogger::queryLogRange(date, date, "all", "持续写入的日志", 1, 50)["total"].toInt(), 21);
        AppLogger::shutdown();
    }
    void ftpTimeoutReleasesCountExactlyOnce() {
        QTemporaryDir dir; FtpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost,0,dir.path(),QMap<QString,QString>{{"u","p"}}));
        QTcpSocket client; client.connectToHost(QHostAddress::LocalHost,server.port());
        QVERIFY(reply(client).startsWith("220")); QCOMPARE(server.clientCount(),1);
        auto session = server.findChild<FtpControlConnection*>(); QVERIFY(session);
        session->findChild<QTimer*>()->start(1);
        QTRY_COMPARE(server.clientCount(),0);
        QTRY_VERIFY(server.findChildren<FtpControlConnection*>().isEmpty());
        server.stop(); QCOMPARE(server.clientCount(),0);
    }
    void ftpDelayedDataConnectionDoesNotBlockEventLoop() {
        QTemporaryDir dir; FtpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost,0,dir.path(),QMap<QString,QString>{{"u","p"}}));
        QTcpSocket client; client.connectToHost(QHostAddress::LocalHost,server.port()); QVERIFY(reply(client).startsWith("220"));
        QVERIFY(command(client,"USER u").startsWith("331")); QVERIFY(command(client,"PASS p").startsWith("230"));
        QString epsv = command(client,"EPSV"); QVERIFY(epsv.startsWith("229"));
        const auto port = epsv.section('|',3,3).toUShort(); QVERIFY(port);
        bool tick = false; QTimer::singleShot(20,[&] {tick=true;});
        QVERIFY(command(client,"LIST").startsWith("150"));
        QTRY_VERIFY_WITH_TIMEOUT(tick,500);
        QTcpSocket data; data.connectToHost(QHostAddress::LocalHost,port);
        QVERIFY(reply(client).startsWith("226"));
        server.stop();
    }
    void ftpRetrStreamsCompleteContentAndAbortDropsPartialUpload() {
        QTemporaryDir dir; FtpServer server;
        QByteArray expected(2*1024*1024,'x');
        QFile source(dir.filePath("download.bin")); QVERIFY(source.open(QIODevice::WriteOnly)); QCOMPARE(source.write(expected),qint64(expected.size())); source.close();
        QVERIFY(server.listen(QHostAddress::LocalHost,0,dir.path(),QMap<QString,QString>{{"u","p"}}));
        QTcpSocket client; client.connectToHost(QHostAddress::LocalHost,server.port()); QVERIFY(reply(client).startsWith("220"));
        QVERIFY(command(client,"USER u").startsWith("331")); QVERIFY(command(client,"PASS p").startsWith("230"));
        const QString epsv=command(client,"EPSV");
        QTcpSocket data; data.connectToHost(QHostAddress::LocalHost,epsv.section('|',3,3).toUShort());
        QTRY_COMPARE(data.state(),QAbstractSocket::ConnectedState);
        QVERIFY(command(client,"RETR download.bin").startsWith("150"));
        QByteArray received;
        QElapsedTimer timer; timer.start();
        while (received.size()<expected.size() && timer.elapsed()<3000) { received+=data.readAll(); QTest::qWait(1); }
        QCOMPARE(received,expected); QVERIFY(reply(client).startsWith("226"));
        const QString second=command(client,"EPSV");
        QTcpSocket upload; upload.connectToHost(QHostAddress::LocalHost,second.section('|',3,3).toUShort());
        QTRY_COMPARE(upload.state(),QAbstractSocket::ConnectedState);
        QVERIFY(command(client,"STOR upload-sample.png").startsWith("150"));
        upload.write("partial image");
        QVERIFY(command(client,"ABOR").startsWith("226"));
        QVERIFY(!QFile::exists(imageName(dir.path())));
        QVERIFY(!QFile::exists(imageName(dir.path())+".cv-pending"));
        QTRY_VERIFY(QDir(dir.path()).entryList({".cv-upload-*"},QDir::Files|QDir::Hidden).isEmpty());
        server.stop();
    }
    void ftpUploadAndCredentialRevocation() {
        QTemporaryDir dir; FtpServer server; QString error;
        server.uploadHandler = [&](const QString &staged,const QString &target,const QString &) { return ImageIngest::accept(staged,target,[](const QString&){return true;},error); };
        QVERIFY(server.listen(QHostAddress::LocalHost,0,dir.path(),QMap<QString,QString>{{"u","p"}}));
        QTcpSocket client; client.connectToHost(QHostAddress::LocalHost,server.port()); QVERIFY(reply(client).startsWith("220"));
        QVERIFY(command(client,"USER u").startsWith("331")); QVERIFY(command(client,"PASS p").startsWith("230"));
        QString epsv=command(client,"EPSV"); const auto port=epsv.section('|',3,3).toUShort(); QVERIFY(port);
        QTcpSocket data; data.connectToHost(QHostAddress::LocalHost,port);
        QTRY_COMPARE(data.state(),QAbstractSocket::ConnectedState);
        auto *session = server.findChild<FtpControlConnection*>(); QVERIFY(session);
        QTRY_VERIFY(session->findChild<QTcpSocket*>());
        QCOMPARE(session->findChild<QTcpSocket*>()->readBufferSize(),qint64(256*1024));
        QVERIFY(command(client,"STOR upload-sample.png").startsWith("150"));
        QImage image(8,8,QImage::Format_RGB32); image.fill(Qt::red); QByteArray bytes; QBuffer buffer(&bytes); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer,"PNG"));
        data.write(bytes); data.disconnectFromHost();
        QVERIFY2(reply(client).startsWith("226"),qPrintable(error)); QVERIFY(QFile::exists(imageName(dir.path())));
        server.setUsers({}); QTRY_COMPARE(server.clientCount(),0);
        server.stop();
    }
    void ftpIngestServiceUploadsAndRecoversOnWorkerThread() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        // 上次异常退出遗留的恢复标记：图片已完整落盘，恢复扫描应补发入库通知并清除标记
        const QString recovered = dir.filePath("recovered.png");
        QVERIFY(makeImage(recovered));
        QFile marker(recovered + ".cv-pending");
        QVERIFY(marker.open(QIODevice::WriteOnly)); marker.write("x"); marker.close();

        QThread thread;
        auto *service = new FtpIngestService;
        service->moveToThread(&thread);
        connect(&thread, &QThread::finished, service, &QObject::deleteLater);
        QStringList stored, failures;
        std::atomic<QThread *> emitThread{nullptr};
        connect(service, &FtpIngestService::imageStored, service,
                [&emitThread](const QString &) { emitThread = QThread::currentThread(); }, Qt::DirectConnection);
        connect(service, &FtpIngestService::imageStored, this,
                [&stored](const QString &path) { stored << QFileInfo(path).fileName(); });
        connect(service, &FtpIngestService::ingestFailed, this,
                [&failures](const QString &error) { failures << error; });
        thread.start();

        quint16 port = 0;
        const QString root = dir.path();
        QMetaObject::invokeMethod(service, [service, root, &port]() {
            service->setRoot(root);
            if (service->start(0, root, {{"u", "p"}}, false, QHostAddress::LocalHost)) port = service->port();
        }, Qt::BlockingQueuedConnection);
        QVERIFY(port);

        QTRY_VERIFY(stored.contains("recovered.png"));
        QVERIFY(!QFile::exists(recovered + ".cv-pending"));

        QTcpSocket client; client.connectToHost(QHostAddress::LocalHost, port);
        QVERIFY(reply(client).startsWith("220"));
        QVERIFY(command(client, "USER u").startsWith("331"));
        QVERIFY(command(client, "PASS p").startsWith("230"));
        auto upload = [&](const QByteArray &name, const QByteArray &bytes) {
            const QString epsv = command(client, "EPSV");
            QTcpSocket data; data.connectToHost(QHostAddress::LocalHost, epsv.section('|', 3, 3).toUShort());
            if (!data.waitForConnected(3000)) return QString();
            const QString started = command(client, "STOR " + name);
            if (!started.startsWith("150")) return started;
            data.write(bytes); data.flush(); data.disconnectFromHost();
            return reply(client);
        };
        QImage image(8, 8, QImage::Format_RGB32); image.fill(Qt::red);
        QByteArray png; QBuffer buffer(&png); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer, "PNG"));

        QVERIFY(upload("upload-sample.png", png).startsWith("226"));
        QTRY_VERIFY(stored.contains("upload-sample.png"));
        QVERIFY(!QFile::exists(imageName(root) + ".cv-pending"));
        QCOMPARE(emitThread.load(), &thread);

        QVERIFY(!upload("broken.png", "not an image").startsWith("226"));
        QTRY_VERIFY(!failures.isEmpty());
        QVERIFY(!stored.contains("broken.png"));

        QMetaObject::invokeMethod(service, [service]() { service->shutdown(); }, Qt::BlockingQueuedConnection);
        thread.quit();
        QVERIFY(thread.wait(5000));
    }
    void databaseDefaultsAndHelpers() {
        QCOMPARE(DBSchema::getConfig(db, "ftp/rootDirectory", ""), QString("images"));
        QCOMPARE(DBSchema::getConfigInt(db, "cleanup/keepDays", 0), 90);
        QCOMPARE(DBSchema::getConfigBool(db, "ui/isDark", true), false);

        QVERIFY(DBSchema::setConfig(db, "ui/isDark", "true"));
        QCOMPARE(DBSchema::getConfigBool(db, "ui/isDark", false), true);
        QVERIFY(DBSchema::setConfig(db, "ui/isDark", "false"));

        // verify ftp_users
        QSqlQuery q(db);
        QVERIFY(q.exec("SELECT username, password_hash FROM ftp_users"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(), QString("agc"));
        QVERIFY(AgcUtils::verifyEncodedPassword("123456", q.value(1).toString()));

        // verify camera_slots
        QVERIFY(q.exec("SELECT COUNT(*) FROM camera_slots"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 12);

        // verify carrier_wheel_norm (50 carriers * 16 wheels)
        QVERIFY(q.exec("SELECT COUNT(*) FROM carrier_wheel_norm"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 800);
    }
    void defaultDatabasePathIsUnderDataDirectory() {
        QString dbPath = DBSchema::defaultDatabasePath();
        QVERIFY(dbPath.endsWith("data/dataAgc.db") || dbPath.endsWith("data\\dataAgc.db"));
    }
    void carrierWheelNormDatabaseOperations() {
        QSqlQuery q(db);
        q.prepare("UPDATE carrier_wheel_norm SET standard_distance = 150.0, lower_tolerance = -2.0 WHERE carrier_id = 5 AND wheel_id = 3");
        QVERIFY(q.exec());

        q.prepare("SELECT standard_distance, lower_tolerance FROM carrier_wheel_norm WHERE carrier_id = 5 AND wheel_id = 3");
        QVERIFY(q.exec());
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toDouble(), 150.0);
        QCOMPARE(q.value(1).toDouble(), -2.0);
    }
    void recordBatchPersistence() {
        ImageIngest::BatchData batch;
        batch.carrierId = 1;
        batch.timestamp = QDateTime::fromString("2026-09-05 12:00:00", "yyyy-MM-dd HH:mm:ss");
        batch.batchId = "BATCH_1_2026-09-05 12:00:00";
        ImageIngest::WheelItem wheel;
        wheel.wheelId = 1;
        wheel.cameraId = 1;
        wheel.minValue = 12.5;
        wheel.maxValue = 13.25;
        wheel.baseValue = 10.0;
        wheel.deviation = 3.25;
        wheel.result = 1; // OK
        wheel.imageName = "20260905_120000_w1.png";
        batch.wheels.append(wheel);

        QString error;
        QVERIFY(ImageIngest::recordBatch(db, batch, error));

        QSqlQuery q(db);
        QVERIFY(q.exec("SELECT carrier_id, wheel_id, distance, dist_norm, min_value, max_value, deviation, result, batch_id "
                       "FROM record WHERE imagename = '20260905_120000_w1.png'"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 1);
        QCOMPARE(q.value(1).toInt(), 1);
        QCOMPARE(q.value(2).toDouble(), 12.5); // distance 保留为最小实测值
        QCOMPARE(q.value(3).toDouble(), 10.0); // dist_norm 为基准值
        QCOMPARE(q.value(4).toDouble(), 12.5);
        QCOMPARE(q.value(5).toDouble(), 13.25);
        QCOMPARE(q.value(6).toDouble(), 3.25);
        QCOMPARE(q.value(7).toInt(), 1);
        QCOMPARE(q.value(8).toString(), QString("BATCH_1_2026-09-05 12:00:00"));

        // verify index exists
        QVERIFY(q.exec("SELECT name FROM sqlite_master WHERE type='index' AND name='idx_record_batch'"));
        QVERIFY(q.next());
    }
    // 视觉端报文示例（README 中同一条）：驱动轮图片名带最小/最大/基准/偏差/结果
    static QJsonObject visionExampleBatch() {
        return QJsonDocument::fromJson(R"({"carrierId":34,"timestamp":"20261116_142451","wheels":[)"
            R"({"wheelId":9,"cameraId":5,"minValue":118.632,"maxValue":151.107,"baseValue":155.000,"deviation":-3.893,)"
            R"("result":"OK","imageName":"CARRIER_34_20261116_142451_cam05_118.6_151.1_155.0_-3.9_OK.jpg"}]})").object();
    }
    void visionMeasurementFieldsParseRejectAndLegacyFallback() {
        ImageIngest::BatchData batch;
        QString error;
        QJsonObject root = visionExampleBatch();
        QVERIFY2(ImageIngest::parseBatchJson(root, batch, error), qPrintable(error));
        QCOMPARE(batch.carrierId, 34);
        QCOMPARE(batch.wheels.size(), 1);
        const ImageIngest::WheelItem &w = batch.wheels.first();
        QCOMPARE(w.wheelId, 9);
        QCOMPARE(w.cameraId, 5);
        QCOMPARE(w.minValue, 118.632);
        QCOMPARE(w.maxValue, 151.107);
        QCOMPARE(w.baseValue, 155.0);
        QCOMPARE(w.deviation, -3.893);
        QCOMPARE(w.result, 1);
        QVERIFY(!w.legacyProtocol);
        QCOMPARE(w.imageName, QStringLiteral("CARRIER_34_20261116_142451_cam05_118.6_151.1_155.0_-3.9_OK.jpg"));

        const QJsonObject exampleWheel = root.value("wheels").toArray().first().toObject();
        auto parseWheel = [&](const QJsonObject &wheel) {
            QJsonObject r = root; r["wheels"] = QJsonArray{wheel};
            return ImageIngest::parseBatchJson(r, batch, error);
        };
        // 偏差值按客户端上报原样保存，服务端不重算
        QJsonObject inconsistent = exampleWheel; inconsistent["deviation"] = 9.5;
        QVERIFY(parseWheel(inconsistent));
        QCOMPARE(batch.wheels.first().deviation, 9.5);

        // 新字段任一缺失、为 null、非数值都拒收，错误信息指明字段
        for (const char *field : {"minValue", "maxValue", "baseValue", "deviation"}) {
            QJsonObject missing = exampleWheel; missing.remove(field);
            QVERIFY(!parseWheel(missing));
            QVERIFY2(error.contains(QLatin1String(field)) && error.contains(QStringLiteral("缺少字段")), qPrintable(error));
            QJsonObject nulled = exampleWheel; nulled[field] = QJsonValue::Null;
            QVERIFY(!parseWheel(nulled));
            QJsonObject text = exampleWheel; text[field] = "1.0";
            QVERIFY(!parseWheel(text));
            QVERIFY2(error.contains(QLatin1String(field)) && error.contains(QStringLiteral("有限数值")), qPrintable(error));
        }
        // 只带部分新字段时不走旧协议兼容
        QJsonObject mixed = exampleWheel; mixed.remove("maxValue"); mixed["actualDistance"] = 120.0;
        QVERIFY(!parseWheel(mixed));
        QVERIFY(error.contains("maxValue"));

        // 旧协议过渡兼容：最小=最大=actualDistance，基准=baseDistance，偏差值为空
        QJsonObject legacy{{"wheelId", 9}, {"cameraId", 5}, {"actualDistance", 118.5}, {"baseDistance", 120.0},
                           {"lowerTolerance", -1.5}, {"result", "NG"}, {"imageName", "legacy.png"}};
        QVERIFY2(parseWheel(legacy), qPrintable(error));
        const ImageIngest::WheelItem lw = batch.wheels.first();
        QVERIFY(lw.legacyProtocol);
        QCOMPARE(lw.minValue, 118.5);
        QCOMPARE(lw.maxValue, 118.5);
        QCOMPARE(lw.baseValue, 120.0);
        QVERIFY(std::isnan(lw.deviation));
        QVERIFY(ImageIngest::recordBatch(db, batch, error));
        QSqlQuery q(db);
        QVERIFY(q.exec("SELECT distance, min_value, max_value, dist_norm, deviation, lower_tolerance FROM record WHERE imagename='legacy.png'"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toDouble(), 118.5);
        QCOMPARE(q.value(1).toDouble(), 118.5);
        QCOMPARE(q.value(2).toDouble(), 118.5);
        QCOMPARE(q.value(3).toDouble(), 120.0);
        QVERIFY(q.value(4).isNull());
        QCOMPARE(q.value(5).toDouble(), -1.5);
        // NG 进入报警缓存，偏差值同样为空
        QVERIFY(q.exec("SELECT min_value, deviation FROM alertrecord WHERE carrier_id=34 AND wheel_id=9"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toDouble(), 118.5);
        QVERIFY(q.value(1).isNull());

        QJsonObject legacyBad = legacy; legacyBad["actualDistance"] = "118.5";
        QVERIFY(!parseWheel(legacyBad));
        QVERIFY(error.contains("actualDistance"));
    }
    // 驱动轮图片名含小数点、负号和多段下划线：FTP 先到与 TCP 先到两种顺序都按文件名关联
    void visionExampleImageAssociatesInBothArrivalOrders() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        FtpServer server; QString error; QStringList stored;
        server.setDateSubdirectories(true);
        server.uploadHandler = [&](const QString &staged, const QString &target, const QString &) {
            return ImageIngest::accept(staged, target, [](const QString &) { return true; }, error);
        };
        connect(&server, &FtpServer::imageStored, this, [&stored](const QString &path) { stored << path; });
        QVERIFY(server.listen(QHostAddress::LocalHost, 0, dir.path(), QMap<QString, QString>{{"u", "p"}}));
        QTcpSocket client; client.connectToHost(QHostAddress::LocalHost, server.port());
        QVERIFY(reply(client).startsWith("220"));
        QVERIFY(command(client, "USER u").startsWith("331"));
        QVERIFY(command(client, "PASS p").startsWith("230"));
        QImage image(16, 16, QImage::Format_RGB32); image.fill(Qt::gray);
        QByteArray jpg; QBuffer buffer(&jpg); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer, "JPG"));
        auto upload = [&](const QString &name) {
            const QString epsv = command(client, "EPSV");
            QTcpSocket data; data.connectToHost(QHostAddress::LocalHost, epsv.section('|', 3, 3).toUShort());
            if (!data.waitForConnected(3000)) return QString();
            const QString started = command(client, "STOR " + name.toUtf8());
            if (!started.startsWith("150")) return started;
            data.write(jpg); data.flush(); data.disconnectFromHost();
            return reply(client);
        };

        // FTP 先到：图片进入待关联缓存，TCP 批次按 imageName（大小写不敏感）取用
        const QString name = QStringLiteral("CARRIER_34_20261116_142451_cam05_118.6_151.1_155.0_-3.9_OK.jpg");
        QVERIFY2(upload(name).startsWith("226"), qPrintable(error));
        QTRY_COMPARE(stored.size(), 1);
        QCOMPARE(QFileInfo(stored.first()).fileName(), name); // 文件名原样落盘，不截断小数点或负号后的部分
        IngestCoordinator::PendingImageCache cache;
        cache.add(name, stored.first(), QDateTime::currentDateTime());
        ImageIngest::BatchData batch;
        QJsonObject root = visionExampleBatch();
        root.remove("timestamp");
        QVERIFY2(ImageIngest::parseBatchJson(root, batch, error), qPrintable(error));
        batch.wheels.first().imageName = name.toLower();
        batch.wheels.first().imagePath = cache.find(batch.wheels.first().imageName);
        QCOMPARE(batch.wheels.first().imagePath, stored.first());
        QVERIFY(ImageIngest::recordBatch(db, batch, error));
        QSqlQuery q(db);
        QVERIFY(q.exec("SELECT image_path, min_value, max_value, dist_norm, deviation FROM record WHERE wheel_id=9"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(), stored.first());
        QCOMPARE(q.value(4).toDouble(), -3.893);

        // TCP 先到：记录入库时没有图片，图片到达后按请求文件名补写路径
        const QString second = QStringLiteral("CARRIER_34_20261116_142520_cam06_-0.5_151.1_155.0_-3.9_NG.jpg");
        QJsonObject wheel = root.value("wheels").toArray().first().toObject();
        wheel["wheelId"] = 10; wheel["cameraId"] = 6; wheel["result"] = "NG"; wheel["imageName"] = second;
        root["wheels"] = QJsonArray{wheel};
        QVERIFY(ImageIngest::parseBatchJson(root, batch, error));
        QVERIFY(ImageIngest::recordBatch(db, batch, error));
        QVERIFY2(upload(second).startsWith("226"), qPrintable(error));
        QTRY_COMPARE(stored.size(), 2);
        QCOMPARE(QFileInfo(stored.last()).fileName(), second);
        QCOMPARE(ImageIngest::linkRecordImage(db, second, stored.last(), QDateTime::currentDateTime().addSecs(-3600), error), 1);
        QVERIFY(q.exec("SELECT image_path FROM record WHERE wheel_id=10"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(), stored.last());
        QCOMPARE(ImageIngest::linkRecordImage(db, second, stored.last(), QDateTime::currentDateTime().addSecs(-3600), error), 0);

        // 超过 240 字符的文件名在 FTP 端直接拒收，不会留下无法提交的 .cv-pending 标记
        QVERIFY(upload(QString(237, u'a') + ".jpg").startsWith("553"));
    }
    // 读取存储方式 ZIP 的全部条目，同时校验本地头、中央目录与 CRC 一致
    static QMap<QString, QByteArray> readStoredZip(const QByteArray &zip) {
        QMap<QString, QByteArray> files;
        auto u16 = [&](qsizetype at) { return qFromLittleEndian<quint16>(zip.constData() + at); };
        auto u32 = [&](qsizetype at) { return qFromLittleEndian<quint32>(zip.constData() + at); };
        if (zip.size() < 22) return files;
        const qsizetype eocd = zip.size() - 22;
        if (u32(eocd) != 0x06054b50u) return files;
        const int count = u16(eocd + 10);
        qsizetype cd = u32(eocd + 16);
        for (int i = 0; i < count; ++i) {
            if (cd + 46 > zip.size() || u32(cd) != 0x02014b50u) return {};
            const quint32 crc = u32(cd + 16), size = u32(cd + 20);
            const quint16 nameLen = u16(cd + 28), extraLen = u16(cd + 30), commentLen = u16(cd + 32);
            const quint32 local = u32(cd + 42);
            const QString name = QString::fromUtf8(zip.mid(cd + 46, nameLen));
            if (u32(local) != 0x04034b50u || u16(local + 8) != 0 || u32(local + 14) != crc) return {};
            const qsizetype dataAt = local + 30 + u16(local + 26) + u16(local + 28);
            const QByteArray data = zip.mid(dataAt, size);
            if (ZipWriter::crc32(data) != crc) return {};
            files.insert(name, data);
            cd += 46 + nameLen + extraLen + commentLen;
        }
        return files;
    }
    void logExportCsvEscapingAndXlsxPackage() {
        // CRC32 标准校验值
        QCOMPARE(ZipWriter::crc32(QByteArray("123456789")), 0xCBF43926u);
        QCOMPARE(ZipWriter::crc32(QByteArray()), 0u);

        // CSV 转义：逗号、引号、换行加引号；公式前缀加 '；负数保持原样
        QCOMPARE(LogExport::csvField("plain"), QString("plain"));
        QCOMPARE(LogExport::csvField("a,b"), QString("\"a,b\""));
        QCOMPARE(LogExport::csvField("say \"hi\""), QString("\"say \"\"hi\"\"\""));
        QCOMPARE(LogExport::csvField("line1\nline2"), QString("\"line1\nline2\""));
        QCOMPARE(LogExport::csvField("=HYPERLINK(\"x\")"), QString("\"'=HYPERLINK(\"\"x\"\")\""));
        QCOMPARE(LogExport::csvField("+1"), QString("'+1"));
        QCOMPARE(LogExport::csvField("@SUM"), QString("'@SUM"));
        QCOMPARE(LogExport::csvField("-3.9"), QString("-3.9"));
        QCOMPARE(LogExport::csvField("-cmd"), QString("'-cmd"));

        // 报警处置日志拆列
        const QString handled = QStringLiteral("报警处置完成，操作员=张三，架号=12，轮位=3，操作=误报，日期=2026-11-01 至 2026-11-16，更新记录=4");
        QCOMPARE(LogExport::dispositionColumns(handled),
                 (QStringList{"报警处置完成", "张三", "12", "3", "误报", "2026-11-01 至 2026-11-16", "4"}));
        QCOMPARE(LogExport::dispositionColumns(QStringLiteral("报警处置被拒绝，架号=7，轮位=2：请填写处置人员")).value(0),
                 QStringLiteral("报警处置被拒绝"));

        LogExport::Table table = LogExport::makeTable(true);
        LogExport::appendLogRow(table, true, "2026-11-16 09:00:00.000", "info", handled);
        LogExport::appendLogRow(table, true, "2026-11-16 09:05:00.000", "warn",
                                QStringLiteral("报警处置完成，操作员==1+1 <李四> & \"王五\"，架号=5，轮位=9，操作=已处理（已换轮），日期=2026-11-16 至 2026-11-16，更新记录=1"));
        QCOMPARE(table.rows.first().size(), table.header.size());

        const QByteArray csv = LogExport::toCsv(table);
        QVERIFY(csv.startsWith("\xEF\xBB\xBF"));
        const QStringList lines = QString::fromUtf8(csv.mid(3)).split(QStringLiteral("\r\n"), Qt::SkipEmptyParts);
        QCOMPARE(lines.size(), 3);
        QVERIFY(lines.at(0).startsWith(QStringLiteral("时间,级别,事件,操作员,架号,轮位,操作,日期范围,更新记录,内容")));
        QVERIFY(lines.at(1).startsWith(QStringLiteral("2026-11-16 09:00:00.000,INFO,报警处置完成,张三,12,3,误报,")));
        QVERIFY(lines.at(2).contains(QStringLiteral(",\"'=1+1 <李四> & \"\"王五\"\"\",")));

        QString error;
        const QByteArray xlsx = LogExport::toXlsx(table, QStringLiteral("报警处置"), &error);
        QVERIFY2(!xlsx.isEmpty(), qPrintable(error));
        const QMap<QString, QByteArray> parts = readStoredZip(xlsx);
        for (const char *name : {"[Content_Types].xml", "_rels/.rels", "xl/workbook.xml", "xl/_rels/workbook.xml.rels",
                                 "xl/styles.xml", "xl/worksheets/sheet1.xml"}) {
            QVERIFY2(parts.contains(QLatin1String(name)), name);
            QXmlStreamReader xml(parts.value(QLatin1String(name)));
            while (!xml.atEnd()) xml.readNext();
            QVERIFY2(!xml.hasError(), qPrintable(QStringLiteral("%1: %2").arg(name, xml.errorString())));
        }
        // 单元格内容：特殊字符转义后可原样读回，数值列写为数值
        QXmlStreamReader sheet(parts.value("xl/worksheets/sheet1.xml"));
        QMap<QString, QString> cells;
        QString ref;
        while (!sheet.atEnd()) {
            sheet.readNext();
            if (!sheet.isStartElement()) continue;
            if (sheet.name() == QLatin1String("c")) ref = sheet.attributes().value("r").toString();
            else if (sheet.name() == QLatin1String("t") || sheet.name() == QLatin1String("v")) cells.insert(ref, sheet.readElementText());
        }
        QCOMPARE(cells.value("A1"), QStringLiteral("时间"));
        QCOMPARE(cells.value("D2"), QStringLiteral("张三"));
        QCOMPARE(cells.value("E2"), QStringLiteral("12"));
        QCOMPARE(cells.value("D3"), QStringLiteral("=1+1 <李四> & \"王五\""));
        QVERIFY(parts.value("xl/worksheets/sheet1.xml").contains("<c r=\"E2\"><v>12</v></c>"));
        QVERIFY(parts.value("xl/workbook.xml").contains(QStringLiteral("name=\"报警处置\"").toUtf8()));

        // 写文件后能完整读回
        QTemporaryDir dir;
        const QString path = dir.filePath("sub/export.xlsx");
        QVERIFY2(LogExport::writeFile(path, xlsx, error), qPrintable(error));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), xlsx);
    }
    void logExportVisitsAllMatchingRowsAcrossDays() {
        QTemporaryDir dir;
        AppLogger::init(dir.path()); AppLogger::shutdown();
        const QList<QPair<QString, QString>> files{
            {"carrier_2025-02-01.log", "[2025-02-01 09:00:00.000] [info] 报警处置完成，操作员=甲，架号=1，轮位=1，操作=误报，日期=2025-02-01 至 2025-02-01，更新记录=1\n"
                                       "[2025-02-01 09:01:00.000] [info] 其他日志\n"},
            {"carrier_2025-02-02.log", "[2025-02-02 09:00:00.000] [warning] 报警处置被拒绝，架号=2，轮位=3：请填写处置人员\n"},
        };
        for (const auto &entry : files) {
            QFile file(dir.filePath(entry.first));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(entry.second.toUtf8());
        }
        QStringList times;
        QVERIFY(AppLogger::forEachLog("2025-02-02", "2025-02-01", "all", "", "alertDisposition",
            [&](const QString &time, const QString &, const QString &, const QString &) { times << time; return true; }));
        QCOMPARE(times, (QStringList{"2025-02-01 09:00:00.000", "2025-02-02 09:00:00.000"})); // 按时间正序，不分页
        times.clear();
        QVERIFY(AppLogger::forEachLog("2025-02-01", "2025-02-02", "warn", "", "alertDisposition",
            [&](const QString &time, const QString &, const QString &, const QString &) { times << time; return true; }));
        QCOMPARE(times.size(), 1);
        QString error;
        QVERIFY(!AppLogger::forEachLog("bad", "2025-02-02", "all", "", "all",
            [](const QString &, const QString &, const QString &, const QString &) { return true; }, &error));
        QVERIFY(!error.isEmpty());
    }
    void tcpDataReceiverPacketParsing() {
        TcpDataReceiver receiver;
        QVERIFY(receiver.listen(QHostAddress::LocalHost, 0));
        const quint16 port = receiver.port();

        bool received = false;
        ImageIngest::BatchData receivedBatch;
        connect(&receiver, &TcpDataReceiver::dataBatchReceived, [&](const QJsonObject &json) {
            QString err;
            if (ImageIngest::parseBatchJson(json, receivedBatch, err)) {
                received = true;
            }
        });

        QTcpSocket client;
        client.connectToHost(QHostAddress::LocalHost, port);
        QVERIFY(client.waitForConnected(3000));

        QJsonObject root;
        root["carrierId"] = 3;
        root["timestamp"] = "20260911_120000";
        QJsonArray wheels;
        QJsonObject w;
        w["wheelId"] = 2;
        w["minValue"] = 11.2;
        w["maxValue"] = 11.6;
        w["baseValue"] = 10.0;
        w["deviation"] = 1.6;
        w["result"] = "OK";
        w["imageName"] = "test_w2.png";
        wheels.append(w);
        root["wheels"] = wheels;

        QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Compact) + "\n";
        client.write(data);
        client.flush();
        QTRY_VERIFY_WITH_TIMEOUT(client.bytesAvailable() > 0, 3000);
        QByteArray ack = client.readLine();
        QVERIFY(ack.contains("\"status\":\"OK\""));
        QVERIFY(received);
        QCOMPARE(receivedBatch.carrierId, 3);
        QCOMPARE(receivedBatch.wheels.size(), 1);
        QCOMPARE(receivedBatch.wheels[0].wheelId, 2);
        QCOMPARE(receivedBatch.wheels[0].minValue, 11.2);
        QCOMPARE(receivedBatch.wheels[0].maxValue, 11.6);
        QCOMPARE(receivedBatch.wheels[0].deviation, 1.6);

        receiver.stop();
    }
    void tcpReceiverOnlyAcknowledgesSuccessfulCommit() {
        TcpDataReceiver receiver;
        receiver.batchHandler = [](const QJsonObject &, QString &error) {
            error = QStringLiteral("database unavailable");
            return false;
        };
        QVERIFY(receiver.listen(QHostAddress::LocalHost, 0));
        QTcpSocket client;
        client.connectToHost(QHostAddress::LocalHost, receiver.port());
        QVERIFY(client.waitForConnected(3000));
        QJsonObject root{{"carrierId", 3}, {"wheels", QJsonArray{QJsonObject{{"wheelId", 1}, {"result", "OK"}}}}};
        client.write(QJsonDocument(root).toJson(QJsonDocument::Compact) + '\n');
        QTRY_VERIFY_WITH_TIMEOUT(client.canReadLine(), 3000);
        const QJsonObject response = QJsonDocument::fromJson(client.readLine()).object();
        QCOMPARE(response.value("status").toString(), QString("ERROR"));
        QVERIFY(response.value("message").toString().contains("database"));
        receiver.stop();
    }
    void ftpStoresRootUploadsInDateSubdirectory() {
        // 传到根目录的图片按接收日期分子目录落盘，客户端指定的子目录不变
        QTemporaryDir dir; FtpServer server; QString error; QStringList stored;
        server.setDateSubdirectories(true);
        server.uploadHandler = [&](const QString &staged, const QString &target, const QString &) {
            return ImageIngest::accept(staged, target, [](const QString&){ return true; }, error);
        };
        connect(&server, &FtpServer::imageStored, this, [&stored](const QString &path) { stored << path; });
        QVERIFY(server.listen(QHostAddress::LocalHost, 0, dir.path(), QMap<QString,QString>{{"u","p"}}));
        QVERIFY(QDir().mkpath(dir.filePath("cam1")));
        QTcpSocket client; client.connectToHost(QHostAddress::LocalHost, server.port());
        QVERIFY(reply(client).startsWith("220"));
        QVERIFY(command(client, "USER u").startsWith("331"));
        QVERIFY(command(client, "PASS p").startsWith("230"));
        QImage image(8, 8, QImage::Format_RGB32); image.fill(Qt::red);
        QByteArray png; QBuffer buffer(&png); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer, "PNG"));
        auto upload = [&](const QByteArray &name) {
            const QString epsv = command(client, "EPSV");
            QTcpSocket data; data.connectToHost(QHostAddress::LocalHost, epsv.section('|', 3, 3).toUShort());
            if (!data.waitForConnected(3000)) return QString();
            const QString started = command(client, "STOR " + name);
            if (!started.startsWith("150")) return started;
            data.write(png); data.flush(); data.disconnectFromHost();
            return reply(client);
        };
        const QString day = QDate::currentDate().toString("yyyy-MM-dd");
        QVERIFY2(upload("dated.png").startsWith("226"), qPrintable(error));
        QVERIFY(QFile::exists(dir.filePath(day + "/dated.png")));
        QVERIFY(!QFile::exists(dir.filePath("dated.png")));
        QVERIFY2(upload("cam1/kept.png").startsWith("226"), qPrintable(error));
        QVERIFY(QFile::exists(dir.filePath("cam1/kept.png")));
        QTRY_COMPARE(stored.size(), 2);
        QVERIFY(stored.first().endsWith(day + "/dated.png"));
        server.stop();
    }
    void collisionSafeFtpUpload() {
        QTemporaryDir dir; FtpServer server; QString error;
        server.uploadHandler = [&](const QString &staged, const QString &target, const QString &) {
            return ImageIngest::accept(staged, target, [](const QString&){ return true; }, error);
        };
        QVERIFY(server.listen(QHostAddress::LocalHost, 0, dir.path(), QMap<QString,QString>{{"u","p"}}));
        QTcpSocket client; client.connectToHost(QHostAddress::LocalHost, server.port());
        QVERIFY(reply(client).startsWith("220"));
        QVERIFY(command(client, "USER u").startsWith("331"));
        QVERIFY(command(client, "PASS p").startsWith("230"));

        // First upload: red image
        QString epsv = command(client, "EPSV");
        QTcpSocket data1; data1.connectToHost(QHostAddress::LocalHost, epsv.section('|', 3, 3).toUShort());
        QTRY_COMPARE(data1.state(), QAbstractSocket::ConnectedState);
        QVERIFY(command(client, "STOR collision-sample.png").startsWith("150"));
        QImage redImg(8, 8, QImage::Format_RGB32); redImg.fill(Qt::red);
        QByteArray redBytes; QBuffer buf1(&redBytes); buf1.open(QIODevice::WriteOnly); redImg.save(&buf1, "PNG");
        data1.write(redBytes); data1.disconnectFromHost();
        QVERIFY2(reply(client).startsWith("226"), qPrintable(error));
        const QString path1 = dir.filePath("collision-sample.png");
        QVERIFY(QFile::exists(path1));

        // Second upload with same name but different content: blue image
        epsv = command(client, "EPSV");
        QTcpSocket data2; data2.connectToHost(QHostAddress::LocalHost, epsv.section('|', 3, 3).toUShort());
        QTRY_COMPARE(data2.state(), QAbstractSocket::ConnectedState);
        QVERIFY(command(client, "STOR collision-sample.png").startsWith("150"));
        QImage blueImg(8, 8, QImage::Format_RGB32); blueImg.fill(Qt::blue);
        QByteArray blueBytes; QBuffer buf2(&blueBytes); buf2.open(QIODevice::WriteOnly); blueImg.save(&buf2, "PNG");
        data2.write(blueBytes); data2.disconnectFromHost();
        QVERIFY2(reply(client).startsWith("226"), qPrintable(error));

        // Both original and version-suffixed files must exist without data loss
        const QString path2 = dir.filePath("collision-sample_v2.png");
        QVERIFY(QFile::exists(path1));
        QVERIFY(QFile::exists(path2));
        QCOMPARE(QImage(path1).pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(QImage(path2).pixelColor(0, 0), QColor(Qt::blue));
        server.stop();
    }
    void loggerEscalationAndCallback() {
        QTemporaryDir dir;
        AppLogger::init(dir.path());
        int callbackCount = 0;
        QString lastLevel;
        QString lastMsg;
        AppLogger::setLogCallback([&](const QString &level, const QString &msg, const QString &/*time*/) {
            ++callbackCount;
            lastLevel = level;
            lastMsg = msg;
        });

        LOG_WARN("测试告警消息123");
        QCOMPARE(lastLevel, QStringLiteral("WARN"));
        QVERIFY(lastMsg.contains("测试告警消息123"));
        QVariantMap warnMap = AppLogger::latestWarningOrError();
        QCOMPARE(warnMap.value("level").toString(), QStringLiteral("WARN"));
        QVERIFY(warnMap.value("message").toString().contains("测试告警消息123"));

        LOG_ERROR("测试严重错误456");
        QCOMPARE(lastLevel, QStringLiteral("ERROR"));
        QVERIFY(lastMsg.contains("测试严重错误456"));
        QVariantMap errMap = AppLogger::latestWarningOrError();
        QCOMPARE(errMap.value("level").toString(), QStringLiteral("ERROR"));
        QVERIFY(errMap.value("message").toString().contains("测试严重错误456"));

        QCOMPARE(callbackCount, 2);
        qWarning("Qt告警只通知一次");
        QCOMPARE(callbackCount, 3);
        QCOMPARE(lastMsg, QStringLiteral("Qt告警只通知一次"));
        qCritical("Qt错误只通知一次");
        QCOMPARE(callbackCount, 4);
        QCOMPARE(lastLevel, QStringLiteral("ERROR"));
        QVERIFY(AppLogger::setLevel("error"));
        qWarning("已过滤的Qt告警");
        QCOMPARE(callbackCount, 4);

        AppLogger::clearLatestWarningOrError();
        QVERIFY(AppLogger::latestWarningOrError().isEmpty());
        AppLogger::setLogCallback(nullptr);
        AppLogger::shutdown();
    }
    void loggerRuntimeDebugAndTraceLevels() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        AppLogger::init(dir.path());
        QVERIFY(AppLogger::setLevel("info"));
        int evaluated = 0;
        LOG_DEBUG("被过滤的调试信息 {}", ++evaluated);
        LOG_TRACE("被过滤的跟踪信息 {}", ++evaluated);
        QCOMPARE(evaluated, 0);
        QVERIFY(AppLogger::setLevel("debug"));
        LOG_DEBUG("运行时调试明细 {}", ++evaluated);
        LOG_TRACE("不可见的跟踪明细 {}", ++evaluated);
        QCOMPARE(evaluated, 1);
        QVERIFY(AppLogger::setLevel("trace"));
        LOG_TRACE("运行时跟踪明细 {}", ++evaluated);
        QCOMPARE(evaluated, 2);
        AppLogger::logger()->flush();
        const QString date = QDate::currentDate().toString("yyyy-MM-dd");
        const auto debug = AppLogger::queryLogs(date, "debug", "运行时调试明细", 1, 20);
        const auto trace = AppLogger::queryLogs(date, "trace", "运行时跟踪明细", 1, 20);
        QCOMPARE(debug.value("total").toInt(), 1);
        QCOMPARE(trace.value("total").toInt(), 1);
        AppLogger::shutdown();
    }
    void loggerFiltersAlertDispositionCategory() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        AppLogger::init(dir.path());
        LOG_INFO("报警处置完成，操作员={}，架号={}，轮位={}", "张三", 12, 3);
        LOG_WARN("报警处置被拒绝，架号={}，轮位={}：{}", 7, 2, "处置人员不在系统设置的名单中");
        LOG_INFO("添加报警处置人员：{}", "张三");
        LOG_INFO("TCP 入库完成，架号={}", 12);
        AppLogger::logger()->flush();
        const QString date = QDate::currentDate().toString("yyyy-MM-dd");
        const auto alerts = AppLogger::queryLogs(date, "all", "", 1, 20, "alertDisposition");
        QCOMPARE(alerts.value("total").toInt(), 2);
        for (const auto &item : alerts.value("items").toList())
            QVERIFY(item.toMap().value("message").toString().startsWith(QStringLiteral("报警处置")));
        // 类型与级别、关键字同时生效
        QCOMPARE(AppLogger::queryLogs(date, "warn", "", 1, 20, "alertDisposition").value("total").toInt(), 1);
        QCOMPARE(AppLogger::queryLogs(date, "all", "张三", 1, 20, "alertDisposition").value("total").toInt(), 1);
        // 未指定类型或 all 时不过滤
        QVERIFY(AppLogger::queryLogs(date, "all", "", 1, 20).value("total").toInt() >= 4);
        QCOMPARE(AppLogger::queryLogs(date, "all", "", 1, 20, "all").value("total").toInt(),
                 AppLogger::queryLogs(date, "all", "", 1, 20).value("total").toInt());
        AppLogger::shutdown();
    }
    void tcpReceiverDisconnectsOverflowingClient() {
        TcpDataReceiver receiver;
        QVERIFY(receiver.listen(QHostAddress::LocalHost, 0));
        quint16 port = receiver.port();
        QTcpSocket client;
        client.connectToHost(QHostAddress::LocalHost, port);
        QVERIFY(client.waitForConnected(3000));
        QTRY_COMPARE(receiver.clientCount(), 1);

        // Send 2.5MB of data without any newline
        QByteArray junk(256 * 1024, 'X');
        for (int i = 0; i < 10; ++i) {
            client.write(junk);
            client.flush();
            QTest::qWait(10);
        }

        // The receiver should disconnect the overflowing client to protect system memory
        QTRY_COMPARE(receiver.clientCount(), 0);
        receiver.stop();
    }
    void tcpReceiverDropsIdleAndOversizedLines() {
        TcpDataReceiver receiver;
        receiver.setIdleTimeoutMs(300);
        receiver.batchHandler = [](const QJsonObject &, QString &) { return true; };
        QVERIFY(receiver.listen(QHostAddress::LocalHost, 0));

        // 空闲连接：超时后被断开
        QTcpSocket idle;
        idle.connectToHost(QHostAddress::LocalHost, receiver.port());
        QVERIFY(idle.waitForConnected(3000));
        QTRY_COMPARE(receiver.clientCount(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(receiver.clientCount(), 0, 3000);

        // 活跃连接持续发送有效报文时不被断开
        QTcpSocket active;
        active.connectToHost(QHostAddress::LocalHost, receiver.port());
        QVERIFY(active.waitForConnected(3000));
        const QByteArray ok = R"({"carrierId":1,"wheels":[{"wheelId":1,"result":"OK","imageName":"a.png"}]})" "\n";
        for (int i = 0; i < 6; ++i) {
            active.write(ok);
            QTRY_VERIFY(active.canReadLine());
            QVERIFY(active.readLine().contains("\"OK\""));
            QTest::qWait(100);
        }
        QCOMPARE(receiver.clientCount(), 1);

        // 单条报文超过 64 KB：断开
        QByteArray huge = "{\"carrierId\":1,\"wheels\":[],\"pad\":\"" + QByteArray(70 * 1024, 'x') + "\"}\n";
        active.write(huge);
        QTRY_COMPARE(receiver.clientCount(), 0);
        receiver.stop();
    }

    void thumbnailCacheBoundsSizeAndMemory() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString color = dir.filePath("large.jpg");
        QImage large(2000, 1000, QImage::Format_RGB32); large.fill(Qt::darkCyan);
        QVERIFY(large.save(color, "JPG"));
        const QString gray = dir.filePath("gray.png");
        QImage grayImage(1200, 900, QImage::Format_Grayscale8); grayImage.fill(128);
        QVERIFY(grayImage.save(gray, "PNG"));

        QCOMPARE(ThumbnailCache::edgeFor(QSize()), ThumbnailCache::kDefaultEdge);
        QCOMPARE(ThumbnailCache::edgeFor(QSize(200, 150)), 320);
        QCOMPARE(ThumbnailCache::edgeFor(QSize(5000, 100)), 640);

        // 解码直接得到缩略图尺寸；灰度图保持单通道
        const QImage thumb = ThumbnailCache::decode(color, 320);
        QCOMPARE(thumb.size(), QSize(320, 160));
        const QImage grayThumb = ThumbnailCache::decode(gray, 320);
        QCOMPARE(grayThumb.size(), QSize(320, 240));
        QCOMPARE(grayThumb.format(), QImage::Format_Grayscale8);
        QString error;
        QVERIFY(ThumbnailCache::decode(dir.filePath("missing.png"), 320, &error).isNull());
        QVERIFY(!error.isEmpty());

        // 缓存命中不重复占用；超过上限时淘汰旧缩略图
        ThumbnailCache cache(300 * 1024); // 一张 320x160 RGB32 缩略图 200KB
        QVERIFY(!cache.thumbnail(color, 320).isNull());
        QVERIFY(!cache.thumbnail(color, 320).isNull());
        QCOMPARE(cache.count(), 1);
        QVERIFY(!cache.thumbnail(color, 480).isNull());
        QVERIFY(cache.usedBytes() <= cache.maxBytes());
        QCOMPARE(cache.count(), 1);

        // 文件被覆盖后按新内容重新生成
        QTest::qWait(20);
        QImage replaced(1000, 1000, QImage::Format_RGB32); replaced.fill(Qt::red);
        QVERIFY(replaced.save(color, "JPG"));
        QCOMPARE(cache.thumbnail(color, 320).size(), QSize(320, 320));
        cache.clear();
        QCOMPARE(cache.count(), 0);
    }
};
QTEST_GUILESS_MAIN(ReleaseBoundaryTests)
#include "ReleaseBoundaryTests.moc"
