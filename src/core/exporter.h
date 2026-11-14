#pragma once

// ============================================================================
//  exporter.h —— 把已安装程序清单导出成 CSV / HTML
//  CSV 带 UTF-8 BOM，Excel 双击直接正常显示中文。
// ============================================================================

#include "core/appinfo.h"

#include <QString>
#include <QVector>

namespace un {

class Exporter {
public:
    static bool exportCsv(const QVector<InstalledApp> &apps, const QString &file, QString *error);
    static bool exportHtml(const QVector<InstalledApp> &apps, const QString &file, QString *error);
};

} // namespace un
