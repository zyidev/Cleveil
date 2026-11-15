// ============================================================================
//  uninstaller.cpp —— 卸载命令拼装与异步执行
// ============================================================================

#include "core/uninstaller.h"
#include "core/winutil.h"

#include <QDir>
#include <QProcess>
#include <QRegularExpression>   // 从卸载命令里抓产品码 GUID
#include <QTimer>

#include <shellapi.h>      // ShellExecuteExW / SEE_MASK_*
#include <string>

namespace un {

namespace {

QString quoted(const QString &arg)
{
    if (arg.isEmpty()) return QStringLiteral("\"\"");
    if (arg.contains(QLatin1Char(' '))) {
        QString s = arg;
        s.replace(QLatin1String("\""), QLatin1String("\\\""));
        return QStringLiteral("\"%1\"").arg(s);
    }
    return arg;
}

QString joinCommand(const QString &program, const QStringList &args)
{
    QStringList parts;
    parts << quoted(QDir::toNativeSeparators(program));
    for (const QString &a : args) parts << quoted(a);
    return parts.join(QLatin1Char(' '));
}

} // namespace

// ---------------------------------------------------------------------------
// 命令行拆分：逐个字符扫描，遇到引号切换 inQuotes 状态
// ---------------------------------------------------------------------------
bool Uninstaller::splitCommandLine(const QString &cmdLine, QString *program, QStringList *args)
{
    const QString s = cmdLine.trimmed();
    if (s.isEmpty()) return false;

    QStringList parts;
    QString cur;
    bool inQuotes = false;

    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (c == QLatin1Char('"')) {
            inQuotes = !inQuotes;
            continue;
        }
        if (!inQuotes && (c == QLatin1Char(' ') || c == QLatin1Char('\t'))) {
            if (!cur.isEmpty()) { parts << cur; cur.clear(); }
            continue;
        }
        cur.append(c);
    }
    if (!cur.isEmpty()) parts << cur;
    if (parts.isEmpty()) return false;

    if (program) *program = parts.first();
    if (args)    *args = parts.mid(1);
    return true;
}

bool Uninstaller::likelyNeedsAdmin(const InstalledApp &app)
{
    // msiexec 的静默模式（/qn）必须要有管理员权限才能跑通，
    // 其它安装器一般会自己弹 UAC，不需要我们代劳。
    return app.type == PackageType::Msi || app.uninstallString.contains(
        QStringLiteral("msiexec"), Qt::CaseInsensitive);
}

bool msiProductInstalled(const QString &productCode)
{
    // 动态取 msi.dll 的导出，避免为一个查询去改链接配置
    typedef int (WINAPI *QueryStateFn)(LPCWSTR);
    static QueryStateFn fn = nullptr;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        if (HMODULE msi = LoadLibraryW(L"msi.dll"))
            fn = reinterpret_cast<QueryStateFn>(GetProcAddress(msi, "MsiQueryProductStateW"));
    }
    if (!fn || productCode.trimmed().isEmpty()) return false;

    // INSTALLSTATE：5 = 默认已安装，3 = 本地已安装，其余（1 未安装 / -1 未知）都不算
    const int state = fn(productCode.toStdWString().c_str());
    return state == 5 || state == 3;
}

// ---------------------------------------------------------------------------
// 卸载计划拼装
// ---------------------------------------------------------------------------
UninstallPlan Uninstaller::buildPlan(const InstalledApp &app, bool silent)
{
    UninstallPlan plan;
    plan.silent     = silent;
    plan.needsAdmin = likelyNeedsAdmin(app);

    const QString normalCmd = app.uninstallString.trimmed();
    const QString quietCmd  = app.quietUninstallString.trimmed();

    // ---------- 情况一：MSI 安装包，统一走 msiexec /X{产品码} ----------
    const bool msiLike = app.type == PackageType::Msi
                         || normalCmd.contains(QStringLiteral("msiexec"), Qt::CaseInsensitive)
                         || quietCmd.contains(QStringLiteral("msiexec"), Qt::CaseInsensitive);

    if (msiLike) {
        // 候选产品码：键名里的 GUID + 卸载命令里出现的 GUID
        // （cached uninstaller 的路径形如 C:\Windows\Installer\{GUID}\uninstall.exe，那个 GUID 就是产品码）。
        // 逐个验"是否真的装着"，取第一个命中的；全都不命中说明这条登记项是僵尸项。
        QStringList codes;
        if (!app.productCode.trimmed().isEmpty()) codes << app.productCode.trimmed();

        static const QRegularExpression kGuid(
            QStringLiteral("\\{[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-"
                           "[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\\}"));
        for (const QString &cmd : { normalCmd, quietCmd }) {
            auto it = kGuid.globalMatch(cmd);
            while (it.hasNext()) {
                const QString g = it.next().captured(0);
                if (!codes.contains(g)) codes << g;
            }
        }

        QString installedCode;
        for (const QString &c : codes) {
            if (msiProductInstalled(c)) { installedCode = c; break; }
        }

        if (!installedCode.isEmpty()) {
            plan.program = QStringLiteral("msiexec.exe");
            plan.arguments << QStringLiteral("/X%1").arg(installedCode);
            if (silent) {
                // /qn = 完全无界面，/norestart = 不要自动重启电脑
                plan.arguments << QStringLiteral("/qn") << QStringLiteral("/norestart");
            }
            plan.valid          = true;
            plan.displayCommand = joinCommand(plan.program, plan.arguments);
            return plan;
        }
        // 没有可用产品码 → 落到下面的"用登记项自己的卸载命令"分支
    }

    // ---------- 情况二：程序自带卸载器 ----------
    QString cmd;
    if (silent && !quietCmd.isEmpty())      cmd = quietCmd;       // 有官方静默命令，最优
    else if (!normalCmd.isEmpty())          cmd = normalCmd;
    else if (!quietCmd.isEmpty())           cmd = quietCmd;       // 只有静默命令，当普通卸载用

    if (cmd.isEmpty()) {
        plan.reason = QStringLiteral("该程序没有在注册表中登记卸载命令，只能使用\"强制卸载\"清理。");
        return plan;
    }

    QString program;
    QStringList args;
    if (!splitCommandLine(cmd, &program, &args)) {
        plan.reason = QStringLiteral("卸载命令无法解析：%1").arg(cmd);
        return plan;
    }

    program = expandEnv(unquote(program));

    // 静默模式但没有静默命令：老实告诉用户"这个程序不认静默参数"
    if (silent && quietCmd.isEmpty() && !msiLike) plan.unsupportedSilent = true;

    plan.program        = program;
    plan.arguments      = args;
    plan.valid          = true;
    plan.displayCommand = joinCommand(program, args);
    return plan;
}

