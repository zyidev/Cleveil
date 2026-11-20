// ============================================================================
//  exporter.cpp —— 清单导出
// ============================================================================

#include "core/exporter.h"
#include "core/winutil.h"

#include <QDateTime>
#include <QFile>
#include <QSaveFile>
#include <QStringConverter>
#include <QTextStream>

namespace un {

namespace {

// CSV 字段转义。以 = + - @ 或制表符开头的单元格在 Excel 里会被当成公式，先加单引号。
QString csvEscape(const QString &field)
{
    QString s = field;
    if (!s.isEmpty()) {
        const QChar c = s.at(0);
        if (c == QLatin1Char('=') || c == QLatin1Char('+') || c == QLatin1Char('-')
            || c == QLatin1Char('@') || c == QLatin1Char('\t') || c == QLatin1Char('\r'))
            s.prepend(QLatin1Char('\''));
    }
    if (s.contains(QLatin1Char(',')) || s.contains(QLatin1Char('"'))
        || s.contains(QLatin1Char('\n')) || s.contains(QLatin1Char('\r'))
        || s != field)
    {
        s.replace(QLatin1String("\""), QLatin1String("\"\""));
        return QStringLiteral("\"%1\"").arg(s);
    }
    return s;
}

// HTML 转义
QString htmlEscape(const QString &field)
{
    QString s = field;
    s.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    s.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    s.replace(QLatin1Char('>'), QLatin1String("&gt;"));
    s.replace(QLatin1Char('"'), QLatin1String("&quot;"));
    return s;
}

} // namespace

bool Exporter::exportCsv(const QVector<InstalledApp> &apps, const QString &file, QString *error)
{
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("无法写入文件：%1").arg(out.errorString());
        return false;
    }

    // UTF-8 BOM：没有它 Excel 会把中文当乱码
    out.write("\xEF\xBB\xBF");

    QTextStream stream(&out);
    stream.setEncoding(QStringConverter::Utf8);

    stream << QStringLiteral("名称,版本,发布者,安装日期,大小(字节),大小,类型,安装位置,"
                             "卸载命令,注册表项\n");

    for (const InstalledApp &a : apps) {
        const QString sizeRaw = a.sizeBytes >= 0 ? QString::number(a.sizeBytes) : QString();
        QStringList row;
        row << csvEscape(a.name)
            << csvEscape(a.version)
            << csvEscape(a.publisher)
            << csvEscape(a.installDate.isValid()
                             ? a.installDate.toString(QStringLiteral("yyyy-MM-dd")) : QString())
            << sizeRaw
            << (sizeRaw.isEmpty() ? QString() : csvEscape(formatBytes(a.sizeBytes)))
            << csvEscape(packageTypeText(a.type))
            << csvEscape(a.installLocation)
            << csvEscape(a.uninstallString)
            << csvEscape(a.registryPath);
        stream << row.join(QLatin1Char(',')) << '\n';
    }

    stream.flush();
    if (!out.commit()) {
        if (error) *error = QStringLiteral("保存失败：%1").arg(out.errorString());
        return false;
    }
    return true;
}

bool Exporter::exportHtml(const QVector<InstalledApp> &apps, const QString &file, QString *error)
{
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("无法写入文件：%1").arg(out.errorString());
        return false;
    }

    QTextStream stream(&out);
    stream.setEncoding(QStringConverter::Utf8);

    // 统计总量
    qint64 totalBytes = 0;
    for (const InstalledApp &a : apps) {
        if (a.sizeBytes > 0) totalBytes += a.sizeBytes;
    }

    stream << "<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n"
              "<meta charset=\"utf-8\">\n"
              "<title>已安装程序清单</title>\n"
              "<style>\n"
              "  body{font-family:'Segoe UI','Microsoft YaHei',sans-serif;background:#f5f6fa;"
              "color:#23262e;margin:0;padding:32px;}\n"
              "  h1{font-size:22px;margin:0 0 8px;}\n"
              "  .meta{color:#6b7280;font-size:13px;margin-bottom:20px;}\n"
              "  table{border-collapse:collapse;width:100%;background:#fff;"
              "box-shadow:0 1px 3px rgba(0,0,0,.08);border-radius:6px;overflow:hidden;}\n"
              "  th,td{padding:8px 12px;font-size:13px;text-align:left;"
              "border-bottom:1px solid #eef0f4;}\n"
              "  th{background:#4a5b8c;color:#fff;font-weight:600;position:sticky;top:0;}\n"
              "  tr:hover td{background:#f7f9ff;}\n"
              "  td.num{text-align:right;font-variant-numeric:tabular-nums;}\n"
              "  .tag{display:inline-block;padding:1px 6px;border-radius:4px;font-size:11px;"
              "background:#e8eefc;color:#3b4f86;}\n"
              "</style>\n</head>\n<body>\n";

    stream << QStringLiteral("<h1>已安装程序清单</h1>\n")
           << QStringLiteral("<div class=\"meta\">生成时间：%1　共 %2 个程序　"
                              "已知占用合计：%3</div>\n")
                  .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")))
                  .arg(apps.size())
                  .arg(formatBytes(totalBytes));

    stream << "<table>\n<thead><tr>"
              "<th>名称</th><th>版本</th><th>发布者</th><th>安装日期</th>"
              "<th>大小</th><th>类型</th><th>安装位置</th>"
              "</tr></thead>\n<tbody>\n";

    for (const InstalledApp &a : apps) {
        stream << "<tr>"
               << QStringLiteral("<td>%1</td>").arg(htmlEscape(a.name))
               << QStringLiteral("<td>%1</td>").arg(htmlEscape(a.version))
               << QStringLiteral("<td>%1</td>").arg(htmlEscape(a.publisher))
               << QStringLiteral("<td>%1</td>").arg(a.installDate.isValid()
                                                        ? a.installDate.toString(QStringLiteral("yyyy-MM-dd"))
                                                        : QStringLiteral("—"))
               << QStringLiteral("<td class=\"num\">%1</td>").arg(
                      a.sizeBytes >= 0 ? htmlEscape(formatBytes(a.sizeBytes))
                                       : QStringLiteral("—"))
               << QStringLiteral("<td><span class=\"tag\">%1</span></td>")
                      .arg(htmlEscape(packageTypeText(a.type)))
               << QStringLiteral("<td>%1</td>").arg(htmlEscape(a.installLocation))
               << "</tr>\n";
    }

    stream << "</tbody>\n</table>\n</body>\n</html>\n";
    stream.flush();

    if (!out.commit()) {
        if (error) *error = QStringLiteral("保存失败：%1").arg(out.errorString());
        return false;
    }
    return true;
}

} // namespace un
