#include "common/ZipWriter.h"

#include <QtEndian>
#include <array>
#include <limits>

namespace {

const std::array<quint32, 256> &crcTable()
{
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> t{};
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    return table;
}

void put16(QByteArray &out, quint16 v)
{
    char buf[2];
    qToLittleEndian(v, buf);
    out.append(buf, 2);
}

void put32(QByteArray &out, quint32 v)
{
    char buf[4];
    qToLittleEndian(v, buf);
    out.append(buf, 4);
}

constexpr quint16 kVersion = 20;       // 2.0：存储方式即可解压
constexpr quint16 kFlagUtf8 = 0x0800;  // 文件名为 UTF-8
constexpr quint16 kMethodStore = 0;

} // namespace

ZipWriter::ZipWriter(const QDateTime &timestamp)
{
    // DOS 时间只能表示 1980 年以后
    QDateTime t = timestamp.isValid() ? timestamp : QDateTime::currentDateTime();
    if (t.date().year() < 1980) t = QDateTime(QDate(1980, 1, 1), QTime(0, 0));
    m_dosTime = static_cast<quint16>((t.time().hour() << 11) | (t.time().minute() << 5) | (t.time().second() / 2));
    m_dosDate = static_cast<quint16>(((t.date().year() - 1980) << 9) | (t.date().month() << 5) | t.date().day());
}

quint32 ZipWriter::crc32(const QByteArray &data)
{
    const auto &table = crcTable();
    quint32 c = 0xFFFFFFFFu;
    for (const char ch : data) c = table[(c ^ static_cast<quint8>(ch)) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void ZipWriter::addFile(const QString &name, const QByteArray &data)
{
    m_entries.append({name.toUtf8(), data, crc32(data)});
}

QByteArray ZipWriter::finish(QString *error) const
{
    constexpr qint64 kLimit = std::numeric_limits<quint32>::max();
    if (m_entries.size() > 0xFFFF) {
        if (error) *error = QStringLiteral("压缩包条目过多");
        return {};
    }
    qint64 estimated = 22;
    for (const Entry &e : m_entries) estimated += 30 + 46 + 2 * e.name.size() + e.data.size();
    if (estimated >= kLimit) {
        if (error) *error = QStringLiteral("导出内容超过 4 GB，请缩小查询范围");
        return {};
    }

    QByteArray out;
    out.reserve(static_cast<qsizetype>(estimated));
    QList<quint32> offsets;
    for (const Entry &e : m_entries) {
        offsets.append(static_cast<quint32>(out.size()));
        put32(out, 0x04034b50u);
        put16(out, kVersion);
        put16(out, kFlagUtf8);
        put16(out, kMethodStore);
        put16(out, m_dosTime);
        put16(out, m_dosDate);
        put32(out, e.crc);
        put32(out, static_cast<quint32>(e.data.size()));
        put32(out, static_cast<quint32>(e.data.size()));
        put16(out, static_cast<quint16>(e.name.size()));
        put16(out, 0);
        out.append(e.name);
        out.append(e.data);
    }

    const quint32 centralOffset = static_cast<quint32>(out.size());
    for (int i = 0; i < m_entries.size(); ++i) {
        const Entry &e = m_entries.at(i);
        put32(out, 0x02014b50u);
        put16(out, kVersion);
        put16(out, kVersion);
        put16(out, kFlagUtf8);
        put16(out, kMethodStore);
        put16(out, m_dosTime);
        put16(out, m_dosDate);
        put32(out, e.crc);
        put32(out, static_cast<quint32>(e.data.size()));
        put32(out, static_cast<quint32>(e.data.size()));
        put16(out, static_cast<quint16>(e.name.size()));
        put16(out, 0); // extra
        put16(out, 0); // comment
        put16(out, 0); // disk
        put16(out, 0); // internal attributes
        put32(out, 0); // external attributes
        put32(out, offsets.at(i));
        out.append(e.name);
    }
    const quint32 centralSize = static_cast<quint32>(out.size()) - centralOffset;

    put32(out, 0x06054b50u);
    put16(out, 0);
    put16(out, 0);
    put16(out, static_cast<quint16>(m_entries.size()));
    put16(out, static_cast<quint16>(m_entries.size()));
    put32(out, centralSize);
    put32(out, centralOffset);
    put16(out, 0);
    return out;
}
