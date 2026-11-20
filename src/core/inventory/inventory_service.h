#pragma once

// ============================================================================
//  inventory_service.h —— 多源枚举入口
//  保留 RegistryScanner；本服务在其之上叠加 MSI / AppX / 驱动 / 服务等来源，
//  合并去重后返回带 sources / heuristic 标记的 InstalledApp。
// ============================================================================

#include "core/inventory/types.h"

namespace un {

class InventoryService {
public:
    // 完整扫描（软件 + 驱动包 + 孤儿服务 + 启发式分区）
    static InventoryScanResult scan(const InventoryScanOptions &options = InventoryScanOptions());

    // 仅返回软件主列表（供 MainWindow::refresh 替换 RegistryScanner::scanAll）
    static QVector<InstalledApp> scanSoftware(const InventoryScanOptions &options = InventoryScanOptions());

    // 软件 + 驱动包 +（可选）孤儿服务。驱动包不携带可复制的 pnputil 命令。
    static QVector<InstalledApp> projectApps(const InventoryScanResult &bundle,
                                             bool includeOrphanServices);

    // 安装监控可调用：把当前 Program Files / LocalAppData\Programs 顶层目录写成基线
    static bool captureDirectorySnapshot(QString *error = nullptr);

    // 与上次基线做差，返回新增目录（供 Heuristic 源使用；也可单独调用）
    static QStringList snapshotNewDirectories(QString *error = nullptr);
};

} // namespace un
