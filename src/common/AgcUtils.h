#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>

namespace AgcUtils {

QStringList splitSerials(const QString &rawText);
QString normalizedPath(const QString &path);
// 相对路径按程序所在目录解析（不依赖启动时的工作目录），返回规范化的绝对路径
QString resolveAppPath(const QString &path);
// 位于程序目录内的路径转为相对路径，便于整体搬迁安装目录；其余路径原样返回规范化绝对路径
QString toAppRelativePath(const QString &path);
// 记录中的图片路径（record.image_path）存为相对图像根目录的路径（"/" 分隔），便于整体搬迁图像目录。
// 不在 root 内的路径原样返回；Windows 下前缀比较不区分大小写
QString toImageRootRelative(const QString &path, const QString &root);
// record.image_path 转为绝对路径：相对路径按 root 解析，绝对路径（旧版写入）原样规范化；空值返回空串
QString imagePathFromRecord(const QString &stored, const QString &root);
QString formatDateTime(const QDateTime &dateTime);
QDateTime parseFlexibleDateTime(const QString &text, bool endOfDay = false);
bool isImageFile(const QString &fileName);
// 图片文件名长度上限：NTFS 单个路径分量最多 255 字符，FTP 提交时还会在文件名后追加
// ".cv-pending"（11 字符）恢复标记，并可能追加 "_vN" 防重名后缀，故留出余量取 240
constexpr int kMaxImageNameLength = 240;
QString makeBatchId(int roundNumber, const QDateTime &receivedAt);
QString safeNamePart(const QString &input);
QString encodePassword(const QString &password);
bool verifyEncodedPassword(const QString &password, const QString &stored);

} // namespace AgcUtils

