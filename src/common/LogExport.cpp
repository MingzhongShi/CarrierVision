#include "common/LogExport.h"
#include "common/ZipWriter.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>

namespace LogExport {

namespace {

const QStringList &dispositionKeys()
{
    static const QStringList keys{QStringLiteral("操作员"), QStringLiteral("架号"), QStringLiteral("轮位"),
                                  QStringLiteral("操作"), QStringLiteral("日期"), QStringLiteral("更新记录")};
    return keys;
}

// 列号转 Excel 列名：0 -> A，26 -> AA
QString columnName(int column)
{
    QString name;
    for (int n = column + 1; n > 0; n = (n - 1) / 26) name.prepend(QChar(u'A' + (n - 1) % 26));
    return name;
}

// XML 1.0 不允许的控制字符直接去掉
QString xmlEscape(const QString &text)
{
    QString out;
    out.reserve(text.size() + 8);
    for (const QChar ch : text) {
        const char16_t u = ch.unicode();
        if (u < 0x20 && u != u'\t' && u != u'\n' && u != u'\r') continue;
        if (u == 0xFFFE || u == 0xFFFF) continue;
        switch (u) {
        case u'&': out += QStringLiteral("&amp;"); break;
        case u'<': out += QStringLiteral("&lt;"); break;
        case u'>': out += QStringLiteral("&gt;"); break;
        case u'"': out += QStringLiteral("&quot;"); break;
        default: out += ch; break;
        }
    }
    return out;
}

bool isPlainNumber(const QString &text)
{
    static const QRegularExpression re(QStringLiteral("^-?\\d{1,15}(\\.\\d{1,15})?$"));
    return re.match(text).hasMatch();
}

} // namespace

QStringList dispositionHeader()
{
    return {QStringLiteral("时间"), QStringLiteral("级别"), QStringLiteral("事件"), QStringLiteral("操作员"),
            QStringLiteral("架号"), QStringLiteral("轮位"), QStringLiteral("操作"), QStringLiteral("日期范围"),
            QStringLiteral("更新记录"), QStringLiteral("内容")};
}

QStringList dispositionColumns(const QString &message)
{
    // 报警处置日志由 SearchService::alertOperation 等写入，字段以全角逗号分隔、键值以 = 连接
    QStringList columns(dispositionKeys().size() + 1);
    const QStringList parts = message.split(u'，');
    columns[0] = parts.value(0).trimmed();
    for (int i = 1; i < parts.size(); ++i) {
        const QString part = parts.at(i);
        const int eq = part.indexOf(u'=');
        if (eq <= 0) continue;
        const int index = dispositionKeys().indexOf(part.left(eq).trimmed());
        if (index >= 0) columns[index + 1] = part.mid(eq + 1).trimmed();
    }
    // "报警处置被拒绝，架号=7，轮位=2：原因" 的原因跟在冒号后，事件列只保留事件名
    const int colon = columns[0].indexOf(u'：');
    if (colon > 0) columns[0] = columns[0].left(colon);
    return columns;
}

Table makeTable(bool alertDisposition)
{
    Table table;
    if (alertDisposition) {
        table.header = dispositionHeader();
        table.numericColumns = {4, 5, 8};
    } else {
        table.header = {QStringLiteral("时间"), QStringLiteral("级别"), QStringLiteral("内容")};
    }
    return table;
}

void appendLogRow(Table &table, bool alertDisposition, const QString &time, const QString &level,
                  const QString &message)
{
    QStringList row{time, level.toUpper()};
    if (alertDisposition) row += dispositionColumns(message);
    row << message;
    table.rows.append(row);
}

QString csvField(const QString &value)
{
    QString text = value;
    if (!text.isEmpty()) {
        const QChar first = text.at(0);
        const bool formulaLike = first == u'=' || first == u'+' || first == u'@' || first == u'\t' || first == u'\r'
                                 || (first == u'-' && !isPlainNumber(text));
        if (formulaLike) text.prepend(u'\'');
    }
    if (text.contains(u',') || text.contains(u'"') || text.contains(u'\n') || text.contains(u'\r')) {
        text.replace(QStringLiteral("\""), QStringLiteral("\"\""));
        text = u'"' + text + u'"';
    }
    return text;
}

QByteArray toCsv(const Table &table)
{
    QString text;
    auto appendRow = [&text](const QStringList &row) {
        for (int i = 0; i < row.size(); ++i) {
            if (i > 0) text += u',';
            text += csvField(row.at(i));
        }
        text += QStringLiteral("\r\n");
    };
    appendRow(table.header);
    for (const QStringList &row : table.rows) appendRow(row);
    return QByteArray("\xEF\xBB\xBF") + text.toUtf8();
}

QByteArray toXlsx(const Table &table, const QString &sheetName, QString *error)
{
    if (table.rows.size() > kMaxXlsxRows) {
        if (error) *error = QStringLiteral("记录超过 Excel 单表上限 %1 行，请缩小查询范围或导出 CSV").arg(kMaxXlsxRows);
        return {};
    }
    const QString xmlHead = QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");
    const int columnCount = table.header.size();

    QString sheet = xmlHead;
    sheet += QStringLiteral("<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">");
    sheet += QStringLiteral("<dimension ref=\"A1:%1%2\"/>")
                 .arg(columnName(qMax(0, columnCount - 1))).arg(table.rows.size() + 1);
    // 冻结表头
    sheet += QStringLiteral("<sheetViews><sheetView workbookViewId=\"0\">"
                            "<pane ySplit=\"1\" topLeftCell=\"A2\" activePane=\"bottomLeft\" state=\"frozen\"/>"
                            "</sheetView></sheetViews>");
    sheet += QStringLiteral("<cols>");
    for (int c = 0; c < columnCount; ++c) {
        // 时间列与正文列加宽，其余按表头长度
        const bool last = c == columnCount - 1;
        const int width = c == 0 ? 24 : (last ? 80 : qMax(10, int(table.header.at(c).size()) * 3));
        sheet += QStringLiteral("<col min=\"%1\" max=\"%1\" width=\"%2\" customWidth=\"1\"/>").arg(c + 1).arg(width);
    }
    sheet += QStringLiteral("</cols><sheetData>");
    auto appendRow = [&](int rowIndex, const QStringList &row, bool header) {
        sheet += QStringLiteral("<row r=\"%1\">").arg(rowIndex);
        for (int c = 0; c < row.size(); ++c) {
            const QString ref = columnName(c) + QString::number(rowIndex);
            const QString value = row.at(c);
            const QString style = header ? QStringLiteral(" s=\"1\"") : QString();
            if (value.isEmpty()) continue;
            if (!header && table.numericColumns.contains(c) && isPlainNumber(value)) {
                sheet += QStringLiteral("<c r=\"%1\"><v>%2</v></c>").arg(ref, value);
            } else {
                sheet += QStringLiteral("<c r=\"%1\"%2 t=\"inlineStr\"><is><t xml:space=\"preserve\">%3</t></is></c>")
                             .arg(ref, style, xmlEscape(value));
            }
        }
        sheet += QStringLiteral("</row>");
    };
    appendRow(1, table.header, true);
    for (int r = 0; r < table.rows.size(); ++r) appendRow(r + 2, table.rows.at(r), false);
    sheet += QStringLiteral("</sheetData></worksheet>");

    // 工作表名不能含 : \ / ? * [ ]，最长 31 字符
    QString name = sheetName;
    name.remove(QRegularExpression(QStringLiteral("[:\\\\/?*\\[\\]]")));
    name = name.left(31).trimmed();
    if (name.isEmpty()) name = QStringLiteral("Sheet1");

    ZipWriter zip;
    zip.addFile(QStringLiteral("[Content_Types].xml"), (xmlHead + QStringLiteral(
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
        "<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
        "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
        "</Types>")).toUtf8());
    zip.addFile(QStringLiteral("_rels/.rels"), (xmlHead + QStringLiteral(
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
        "</Relationships>")).toUtf8());
    zip.addFile(QStringLiteral("xl/workbook.xml"), (xmlHead + QStringLiteral(
        "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
        "<sheets><sheet name=\"%1\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>").arg(xmlEscape(name))).toUtf8());
    zip.addFile(QStringLiteral("xl/_rels/workbook.xml.rels"), (xmlHead + QStringLiteral(
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
        "</Relationships>")).toUtf8());
    // 样式 0 为默认，样式 1 为表头加粗
    zip.addFile(QStringLiteral("xl/styles.xml"), (xmlHead + QStringLiteral(
        "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
        "<fonts count=\"2\"><font><sz val=\"11\"/><name val=\"Calibri\"/></font>"
        "<font><b/><sz val=\"11\"/><name val=\"Calibri\"/></font></fonts>"
        "<fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill><fill><patternFill patternType=\"gray125\"/></fill></fills>"
        "<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>"
        "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
        "<cellXfs count=\"2\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/>"
        "<xf numFmtId=\"0\" fontId=\"1\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyFont=\"1\"/></cellXfs>"
        "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles>"
        "</styleSheet>")).toUtf8());
    zip.addFile(QStringLiteral("xl/worksheets/sheet1.xml"), sheet.toUtf8());
    return zip.finish(error);
}

bool writeFile(const QString &path, const QByteArray &data, QString &error)
{
    if (path.isEmpty()) {
        error = QStringLiteral("未选择保存位置");
        return false;
    }
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        error = QStringLiteral("无法创建目录: %1").arg(info.absolutePath());
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        error = QStringLiteral("无法写入文件（可能正被 Excel 打开）: %1").arg(file.errorString());
        return false;
    }
    if (file.write(data) != data.size() || !file.commit()) {
        error = QStringLiteral("写入文件失败: %1").arg(file.errorString());
        return false;
    }
    return true;
}

} // namespace LogExport
