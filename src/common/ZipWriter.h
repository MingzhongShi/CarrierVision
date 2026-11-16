#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>

// 最小 ZIP 写入器：只用存储方式（不压缩），用于生成 .xlsx 等 Office Open XML 包。
// 不支持 ZIP64，单个包总大小不超过 4 GB；文件名按 UTF-8 写入。
class ZipWriter
{
public:
    explicit ZipWriter(const QDateTime &timestamp = QDateTime::currentDateTime());

    void addFile(const QString &name, const QByteArray &data);
    // 生成完整的 ZIP 字节流；超过 4 GB 或条目过多时返回空并给出原因
    QByteArray finish(QString *error = nullptr) const;

    static quint32 crc32(const QByteArray &data);

private:
    struct Entry {
        QByteArray name;
        QByteArray data;
        quint32 crc = 0;
    };
    QList<Entry> m_entries;
    quint16 m_dosTime = 0;
    quint16 m_dosDate = 0;
};
