#pragma once

#include <QString>
#include <QStringList>
#include <QList>

// 日志导出：把查询条件下的全部日志写成 CSV（UTF-8 带 BOM，Excel 直接打开不乱码）
// 或 Excel .xlsx（不依赖第三方库，按 Office Open XML 最小结构生成，存储方式打包）。
namespace LogExport {

struct Table {
    QStringList header;
    QList<QStringList> rows;
    QList<int> numericColumns; // 这些列的纯数字单元格在 xlsx 中写为数值
};

// Excel 单个工作表最多 1048576 行（含表头）
constexpr int kMaxXlsxRows = 1048575;

// 报警处置日志"报警处置完成，操作员=张三，架号=12，轮位=3，…"拆出的列
QStringList dispositionHeader();
QStringList dispositionColumns(const QString &message);

// 表头 + 一行日志
Table makeTable(bool alertDisposition);
void appendLogRow(Table &table, bool alertDisposition, const QString &time, const QString &level,
                  const QString &message);

// CSV 单元格转义：含逗号、引号、换行时加引号并把引号加倍；以 = + - @ 等开头的文本前加 '，
// 避免 Excel 把用户填写的内容（如处置人员姓名）当作公式执行
QString csvField(const QString &value);
QByteArray toCsv(const Table &table);
QByteArray toXlsx(const Table &table, const QString &sheetName, QString *error = nullptr);

// 原子写入（QSaveFile）：成功返回 true，失败给出中文原因
bool writeFile(const QString &path, const QByteArray &data, QString &error);

} // namespace LogExport
