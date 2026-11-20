// ============================================================================
//  whitelist.cpp —— 系统共享组件白名单
// ============================================================================

#include "core/inventory/whitelist.h"
#include "core/winutil.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include <setupapi.h>

namespace un {

namespace {

QStringList protectedPublishers()
{
    return {
        QStringLiteral("microsoft corporation"),
        QStringLiteral("microsoft windows"),
        QStringLiteral("microsoft windows publisher"),
        QStringLiteral("windows publisher"),
    };
}

QStringList protectedNameTokens()
{
    return {
        QStringLiteral("microsoft visual c++"),
        QStringLiteral("microsoft .net"),
        QStringLiteral("windows sdk"),
        QStringLiteral("windows software development kit"),
        QStringLiteral("universal crt"),
        QStringLiteral("directx"),
        QStringLiteral("edge update"),
        QStringLiteral("edgewebview"),
        QStringLiteral("web view"),
        QStringLiteral("runtime"),
        QStringLiteral("redistributable"),
        QStringLiteral("shared frameworks"),
    };
}

} // namespace

bool isInboxDriverInf(const QString &infFileName)
{
    const QString name = QFileInfo(infFileName).fileName().toLower();
    if (name.isEmpty()) return true;
    // 只有 oemNN.inf 才是第三方包；inbox INF 不可卸载
    static const QRegularExpression oem(QStringLiteral("^oem\\d+\\.inf$"),
                                        QRegularExpression::CaseInsensitiveOption);
    return !oem.match(name).hasMatch();
}

bool isCriticalDriverClass(const QString &className)
{
    const QString cn = className.trimmed().toLower();
    if (cn.isEmpty()) return false;
    const QStringList tokens = {
        QStringLiteral("disk"), QStringLiteral("hdc"), QStringLiteral("system"),
        QStringLiteral("computer"), QStringLiteral("keyboard"), QStringLiteral("mouse"),
        QStringLiteral("hidclass"), QStringLiteral("boot"),
    };
    for (const QString &token : tokens) {
        if (cn.contains(token)) return true;
    }
    return false;
}

bool driverInfRemovable(const QString &infFileName, QString *reason)
{
    const QString name = QFileInfo(infFileName).fileName();
    if (isInboxDriverInf(name)) {
        if (reason)
            *reason = QStringLiteral("只允许删除 Windows\\INF 下的 oem*.inf 第三方驱动包");
        return false;
    }

    wchar_t winDir[MAX_PATH] = {};
    if (GetWindowsDirectoryW(winDir, MAX_PATH) == 0) {
        if (reason) *reason = QStringLiteral("无法定位 Windows 目录，已拒绝删除驱动包");
        return false;
    }
    const QString path = QDir::toNativeSeparators(
        QString::fromWCharArray(winDir) + QStringLiteral("\\INF\\") + name);
    if (!QFileInfo::exists(path)) {
        if (reason) *reason = QStringLiteral("驱动包不在系统 INF 目录中：%1").arg(name);
        return false;
    }

    HINF hInf = SetupOpenInfFileW(path.toStdWString().c_str(), nullptr, INF_STYLE_WIN4, nullptr);
    if (hInf == INVALID_HANDLE_VALUE) {
        if (reason) *reason = QStringLiteral("无法读取驱动信息，已拒绝删除：%1").arg(name);
        return false;
    }
    QString className;
    INFCONTEXT ctx{};
    if (SetupFindFirstLineW(hInf, L"Version", L"Class", &ctx)) {
        wchar_t buf[512] = {};
        if (SetupGetStringFieldW(&ctx, 1, buf, 512, nullptr))
            className = QString::fromWCharArray(buf);
    }
    SetupCloseInfFile(hInf);

    if (className.trimmed().isEmpty()) {
        if (reason) *reason = QStringLiteral("读不到设备类，已拒绝删除：%1").arg(name);
        return false;
    }
    if (isCriticalDriverClass(className)) {
        if (reason)
            *reason = QStringLiteral("关键设备类「%1」，已拒绝删除 %2").arg(className, name);
        return false;
    }
    return true;
}

bool isInventoryWhitelisted(const QString &name,
                            const QString &publisher,
                            const QString &installLocation,
                            const QString &productOrInf)
{
    // 只在参数看起来是 INF 文件名时才套用收件箱驱动规则。
    // 产品码 GUID、AppX 全名不是 oem*.inf，不能因此整条拉黑。
    const QString infName = QFileInfo(productOrInf).fileName();
    if (infName.endsWith(QStringLiteral(".inf"), Qt::CaseInsensitive)
        && isInboxDriverInf(infName))
        return true;

    if (!installLocation.trimmed().isEmpty()) {
        if (isDangerousPath(installLocation))
            return true;

        const QString loc = QDir::fromNativeSeparators(installLocation).toLower();
        if (loc.contains(QStringLiteral("/windowsapps"))
            || loc.contains(QStringLiteral("/driverstore"))
            || loc.contains(QStringLiteral("/winsxs"))
            || loc.contains(QStringLiteral("/system32"))
            || loc.contains(QStringLiteral("/syswow64")))
            return true;
    }

    const QString pub = publisher.trimmed().toLower();
    for (const QString &p : protectedPublishers()) {
        if (!pub.isEmpty() && pub.contains(p)) {
            // 微软发布者本身不一定白名单；结合运行库类名称才拦
            const QString n = name.trimmed().toLower();
            for (const QString &token : protectedNameTokens()) {
                if (n.contains(token)) return true;
            }
            break;
        }
    }

    const QString n = name.trimmed().toLower();
    for (const QString &token : protectedNameTokens()) {
        if (n.contains(token)) return true;
    }

    return false;
}

} // namespace un