// ===========================================================================
//  UninstallRunner —— 异步执行
// ===========================================================================

UninstallRunner::UninstallRunner(QObject *parent)
    : QObject(parent)
{
    m_timer = new QTimer(this);
    m_timer->setInterval(300);
    connect(m_timer, &QTimer::timeout, this, &UninstallRunner::pollElevatedProcess);
}

UninstallRunner::~UninstallRunner()
{
    cleanup();
}

void UninstallRunner::cleanup()
{
    if (m_proc) {
        m_proc->disconnect(this);
        if (m_proc->state() != QProcess::NotRunning)
            m_proc->kill();
        m_proc->deleteLater();
        m_proc = nullptr;
    }
    if (m_timer) m_timer->stop();
    if (m_processHandle) {
        CloseHandle(reinterpret_cast<HANDLE>(m_processHandle));
        m_processHandle = nullptr;
    }
}

void UninstallRunner::run(const UninstallPlan &plan, bool elevate)
{
    cleanup();
    m_finished = false;

    if (!plan.valid) {
        m_finished = true;
        emit finished(false, 0, plan.reason);
        return;
    }

    if (elevate) {
        startElevated(plan);
        return;
    }

    m_proc = new QProcess(this);
    m_proc->setProgram(plan.program);
    m_proc->setArguments(plan.arguments);
    m_proc->setProcessChannelMode(QProcess::MergedChannels);

    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus status) {
                Q_UNUSED(status)
                if (m_finished) return;
                m_finished = true;
                // 不要在这个回调里销毁 QProcess：调用方如果正停在 waitFor* 上会踩空对象
                emit finished(true, static_cast<quint32>(code), QString());
            });

    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError err) {
        // FailedToStart 是"没起来"，要单独报；其它错误（如崩溃）等 finished 信号即可
        if (err == QProcess::FailedToStart) {
            if (m_finished) return;
            m_finished = true;
            const QString reason = m_proc ? m_proc->errorString() : QStringLiteral("未知原因");
            emit finished(false, 0, QStringLiteral("无法启动卸载程序：%1").arg(reason));
        }
    });

    m_proc->start();
}

void UninstallRunner::startElevated(const UninstallPlan &plan)
{
    const std::wstring wprogram = QDir::toNativeSeparators(plan.program).toStdWString();

    QString argLine;
    if (!plan.arguments.isEmpty()) {
        QStringList parts;
        for (const QString &a : plan.arguments) parts << quoted(a);
        argLine = parts.join(QLatin1Char(' '));
    }
    const std::wstring wargs = argLine.toStdWString();

    SHELLEXECUTEINFOW sei{};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb       = L"runas";
    sei.lpFile       = wprogram.c_str();
    sei.lpParameters = argLine.isEmpty() ? nullptr : wargs.c_str();
    sei.nShow        = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        const DWORD code = GetLastError();
        m_finished = true;
        emit finished(false, 0,
                      code == ERROR_CANCELLED
                          ? QStringLiteral("已取消管理员授权")
                          : QStringLiteral("以管理员身份启动失败（错误码 %1）").arg(code));
        return;
    }

    m_processHandle = sei.hProcess;
    if (!m_processHandle) {
        // 有些系统会把进程直接接管，拿不到句柄，只能认为已成功交出
        m_finished = true;
        emit finished(true, 0, QString());
        return;
    }

    m_timer->start();   // 改为轮询等待，不阻塞界面
}

void UninstallRunner::pollElevatedProcess()
{
    if (!m_processHandle || m_finished) return;

    DWORD code = 0;
    if (!GetExitCodeProcess(reinterpret_cast<HANDLE>(m_processHandle), &code)) {
        m_timer->stop();
        m_finished = true;
        cleanup();
        emit finished(false, 0, QStringLiteral("无法获取卸载进程状态"));
        return;
    }

    if (code == STILL_ACTIVE) return;   // 还在跑，下次再看

    m_timer->stop();
    m_finished = true;
    cleanup();
    emit finished(true, static_cast<quint32>(code), QString());
}

void UninstallRunner::terminate()
{
    // 只发结束请求。finished 由 QProcess 或轮询回调发出，避免在这里同步等待把界面卡死。
    if (m_proc && m_proc->state() != QProcess::NotRunning)
        m_proc->kill();
    if (m_processHandle)
        TerminateProcess(reinterpret_cast<HANDLE>(m_processHandle), 1);
}

} // namespace un
