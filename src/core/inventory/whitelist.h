#pragma once

#include <QString>

namespace un {

// 系统共享组件 / 收件箱路径白名单：启发式与安装器文件探测命中后标记为不可卸载。
bool isInventoryWhitelisted(const QString &name,
                            const QString &publisher,
                            const QString &installLocation,
                            const QString &productOrInf = QString());

// 驱动包是否为收件箱（非 oem*.inf）——不可作为卸载目标。
bool isInboxDriverInf(const QString &infFileName);

// 设备类是否属于磁盘 / 系统 / 键盘等关键类。空类名不算关键类。
bool isCriticalDriverClass(const QString &className);

// 执行前复核：必须是 %windir%\INF\oemN.inf，且能读到非关键设备类。
// 失败时写出原因。不生成可复制的 pnputil 命令。
bool driverInfRemovable(const QString &infFileName, QString *reason = nullptr);

} // namespace un
