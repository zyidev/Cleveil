#pragma once

// ============================================================================
//  appinfo.h —— 一条"已安装程序"记录的内存表示
//  扫描器负责填充，界面负责展示，卸载器/残留清理器负责消费。
//  纯数据 + 少量派生逻辑，刻意不依赖 UI 头文件，方便单元测试复用。
// ============================================================================

#include <QDate>
#include <QString>
#include <QStringList>
#include <QVector>

namespace un {

// 安装类型决定卸载命令怎么拼装、残留怎么找。
enum class PackageType {
    Msi,      // Windows Installer：注册表项名通常是 {GUID}，用 msiexec /X{GUID}
    Exe,      // 程序自带卸载器：uninst.exe / unins000.exe / Uninstall.exe …
    Store,    // Microsoft Store / MSIX 包（走 winget 或 explorer shell）
    Unknown   // 判定不出来（例如只有一条裸命令行）
};

inline QString packageTypeText(PackageType type)
{
    switch (type) {
    case PackageType::Msi:   return QStringLiteral("MSI");
    case PackageType::Exe:   return QStringLiteral("EXE");
    case PackageType::Store: return QStringLiteral("Store");
    default:                 return QStringLiteral("未知");
    }
}

// ----------------------------------------------------------------------------
// InstalledApp —— 一个已安装的程序
// ----------------------------------------------------------------------------
struct InstalledApp {
    QString     id;                    // 唯一键：注册表完整路径（含 32/64 视图后缀）
    QString     name;                  // DisplayName
    QString     version;               // DisplayVersion
    QString     publisher;             // Publisher
    QDate       installDate;           // InstallDate（YYYYMMDD → QDate），无效表示未知
    qint64      sizeBytes = -1;        // EstimatedSize(KB) 换算后的字节数，-1 表示未知
    QString     installLocation;       // InstallLocation
    QString     iconSource;            // DisplayIcon（可能是 "path,0" 或 "path,-123"）
    QString     uninstallString;       // UninstallString
    QString     quietUninstallString;  // QuietUninstallString（静默卸载用）
    QString     registryPath;          // 例：HKEY_LOCAL_MACHINE\SOFTWARE\…\Uninstall\{GUID}
    QString     productCode;           // 从 UninstallString 或键名提取出的 GUID
    QStringList keywords;              // 残留匹配用的候选名（程序名/发布者/可执行名）
    PackageType type = PackageType::Unknown;
    bool        is64View = true;       // 是否取自 64 位注册表视图
    bool        systemComponent = false; // SystemComponent=1：微软隐藏项
    bool        isUpdate = false;      // 补丁 / 更新包（ReleaseType、ParentKeyName）

    // 是否有可用的卸载命令。没有的话只能走"强制卸载"。
    bool canUninstall() const
    {
        return !uninstallString.trimmed().isEmpty()
            || !quietUninstallString.trimmed().isEmpty();
    }

    // 是否有静默卸载能力（自己提供了 QuietUninstallString）。
    bool hasQuietCommand() const
    {
        return !quietUninstallString.trimmed().isEmpty();
    }

    // 名字首字母，用于列表分组（预留）。
    QString initial() const
    {
        return name.isEmpty() ? QStringLiteral("#") : name.left(1).toUpper();
    }
};

} // namespace un
