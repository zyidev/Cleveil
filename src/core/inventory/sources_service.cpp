// ============================================================================
//  sources_service.cpp —— SCM 服务枚举
//  ImagePath 能挂到软件安装目录的不单独成条（由合并层处理）；
//  这里输出全部服务记录，合并时再区分挂靠 / 孤儿。
// ============================================================================

#include "core/inventory/source.h"
#include "core/inventory/whitelist.h"
#include "core/winutil.h"

#include <QDir>
#include <QFileInfo>

namespace un {

namespace {

QString extractExePath(const QString &commandLine)
{
    QString s = commandLine.trimmed();
    if (s.isEmpty()) return {};
    if (s.startsWith(QLatin1Char('"'))) {
        const int end = s.indexOf(QLatin1Char('"'), 1);
        if (end > 1) return s.mid(1, end - 1);
    }
    const QString lower = s.toLower();
    const int exePos = lower.indexOf(QStringLiteral(".exe"));
    if (exePos > 0) return s.left(exePos + 4);
    const int space = s.indexOf(QLatin1Char(' '));
    return space > 0 ? s.left(space) : s;
}

} // namespace

class ServiceInventorySource final : public IInventorySource {
public:
    InventorySourceId id() const override { return InventorySourceId::Service; }

    QVector<InventoryRecord> scan(QVector<InventorySourceError> *errors) override
    {
        QVector<InventoryRecord> out;
        try {
            const QVector<ServiceRef> services = enumerateServices();
            out.reserve(services.size());
            for (const ServiceRef &svc : services) {
                if (svc.name.trimmed().isEmpty()) continue;

                const QString exe = QDir::toNativeSeparators(
                    expandEnv(extractExePath(svc.binaryPath)));
                const QString dir = QFileInfo(exe).absolutePath();

                InventoryRecord r;
                r.source = InventorySourceId::Service;
                r.trust = InventoryTrust::Authoritative;
                r.category = InventoryCategory::OrphanService;
                r.packageType = PackageType::Service;
                r.localId = QStringLiteral("svc:%1").arg(svc.name.toLower());
                r.identityKey = r.localId;
                r.productCode = svc.name;
                r.name = svc.displayName.isEmpty() ? svc.name : svc.displayName;
                r.installLocation = isDangerousPath(dir) ? QString() : QDir::toNativeSeparators(dir);
                r.uninstallString = QStringLiteral("sc delete %1").arg(svc.name);
                r.iconSource = exe;
                r.note = svc.binaryPath;
                r.whitelistBlocked = isInventoryWhitelisted(r.name, QString(), exe)
                                     || isProtectedProcessName(QFileInfo(exe).fileName())
                                     || exe.toLower().contains(QStringLiteral("\\system32\\svchost.exe"));
                if (r.whitelistBlocked) {
                    r.systemComponent = true;
                    r.note = QStringLiteral("系统服务，禁止删除");
                }
                // 合并层：若 installLocation 非空且落在某软件目录下，会挂靠而不是孤儿
                if (!r.installLocation.isEmpty())
                    r.identityKey = QStringLiteral("path:%1").arg(
                        QDir::fromNativeSeparators(r.installLocation).toLower());
                out.push_back(r);
            }
        } catch (const std::exception &ex) {
            if (errors) {
                InventorySourceError e;
                e.source = InventorySourceId::Service;
                e.message = QString::fromUtf8(ex.what());
                errors->push_back(e);
            }
        } catch (...) {
            if (errors) {
                InventorySourceError e;
                e.source = InventorySourceId::Service;
                e.message = QStringLiteral("服务枚举出现未知异常");
                errors->push_back(e);
            }
        }
        return out;
    }
};

IInventorySource *createServiceInventorySource()
{
    return new ServiceInventorySource;
}

} // namespace un
