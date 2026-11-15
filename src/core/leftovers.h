#pragma once

// ============================================================================
//  leftovers.h —— 残留扫描与清理
//  卸载器跑完之后，注册表项、安装目录、用户数据、快捷方式、服务、启动项、
//  计划任务里往往会留一堆垃圾。这一层负责把它们找出来（供用户勾选），
//  再负责安全地删掉（能进回收站的进回收站，注册表先备份）。
// ============================================================================

#include "core/appinfo.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace un {

enum class LeftoverKind {
    RegistryKey,     // 整棵注册表键
    RegistryValue,   // 注册表里某个值（Run 启动项）
    Folder,          // 目录
    File,            // 单个文件（快捷方式等）
    Service,         // Windows 服务
    ScheduledTask    // 计划任务
};

// 这条残留凭什么被列出来。自动清理（命令行 --apply、批量向导）只删 PathBound。
enum class LeftoverMatch {
    PathBound,   // 卸载登记项，或文件/服务/任务的路径确在安装目录内
    Name,        // 名称与程序名完全相同，未经路径证实
    Publisher    // 只和发布者同名
};

// 一条残留记录
struct LeftoverItem {
    LeftoverKind kind = LeftoverKind::Folder;
    QString      category;              // 分组名：注册表项 / 安装目录 / …
    QString      title;                 // 列表里显示的短名字
    QString      detail;                // 完整路径（或"键路径 → 值名"）
    QString      extra;                 // 服务名 / 计划任务名 / 注册表值名
    qint64       sizeBytes = -1;        // 目录、文件才有意义
    bool         is64View = true;        // 注册表项用
    bool         requiresAdmin = false;
    LeftoverMatch match = LeftoverMatch::Name;
    bool         checked = false;       // 只有 PathBound 默认勾选
    QString      note;                  // 给用户的提示
};

// 清理结果汇总
struct RemovalResult {
    int     successCount = 0;
    int     failedCount = 0;
    qint64  reclaimedBytes = 0;         // 成功清理掉的磁盘占用
    QStringList errors;
    QStringList backups;                // 生成的 .reg 备份文件路径
};

class LeftoverScanner {
public:
    // stage 用于把"正在扫描 XXX"回报给界面，percent 0~100
    using ProgressFn = std::function<void(const QString &stage, int percent)>;

    // 扫描指定程序的残留；返回按类别排好序的条目列表。
    static QVector<LeftoverItem> scan(const InstalledApp &app,
                                      const ProgressFn &progress = ProgressFn());
};

class LeftoverRemover {
public:
    // 执行清理。backupDir 不为空时，注册表删除前会先导出 .reg 备份到该目录。
    static RemovalResult remove(const QVector<LeftoverItem> &items,
                                const QString &backupDir = QString());

    // 结束所有可执行文件位于该程序安装目录下的进程；返回结束数量。
    static int killRelatedProcesses(const InstalledApp &app);

    // 找出与该程序关联的进程（供界面确认）
    static QStringList relatedProcessNames(const InstalledApp &app);
};

} // namespace un
