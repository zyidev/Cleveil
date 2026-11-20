#pragma once

// ============================================================================
//  uninstaller.h —— 卸载命令的解析与异步执行
//  两条执行通道：
//    1) 普通通道：QProcess 直接拉起卸载程序（最常见）
//    2) 提权通道：ShellExecuteEx(runas) 弹 UAC，用于需要改组策略/写 HKLM 的卸载器
//  之所以做成 QObject + 信号，是为了不阻塞界面线程（卸载器可能跑几分钟）。
// ============================================================================

#include "core/appinfo.h"

#include <QObject>
#include <QString>
#include <QStringList>

QT_BEGIN_NAMESPACE
class QProcess;
class QTimer;
QT_END_NAMESPACE

namespace un {

// 查询某个 MSI 产品码在本机是否"真的装着"。
// 存在的意义：卸载登记项的键名不一定等于产品码（安装器升级后常留下僵尸项），
// 直接 msiexec /X{键名GUID} 会拿到 1605（此产品未安装）。
// 返回 true 表示 MsiQueryProductState == INSTALLSTATE_DEFAULT(5) 或 LOCAL(3)。
bool msiProductInstalled(const QString &productCode);

// 卸载后的复核：MSI 看产品码，商店包看 AppX，驱动看 INF 是否还在，
// 服务看 SCM，其余看登记项。没有可核对的记录时返回 true（仍视为还在），
// 避免“键是空的”被当成已经卸载成功。
bool appStillInstalled(const InstalledApp &app);

// 一条准备好执行的卸载命令
struct UninstallPlan {
    QString     program;          // 可执行文件（如 C:\…\uninst.exe 或 msiexec.exe）
    QStringList arguments;        // 参数列表
    bool        silent = false;   // 是否静默执行
    bool        needsAdmin = false;
    bool        valid = false;
    bool        unsupportedSilent = false;   // 程序没提供静默参数，只能弹窗
    QString     reason;           // valid=false 时的失败原因
    QString     displayCommand;   // 给用户看的完整命令行
};

class Uninstaller {
public:
    // 根据记录拼装卸载命令；silent=true 时优先用 QuietUninstallString / msiexec /qn
    static UninstallPlan buildPlan(const InstalledApp &app, bool silent);

    // 把一条命令行拆成 程序 + 参数（正确处理引号）
    static bool splitCommandLine(const QString &cmdLine, QString *program, QStringList *args);

    // 该程序是否大概率需要管理员权限
    static bool likelyNeedsAdmin(const InstalledApp &app);
};

// ----------------------------------------------------------------------------
// UninstallRunner —— 异步执行一条卸载计划
// ----------------------------------------------------------------------------
class UninstallRunner : public QObject {
    Q_OBJECT

public:
    explicit UninstallRunner(QObject *parent = nullptr);
    ~UninstallRunner() override;

    // 开始执行。elevate=true 时走 UAC 提权通道。
    void run(const UninstallPlan &plan, bool elevate);

    // 请求终止（用户点"取消"时调用）
    void terminate();

signals:
    // started=false 表示进程压根没起来（error 里是原因）
    void finished(bool started, quint32 exitCode, const QString &error);

private slots:
    void pollElevatedProcess();   // 提权通道用定时器轮询进程是否结束

private:
    void startElevated(const UninstallPlan &plan);
    void cleanup();

    QProcess *m_proc = nullptr;
    QTimer   *m_timer = nullptr;
    Qt::HANDLE m_processHandle = nullptr;   // 提权通道拿到的进程句柄
    bool       m_finished = false;
};

} // namespace un
