// ============================================================================
//  installmonitor.cpp —— 安装 / 卸载实时监控的实现
// ----------------------------------------------------------------------------
//  后台线程做三件事，全都围绕"尽快知道系统里装了什么、卸了什么"：
//    1) WaitForMultipleObjects 等四个注册表键的变更事件（内核写入时主动 set）
//    2) 每 2 秒枚举一次进程，捕捉 msiexec / setup.exe 这类安装活动
//    3) 有变更且静默 2 秒后，重新扫描 Uninstall 键，与基线做差集
//  差集结果通过 Queued 信号发回界面线程，界面只负责显示。
// ============================================================================

#include "core/installmonitor.h"
#include "core/registryscanner.h"
#include "core/winutil.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>

#include <atomic>
#include <thread>

namespace un {

namespace {

// 四条要监视的键：HKLM 64/32 位视图、HKCU 64/32 位视图
struct WatchTarget {
    HKEY         root;
    const wchar_t *subKey;
    DWORD        viewFlags;
};

const WatchTarget kWatchTargets[4] = {
    { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0 },
    { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", KEY_WOW64_32KEY },
    { HKEY_CURRENT_USER,  L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0 },
    { HKEY_CURRENT_USER,  L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", KEY_WOW64_32KEY },
};

constexpr int kWatchCount = 4;
constexpr int kWakeIndex  = kWatchCount;          // 第五个句柄是"唤醒/退出"事件
constexpr int kHandleCount = kWatchCount + 1;

// 一次变更后等这么久再扫描：安装器往往连续写十几个值，抖动要合并
constexpr DWORD kDebounceMs     = 2000;
// 没有注册表事件时，也定期扫一次（防止某些安装器不写 Uninstall 键、只写服务/文件）
constexpr DWORD kIdleRescanMs   = 15000;
// 进程轮询间隔
constexpr DWORD kProcessPollMs  = 2000;

// 安装器进程的识别规则（小写匹配）
bool looksLikeInstaller(const QString &lowerName)
{
    static const char *kTokens[] = {
        "msiexec", "setup", "install", "update", "unins", "patch", "installer", "bootstrapper"
    };
    for (const char *token : kTokens) {
        if (lowerName.contains(QLatin1String(token))) return true;
    }
    return false;
}

// 把 FILETIME 转成 QDateTime（保留给驱动事件流使用：驱动上报的是 UTC 时间戳）
QDateTime fileTimeToQDateTime(quint64 ft)
{
    FILETIME f;
    f.dwLowDateTime  = static_cast<DWORD>(ft & 0xFFFFFFFFull);
    f.dwHighDateTime = static_cast<DWORD>(ft >> 32);
    SYSTEMTIME st;
    if (!FileTimeToSystemTime(&f, &st)) return QDateTime::currentDateTime();
    return QDateTime(QDate(st.wYear, st.wMonth, st.wDay),
                     QTime(st.wHour, st.wMinute, st.wSecond), Qt::UTC).toLocalTime();
}

} // namespace

// ----------------------------------------------------------------------------
// Impl —— 只在 .cpp 里出现的后台状态
// ----------------------------------------------------------------------------
struct InstallMonitor::Impl {
    std::thread             thread;
    std::atomic<bool>       stopRequested{false};
    HANDLE                  wakeEvent = nullptr;

    // 基线（上一次看到的程序列表），只由后台线程读写，UI 线程通过 pendingBaseline 请求重建
    QVector<InstalledApp>   baseline;
    std::atomic<bool>       pendingBaseline{false};

    // 已上报过的安装器进程 PID，避免同一个 msiexec 反复刷屏
    QSet<quint32>           reportedPids;

    // 节流控制
    DWORD                   lastChangeTick = 0;
    DWORD                   lastScanTick   = 0;
    bool                    dirty          = false;

    QMutex                  mutex;   // 保护 baseline / reportedPids
};

// ----------------------------------------------------------------------------

InstallMonitor::InstallMonitor(QObject *parent)
    : QObject(parent)
    , d(new Impl)
{
}

InstallMonitor::~InstallMonitor()
{
    stop();
    delete d;
}

void InstallMonitor::start()
{
    if (m_running) return;

    d->stopRequested.store(false);
    d->pendingBaseline.store(true);
    d->dirty = false;
    d->reportedPids.clear();

    // 驱动存在就走内核通道，否则退回注册表通知通道
    m_channel = MonitorChannel::Registry;
    HANDLE driver = CreateFileW(L"\\\\.\\CleveilMon", GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (driver != INVALID_HANDLE_VALUE) {
        CloseHandle(driver);
        m_channel = MonitorChannel::Driver;
    }

    m_running = true;
    d->thread = std::thread(&InstallMonitor::workerLoop, this);
    emit started(m_channel);
}

void InstallMonitor::stop()
{
    if (!m_running) return;

    m_running = false;
    d->stopRequested.store(true);
    if (d->wakeEvent) SetEvent(d->wakeEvent);     // 立刻把阻塞中的等待唤醒

    if (d->thread.joinable()) d->thread.join();
    emit stopped();
}

void InstallMonitor::resetBaseline()
{
    d->pendingBaseline.store(true);
}

void InstallMonitor::emitEvent(const MonitorEvent &ev)
{
    // worker 线程不能直接 emit 到 UI 线程的对象，走 Queued 投递
    QMetaObject::invokeMethod(this, [this, ev]() { emit eventRecorded(ev); },
                              Qt::QueuedConnection);
}

// ----------------------------------------------------------------------------
// 后台线程主体
// ----------------------------------------------------------------------------
void InstallMonitor::workerLoop()
{
    HKEY  keys[kWatchCount]     = {};
    HANDLE events[kHandleCount] = {};

    // 退出事件（manual reset，用来从 WaitForMultipleObjects 里立刻返回）
    d->wakeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    events[kWakeIndex] = d->wakeEvent;

    for (int i = 0; i < kWatchCount; ++i) {
        keys[i] = nullptr;
        events[i] = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        const WatchTarget &t = kWatchTargets[i];
        if (RegOpenKeyExW(t.root, t.subKey, 0,
                          KEY_NOTIFY | KEY_READ | t.viewFlags, &keys[i]) != ERROR_SUCCESS) {
            keys[i] = nullptr;   // 这一路不可用（例如权限不足），其余继续
            continue;
        }
        RegNotifyChangeKeyValue(keys[i], TRUE,
                                REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET,
                                events[i], TRUE);
    }

    DWORD lastPoll = GetTickCount();

    while (!d->stopRequested.load()) {
        DWORD waitMs = kProcessPollMs;
        const DWORD r = WaitForMultipleObjects(kHandleCount, events, FALSE, waitMs);

        if (r == WAIT_OBJECT_0 + kWakeIndex) break;             // 退出
        if (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + kWatchCount) {
            const int idx = static_cast<int>(r - WAIT_OBJECT_0);
            if (keys[idx]) {
                ResetEvent(events[idx]);
                // 重新挂上通知（RegNotifyChangeKeyValue 会重置事件，必须重新注册才能持续收到）
                const WatchTarget &t = kWatchTargets[idx];
                RegNotifyChangeKeyValue(keys[idx], TRUE,
                                        REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET,
                                        events[idx], TRUE);
            }
            d->dirty = true;
            d->lastChangeTick = GetTickCount();
        }

        // ---- 1. 安装活动侦测（每 2 秒一次）----
        const DWORD now = GetTickCount();
        if (now - lastPoll >= kProcessPollMs) {
            lastPoll = now;
            const QVector<ProcessRef> procs = enumerateProcesses();
            QSet<quint32> alive;
            for (const ProcessRef &p : procs) {
                if (!looksLikeInstaller(p.name.toLower())) continue;
                alive.insert(p.pid);
                if (d->reportedPids.contains(p.pid)) continue;
                d->reportedPids.insert(p.pid);

                MonitorEvent ev;
                ev.time   = QDateTime::currentDateTime();
                ev.kind   = QStringLiteral("安装活动");
                ev.name   = p.name;
                ev.detail = p.path.isEmpty() ? QStringLiteral("PID %1").arg(p.pid)
                                             : QStringLiteral("PID %1 · %2").arg(p.pid).arg(p.path);
                emitEvent(ev);
            }
            // 回收已退出的 PID。不能在遍历 QSet 时 remove，迭代器会失效。
            QSet<quint32> still;
            for (const quint32 pid : d->reportedPids) {
                if (alive.contains(pid)) still.insert(pid);
            }
            d->reportedPids = still;
        }

        // ---- 2. 到点重新扫描并做差集 ----
        const bool needScan = d->pendingBaseline.load()
                              || (d->dirty && now - d->lastChangeTick >= kDebounceMs)
                              || (now - d->lastScanTick >= kIdleRescanMs && !d->baseline.isEmpty());
        if (!needScan) continue;

        d->lastScanTick = now;
        d->dirty = false;
        const bool baselineOnly = d->pendingBaseline.exchange(false);

        QVector<InstalledApp> current;
        try {
            current = RegistryScanner::scanAll();
        } catch (...) {
            QMetaObject::invokeMethod(this, [this]() {
                emit scanFailed(QStringLiteral("扫描注册表时出现异常"));
            }, Qt::QueuedConnection);
            continue;
        }

        if (baselineOnly || d->baseline.isEmpty()) {
            d->baseline = current;
            continue;                     // 首次只建基线，不报"新装了 XX"
        }

        // ---- 3. 差集：按注册表路径（唯一键）比对 ----
        QSet<QString> before, after;
        for (const InstalledApp &a : d->baseline) before.insert(a.id);
        for (const InstalledApp &a : current)      after.insert(a.id);

        for (const InstalledApp &a : current) {
            if (before.contains(a.id)) continue;
            MonitorEvent ev;
            ev.time   = QDateTime::currentDateTime();
            ev.kind   = QStringLiteral("安装完成");
            ev.name   = a.name;
            ev.detail = QStringLiteral("%1 · %2 · %3")
                            .arg(a.version.isEmpty() ? QStringLiteral("未知版本") : a.version,
                                 a.publisher.isEmpty() ? QStringLiteral("未知发布者") : a.publisher,
                                 a.installLocation.isEmpty() ? QStringLiteral("未记录安装目录")
                                                             : a.installLocation);
            ev.app = a;
            emitEvent(ev);
        }
        for (const InstalledApp &a : d->baseline) {
            if (after.contains(a.id)) continue;
            MonitorEvent ev;
            ev.time   = QDateTime::currentDateTime();
            ev.kind   = QStringLiteral("已卸载");
            ev.name   = a.name;
            ev.detail = QStringLiteral("%1 · %2")
                            .arg(a.version.isEmpty() ? QStringLiteral("未知版本") : a.version,
                                 a.registryPath);
            ev.app = a;
            emitEvent(ev);
        }

        d->baseline = current;
    }

    // ---- 清理 ----
    for (int i = 0; i < kWatchCount; ++i) {
        if (keys[i]) RegCloseKey(keys[i]);
        if (events[i]) CloseHandle(events[i]);
    }
    if (d->wakeEvent) { CloseHandle(d->wakeEvent); d->wakeEvent = nullptr; }
}

} // namespace un
