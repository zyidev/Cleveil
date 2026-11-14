#pragma once

// ============================================================================
//  registryscanner.h —— 已安装程序扫描器
//  数据来源：Windows 注册表中的 Uninstall 键（与"设置-应用"、"控制面板
//  程序和功能"用的是同一批数据），因此能覆盖绝大多数传统安装的程序。
// ============================================================================

#include "core/appinfo.h"

#include <QVector>

namespace un {

class RegistryScanner {
public:
    // 扫描全部四条路径：
    //   HKLM\SOFTWARE\…\Uninstall（64 位视图）
    //   HKLM\SOFTWARE\…\Uninstall（32 位视图，即 WOW6432Node）
    //   HKCU\SOFTWARE\…\Uninstall（当前用户，两个视图）
    // 返回值不去重前是"原始条目"，内部已按 名称+版本+发布者 做过合并。
    static QVector<InstalledApp> scanAll();
};

} // namespace un
