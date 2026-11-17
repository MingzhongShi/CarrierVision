#pragma once

#include <QString>
#include <QVector>
#include <QDateTime>
#include <QJsonObject>
#include <QJsonArray>
#include <functional>
#include <QSqlDatabase>
#include <QVariantList>
#include <limits>

namespace ImageIngest {

struct WheelItem {
    int wheelId = 1;
    int cameraId = 1;
    double minValue = 0.0;   // 最小实测值
    double maxValue = 0.0;   // 最大实测值
    double baseValue = 0.0;  // 基准值（入库 dist_norm）
    // 偏差值 = maxValue - baseValue，由视觉端计算并原样入库，服务端不重算；
    // 旧协议报文没有该字段，为 NaN（入库为 NULL）
    double deviation = std::numeric_limits<double>::quiet_NaN();
    double lowerTolerance = 0.0; // 仅旧协议（actualDistance/lowerTolerance）兼容入库
    bool legacyProtocol = false;
    int result = 1; // 0=NG, 1=OK
    QString imageName;  // 规范化后的纯文件名
    QString imagePath;  // 已关联到的实际落盘文件（绝对路径），未关联时为空
};

struct BatchData {
    int carrierId = 0;
    QDateTime timestamp;
    QString batchId;
    QString requestId;
    QVector<WheelItem> wheels;
};

// 时间戳允许范围：now 无效时不限制；maxAgeDays 为 0 时不限制过去
struct ParseLimits {
    QDateTime now;
    int maxAgeDays = 0;
    int maxFutureSecs = 600;
};

bool parseBatchJson(const QJsonObject &obj, BatchData &out, QString &error, const ParseLimits &limits = {});
// 取 imageName 的文件名部分（兼容 / 与 \ 分隔）；空名、"."、".." 开头或非图片扩展名返回空串并给出原因
// strippedPath 非空时报告是否去掉了路径部分（由调用方汇总记录告警）
// 文件名超过 AgcUtils::kMaxImageNameLength 时同样拒绝
QString normalizeImageName(const QString &raw, QString &error, bool *strippedPath = nullptr);
// imageRoot 非空时，图像根目录内的 imagePath 以相对路径入库（见 AgcUtils::toImageRootRelative）
// duplicate 非空时报告本批是否为已入库 requestId 的重发（返回 true 但未写入任何记录）
bool recordBatch(QSqlDatabase db, const BatchData &batch, QString &error, const QString &imageRoot = QString(),
                 bool *duplicate = nullptr);
// 记录先于图片到达时补写实际落盘路径：只匹配 since 之后、尚未关联的同名记录（不区分大小写）。
// 返回补写的行数，失败返回 -1 并给出原因
int linkRecordImage(QSqlDatabase db, const QString &imageName, const QString &absolutePath,
                    const QDateTime &since, QString &error, const QString &imageRoot = QString());

bool validate(const QString &file, const QString &target, QString &error);
// 接收并归档上传图像
bool accept(const QString &staged, const QString &target,
            const std::function<bool(const QString &)> &ingest, QString &error);

}
