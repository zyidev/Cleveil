#pragma once

// ============================================================================
//  installmonitor.h —— 安装 / 卸载实时监控
// ----------------------------------------------------------------------------
//  为什么"驱动级"在这里落地成"注册表通知 + 内核回调接口"：
//  真正的内核驱动（注册表回调 CmRegisterCallbackEx、进程回调 PsSetCreateProcessNotifyRoutine）
//  需要 WDK 编译、EV 证书签名、并在开启"测试签名模式"后重启才能加载，属于系统级改动。
//  本模块的做法是：
//    1) 用户态主力通道：对四条 Uninstall 键挂 RegNotifyChangeKeyValue（内核会在键被写入时
//       主动通知调用线程，不需要轮询），秒级捕获"安装/卸载"事件；
//    2) 进程通道：定期枚举进程，捕捉 msiexec / setup.exe 之类的安装活动，给出"正在安装什么"；
//    3) 驱动通道：如果系统里装了同名驱动服务（设备名 \\.\CleveilMon），自动切换到驱动事件流；
//       驱动 ABI 定义见下方 CleveilDriverEvent，接口保持不变，后续编译并签名驱动即可无缝启用。
//
//  线程模型：一个后台线程负责阻塞等待 + 扫描，事件通过 Queued 信号回到界面线程，
//  界面永远不会被卡住。
// ============================================================================

#include "core/appinfo.h"

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QVector>

namespace un {

// 监控通道
enum class MonitorChannel {
    Registry,   // 用户态：注册表变更通知（默认，无需驱动）
    Driver,     // 内核态：驱动上报事件（需要自行编译+签名驱动）
};

inline QString monitorChannelText(MonitorChannel c)
{
    return c == MonitorChannel::Driver ? QStringLiteral("内核驱动通道")
                                       : QStringLiteral("注册表通知通道（用户态）");
}

// 一条监控事件
struct MonitorEvent {
    QDateTime    time;
    QString      kind;          // "安装完成" / "已卸载" / "安装活动"
    QString      name;          // 程序名或进程名
    QString      detail;        // 版本 / 发布者 / 路径等补充说明
    InstalledApp app;           // kind == "安装完成" 时是完整记录，可直接卸载
};

// 与驱动约定的单条事件结构（POD，必须与驱动侧逐字节一致）
struct CleveilDriverEvent {
    quint64 timestampUtc;   // FILETIME，UTC
    quint32 eventType;      // 1=注册表写入 2=进程创建 3=文件落盘
    quint32 reserved;
    wchar_t subject[260];   // 键路径 / 进程名 / 文件路径
};

class InstallMonitor : public QObject {
    Q_OBJECT

public:
    explicit InstallMonitor(QObject *parent = nullptr);
    ~InstallMonitor() override;

    void start();
    void stop();
    bool isRunning() const { return m_running; }
    MonitorChannel channel() const { return m_channel; }

    // 手动触发一次基线重建（程序列表刷新后调用，避免把"已知程序"误报成新装）
    void resetBaseline();

signals:
    void started(MonitorChannel channel);
    void stopped();
    void eventRecorded(const MonitorEvent &ev);   // 已按时间顺序发出
    void scanFailed(const QString &reason);

private:
    void workerLoop();                 // 后台线程主体
    void emitEvent(const MonitorEvent &ev);

    bool            m_running = false;
    MonitorChannel  m_channel = MonitorChannel::Registry;
    Qt::HANDLE      m_thread = nullptr;          // std::thread 句柄（void* 不透明存放）
    struct Impl;                                  // 具体实现细节放进 .cpp，避免头文件依赖 Win32
    Impl           *d = nullptr;
};

} // namespace un
