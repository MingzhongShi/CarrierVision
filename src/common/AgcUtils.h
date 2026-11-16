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

