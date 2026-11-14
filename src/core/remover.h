#pragma once

// ============================================================================
//  remover.h —— 强制卸载的高层动作
//  "强制卸载"= 结束占用进程 + 删掉应用列表里的登记项 + 扫描残留交给用户清理。
//  这里只做前两步，残留的勾选与删除交给 LeftoverDialog，避免误删。
// ============================================================================

#include "core/appinfo.h"

#include <QString>

namespace un {

class ForceUninstaller {
public:
    // 结束所有可执行文件位于安装目录下的进程，返回结束个数
    static int terminateProcesses(const InstalledApp &app);

    // 删除该程序在"应用列表"中的登记项（会先导出 .reg 备份）
    // 返回是否成功；成功时 backupFile 带出备份文件路径（可能为空）
    static bool removeRegistration(const InstalledApp &app,
                                   const QString &backupDir,
                                   QString *backupFile,
                                   QString *error);

    // 生成一个带时间戳的备份目录：%LOCALAPPDATA%\Cleveil\Cleveil\backup\20261114-2130
    static QString makeBackupDir();
};

} // namespace un
