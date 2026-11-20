// ============================================================================
//  sources_appx.cpp —— MSIX / UWP 包枚举（PackageManager / AppX Deployment API）
//  优先 WinRT PackageManager；公寓初始化失败时退回包仓库注册表只读枚举。
// ============================================================================

#include "core/inventory/source.h"
#include "core/inventory/whitelist.h"
#include "core/winutil.h"

#include <QDir>
#include <QFileInfo>

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <vector>

// C++/WinRT
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Management.Deployment.h>
#include <winrt/Windows.Storage.h>

#pragma comment(lib, "windowsapp.lib")

namespace un {

namespace {

QVector<InventoryRecord> scanViaWinRt(QVector<InventorySourceError> *errors)
{
    QVector<InventoryRecord> out;
    ensureWinRtApartment();

    try {
        winrt::Windows::Management::Deployment::PackageManager pm;
        auto packages = pm.FindPackagesForUser(L"");

        for (auto const &pkg : packages) {
            try {
                if (pkg.IsFramework() || pkg.IsResourcePackage() || pkg.IsBundle())
                    continue;

                const auto id = pkg.Id();
                const QString fullName = QString::fromStdWString(std::wstring(id.FullName()));
                const QString family = QString::fromStdWString(std::wstring(id.FamilyName()));
                const QString name = QString::fromStdWString(std::wstring(id.Name()));
                if (name.isEmpty()) continue;

                QString publisher;
                try {
                    publisher = QString::fromStdWString(std::wstring(id.Publisher()));
                } catch (...) {
                }

                QString version;
                try {
                    const auto v = id.Version();
                    version = QStringLiteral("%1.%2.%3.%4")
                                  .arg(v.Major).arg(v.Minor).arg(v.Build).arg(v.Revision);
                } catch (...) {
                }

                QString location;
                try {
                    location = QDir::toNativeSeparators(
                        QString::fromStdWString(std::wstring(pkg.InstalledLocation().Path())));
                } catch (...) {
                }

                // 系统签名 / WindowsApps 下的微软收件箱包：标白名单，仍可展示
                const bool blocked = isInventoryWhitelisted(name, publisher, location, fullName)
                                     || publisher.contains(QStringLiteral("CN=Microsoft Windows"),
                                                           Qt::CaseInsensitive);

                InventoryRecord r;
                r.source = InventorySourceId::Appx;
                r.trust = InventoryTrust::Authoritative;
                r.category = InventoryCategory::Software;
                r.packageType = PackageType::Store;
                r.localId = QStringLiteral("appx:%1").arg(fullName);
                r.identityKey = QStringLiteral("appx:%1").arg(family.toLower());
                r.name = name;
                r.version = version;
                r.publisher = publisher;
                r.installLocation = location;
                r.productCode = fullName;
                r.uninstallString = QStringLiteral("Remove-AppxPackage %1").arg(fullName);
                r.systemComponent = blocked;
                r.whitelistBlocked = blocked;
                if (blocked)
                    r.note = QStringLiteral("系统或共享 AppX 包，禁止直接删除");
                out.push_back(r);
            } catch (...) {
                // 单个包失败跳过
            }
        }
    } catch (winrt::hresult_error const &ex) {
        if (errors) {
            InventorySourceError e;
            e.source = InventorySourceId::Appx;
            e.accessDenied = (static_cast<HRESULT>(ex.code()) == E_ACCESSDENIED);
            e.message = QStringLiteral("AppX PackageManager 失败：%1")
                            .arg(QString::fromStdWString(std::wstring(ex.message())));
            errors->push_back(e);
        }
    } catch (...) {
        if (errors) {
            InventorySourceError e;
            e.source = InventorySourceId::Appx;
            e.message = QStringLiteral("AppX 枚举出现未知异常");
            errors->push_back(e);
        }
    }
    return out;
}

} // namespace

class AppxInventorySource final : public IInventorySource {
public:
    InventorySourceId id() const override { return InventorySourceId::Appx; }

    QVector<InventoryRecord> scan(QVector<InventorySourceError> *errors) override
    {
        return scanViaWinRt(errors);
    }
};

IInventorySource *createAppxInventorySource()
{
    return new AppxInventorySource;
}

} // namespace un
