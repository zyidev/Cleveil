#pragma once

// ============================================================================
//  inventory/types.h —— 多源枚举的公共类型
//  权威来源与启发式分开标记；合并后投影为 InstalledApp 供现有 UI 使用。
// ============================================================================

#include "core/appinfo.h"

#include <QDate>
#include <QString>
#include <QStringList>
#include <QVector>

namespace un {

enum class InventorySourceId {
    Arp,
    Msi,
    Appx,
    DriverPackage,
    Service,
    InstallerFile,
    Shortcut,
    ExeVersion,
    SnapshotDiff
};

enum class InventoryTrust {
    Authoritative,   // ARP / MSI / AppX / OEM 驱动包 / 服务存在性
    Corroborating,   // 安装器卸载文件印证已有条目
    Heuristic        // 快捷方式 / 版本信息 / 快照差分 —— 单独标识
};

enum class InventoryCategory {
    Software,
    DriverPackage,
    OrphanService,
    HeuristicFind
};

inline QString inventorySourceText(InventorySourceId id)
{
    switch (id) {
    case InventorySourceId::Arp:           return QStringLiteral("ARP");
    case InventorySourceId::Msi:           return QStringLiteral("MSI");
    case InventorySourceId::Appx:          return QStringLiteral("AppX");
    case InventorySourceId::DriverPackage: return QStringLiteral("Driver");
    case InventorySourceId::Service:       return QStringLiteral("Service");
    case InventorySourceId::InstallerFile: return QStringLiteral("Installer");
    case InventorySourceId::Shortcut:      return QStringLiteral("Shortcut");
    case InventorySourceId::ExeVersion:    return QStringLiteral("ExeInfo");
    case InventorySourceId::SnapshotDiff:  return QStringLiteral("Snapshot");
    }
    return QStringLiteral("Unknown");
}

struct InventorySourceError {
    InventorySourceId source = InventorySourceId::Arp;
    QString message;
    bool accessDenied = false;
};

// 单条原始记录（合并前）
struct InventoryRecord {
    InventorySourceId source = InventorySourceId::Arp;
    InventoryTrust    trust  = InventoryTrust::Authoritative;
    InventoryCategory category = InventoryCategory::Software;

    QString localId;           // 源内稳定 id
    QString identityKey;       // 合并键：msi:{guid} / appx:{pfn} / arp:{path}|32|64 / path:{dir}
    QString name;
    QString version;
    QString publisher;
    QString installLocation;
    QString iconSource;
    QString uninstallString;
    QString quietUninstallString;
    QString productCode;       // MSI GUID / AppX full name / oemXX.inf
    QString registryPath;
    QString note;
    QDate   installDate;
    qint64  sizeBytes = -1;
    bool    is64View = true;
    bool    systemComponent = false;
    bool    isUpdate = false;
    bool    whitelistBlocked = false;  // 命中系统共享组件白名单，禁止当作可卸载目标
    PackageType packageType = PackageType::Unknown;
};

struct InventoryScanOptions {
    bool arp = true;
    bool msi = true;
    bool appx = true;
    bool driverPackages = true;
    bool services = true;
    bool installerFiles = true;   // 一层目录探测（权威缺失时才独立成条）
    bool shortcuts = false;       // 启发式，默认关
    bool exeVersion = false;      // 启发式，默认关
    bool snapshotDiff = false;    // 启发式，默认关
    bool includeHeuristicInSoftware = false; // 是否把启发式并入主列表（仍带 heuristic 标记）
};

struct InventoryScanResult {
    QVector<InstalledApp> software;        // 主列表（含来源标记）
    QVector<InventoryRecord> drivers;      // OEM 驱动包
    QVector<InventoryRecord> orphanServices;
    QVector<InventoryRecord> heuristic;    // 单独分区
    QVector<InventorySourceError> errors;
};

} // namespace un
