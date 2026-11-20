#pragma once

// ============================================================================
//  winutil.h —— Windows 平台底层工具函数集合
//  这里集中所有"直接调 Win32 API"的代码，其它模块只管上层逻辑。
// ============================================================================

#include <QDate>
#include <QImage>
#include <QIcon>
#include <QString>
#include <QStringList>
#include <QVector>

#include <QtGlobal>

#ifndef NOMINMAX
#define NOMINMAX          // 关掉 windows.h 的 min/max 宏，否则和 std::min/std::max 打架
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace un {

// ---------------------------------------------------------------------------
// 零、WinRT / COM 公寓（AppX 枚举前调用，与 Qt 主线程 STA 一致；禁止再 init MTA）
// ---------------------------------------------------------------------------
void ensureWinRtApartment();

// ---------------------------------------------------------------------------
// 一、字符串与环境变量
// ---------------------------------------------------------------------------

// 展开 %LOCALAPPDATA% 之类的环境变量；无法展开时原样返回。
QString expandEnv(const QString &text);

// 去掉路径首尾的引号与空白（注册表里的路径经常带引号）。
QString unquote(const QString &text);

// 把任意字符串归一化成"匹配用的令牌"：小写、去掉空格/标点/常见后缀词。
// 用于判断某个目录名/注册表键名是否属于目标程序。
QString normalizeToken(const QString &text);

// 拆出 "C:\App\app.exe,0" 这种 DisplayIcon 规格里的路径和索引。
// 返回路径，index 通过出参带回（默认 0；负数表示资源 ID）。
QString splitIconSpec(const QString &spec, int *index);

// 判断 child 是否位于 parent 目录之内（大小写不敏感、解析 .. 与 \\?\ 前缀）。
bool isPathUnder(const QString &child, const QString &parent);

// 危险路径防护：拒绝盘符根、Windows 整棵目录、Program Files / ProgramData / Users
// 的根目录，以及用户配置目录本身。Program Files\某程序 这种子目录允许删除。
// 回收站、结束进程、残留扫描共用这一判断。
bool isDangerousPath(const QString &path);

// 系统关键进程（csrss / lsass 等）。对这些进程调用 TerminateProcess 可能直接蓝屏。
bool isProtectedProcessName(const QString &exeName);

// -------------------------------------------------------------------------
// 二、图标
// -------------------------------------------------------------------------

// 把 HICON 转成 QImage。Qt6 已经移除了 QImage::fromHICON，
// 所以这里手工走 GetIconInfo + GetDIBits 的路径。
QImage imageFromHIcon(HICON hIcon);

// 从文件（exe/dll/ico）按索引取图标；取不到返回空 QImage。
QImage imageFromFileIcon(const QString &path, int index = 0, int size = 32);

// -------------------------------------------------------------------------
// 三、权限
// -------------------------------------------------------------------------

// 当前进程是否拥有管理员权限（TokenElevation）。
bool isRunningAsAdmin();

// 以管理员身份启动某个程序并等待它结束。
// program/args 会被拼成命令行交给 ShellExecuteEx(runas)，会弹 UAC。
bool runAsAdminAndWait(const QString &program,
                       const QStringList &args,
                       quint32 *exitCode,
                       QString *error);

// 用当前权限启动程序并等待结束（不弹 UAC，用于普通卸载器）。
bool runAndWait(const QString &program,
                const QStringList &args,
                quint32 *exitCode,
                QString *error);

// 重新以管理员身份启动本程序（参数原样传递），成功后调用方应退出。
bool relaunchSelfAsAdmin(const QStringList &args);

// -------------------------------------------------------------------------
// 四、文件系统
// -------------------------------------------------------------------------

// 删除到回收站（可撤销）。内部走 SHFileOperation(FOF_ALLOWUNDO)。
bool recyclePaths(const QStringList &paths, QString *error);

// 递归统计目录占用（限制递归深度，避免在大目录上卡死）。
qint64 directorySize(const QString &path, int maxDepth = 8, int *fileCount = nullptr);

// 朋友化的大小文本："1.2 GB" / "356 MB" / "12.4 KB" / "未知"。
QString formatBytes(qint64 bytes);

// -------------------------------------------------------------------------
// 五、注册表
// -------------------------------------------------------------------------

// 解析 "HKEY_LOCAL_MACHINE\SOFTWARE\X" 形式为根键 + 子路径。
// 支持 HKLM/HKCU/HKCR/HKU/HKCC 等简写。
bool splitRegistryPath(const QString &fullPath, HKEY *root, QString *subPath);

// 从 ARP Uninstall 登记项读取 InstallDate（yyyyMMdd）与 EstimatedSize（转为字节）
bool supplementFromUninstallKey(const QString &registryPath, bool is64View,
                                QDate *installDate, qint64 *sizeBytes);

// 删除整棵注册表子树（值 + 所有子键 + 键本身）。
bool deleteRegistryKey(const QString &fullPath, bool is64View, QString *error);

// 只删除某个键下的一个值（用于清理 Run 启动项之类）。
bool deleteRegistryValue(const QString &keyPath, const QString &valueName,
                         bool is64View, QString *error);

// 用 reg.exe export 备份某个键到 .reg 文件（强制卸载前的保险）。
bool backupRegistryKey(const QString &fullPath, const QString &regFile, QString *error);

// 判断某个注册表键是否还存在（卸载后用它确认"真的卸掉了"）。
bool registryKeyExists(const QString &fullPath, bool is64View);

// -------------------------------------------------------------------------
// 六、进程
// -------------------------------------------------------------------------

struct ProcessRef {
    quint32 pid = 0;
    QString name;   // 可执行文件名
    QString path;   // 完整路径（取不到时为空）
};

// 枚举系统中所有可访问的进程。
QVector<ProcessRef> enumerateProcesses();

// 找出可执行文件位于指定目录下的进程（用于"卸载前先结束占用"）。
QVector<ProcessRef> processesUnder(const QString &directory);

// 强杀进程。成功返回 true。
bool killProcess(quint32 pid, QString *error);

// 仅当进程映像仍位于 directory 内、且不是系统关键进程时才结束。
// 用同一次 OpenProcess 拿到的句柄复核路径，避免 PID 被复用后误杀。
bool killProcessIfUnder(quint32 pid, const QString &directory, QString *error);

// 枚举某个服务的信息，供残留扫描使用。
struct ServiceRef {
    QString name;        // 服务名
    QString displayName;
    QString binaryPath;  // ImagePath
};
QVector<ServiceRef> enumerateServices();

// 删除服务（需要管理员权限）。
bool deleteService(const QString &serviceName, QString *error);

} // namespace un
