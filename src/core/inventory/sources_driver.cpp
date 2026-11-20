// ============================================================================
//  sources_driver.cpp —— PnP OEM 驱动包（SetupAPI + DriverStore 位置）
//  只枚举 oem*.inf；设备节点不作为可卸载程序条目。
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
#include <setupapi.h>

#include <vector>

#pragma comment(lib, "setupapi.lib")

namespace un {

namespace {

QString driverStoreLocation(const QString &infPath)
{
    // SetupGetInfDriverStoreLocationW：把 INF 映射到 DriverStore\FileRepository
    const std::wstring winf = QDir::toNativeSeparators(infPath).toStdWString();
    DWORD need = 0;
    SetupGetInfDriverStoreLocationW(winf.c_str(), nullptr, nullptr, nullptr, 0, &need);
    if (need == 0) return {};
    std::vector<wchar_t> buf(need + 1, L'\0');
    if (!SetupGetInfDriverStoreLocationW(winf.c_str(), nullptr, nullptr, buf.data(), need, &need))
        return {};
    return QDir::toNativeSeparators(QString::fromWCharArray(buf.data()));
}

QString infString(HINF hInf, const wchar_t *key)
{
    INFCONTEXT ctx{};
    if (!SetupFindFirstLineW(hInf, L"Version", key, &ctx)) return {};
    wchar_t buf[512] = {};
    if (!SetupGetStringFieldW(&ctx, 1, buf, 512, nullptr)) return {};
    return QString::fromWCharArray(buf);
}

} // namespace

class DriverInventorySource final : public IInventorySource {
public:
    InventorySourceId id() const override { return InventorySourceId::DriverPackage; }

    QVector<InventoryRecord> scan(QVector<InventorySourceError> *errors) override
    {
        QVector<InventoryRecord> out;
        try {
            wchar_t winDir[MAX_PATH] = {};
            if (GetWindowsDirectoryW(winDir, MAX_PATH) == 0) {
                if (errors) {
                    InventorySourceError e;
                    e.source = InventorySourceId::DriverPackage;
                    e.message = QStringLiteral("无法定位 Windows 目录");
                    errors->push_back(e);
                }
                return out;
            }

            const QString infDir = QDir::toNativeSeparators(
                QString::fromWCharArray(winDir) + QStringLiteral("\\INF"));
            QDir dir(infDir);
            const QFileInfoList oemFiles = dir.entryInfoList({QStringLiteral("oem*.inf")},
                                                             QDir::Files | QDir::Readable);

            for (const QFileInfo &fi : oemFiles) {
                if (isInboxDriverInf(fi.fileName())) continue;

                const QString path = QDir::toNativeSeparators(fi.absoluteFilePath());
                HINF hInf = SetupOpenInfFileW(path.toStdWString().c_str(), nullptr,
                                             INF_STYLE_WIN4, nullptr);
                if (hInf == INVALID_HANDLE_VALUE) continue;

                const QString className = infString(hInf, L"Class");
                const QString provider = infString(hInf, L"Provider");
                const QString driverVer = infString(hInf, L"DriverVer");
                const QString catalog = infString(hInf, L"CatalogFile");
                SetupCloseInfFile(hInf);

                const QString store = driverStoreLocation(path);
                InventoryRecord r;
                r.source = InventorySourceId::DriverPackage;
                r.trust = InventoryTrust::Authoritative;
                r.category = InventoryCategory::DriverPackage;
                r.packageType = PackageType::Driver;
                r.localId = QStringLiteral("drv:%1").arg(fi.fileName().toLower());
                r.identityKey = r.localId;
                r.productCode = fi.fileName();
                r.name = className.isEmpty()
                             ? fi.fileName()
                             : QStringLiteral("%1 (%2)").arg(className, fi.fileName());
                r.publisher = provider;
                r.version = driverVer;
                r.installLocation = store.isEmpty() ? path : store;
                r.iconSource = path;
                // 不把 pnputil 命令放进记录。真正删除由 Uninstaller::buildPlan 现编，
                // 并在执行前再次打开 INF 核对设备类。
                r.note = catalog.isEmpty()
                             ? QStringLiteral("OEM 驱动包（设备节点请在设备管理器中查看）")
                             : QStringLiteral("OEM 驱动包 · 目录 %1").arg(catalog);
                if (isCriticalDriverClass(className)) {
                    r.whitelistBlocked = true;
                    r.systemComponent = true;
                    r.note += QStringLiteral(" · 关键设备类，禁止卸载");
                }
                out.push_back(r);
            }
        } catch (const std::exception &ex) {
            if (errors) {
                InventorySourceError e;
                e.source = InventorySourceId::DriverPackage;
                e.message = QString::fromUtf8(ex.what());
                errors->push_back(e);
            }
        } catch (...) {
            if (errors) {
                InventorySourceError e;
                e.source = InventorySourceId::DriverPackage;
                e.message = QStringLiteral("驱动包枚举出现未知异常");
                errors->push_back(e);
            }
        }
        return out;
    }
};

IInventorySource *createDriverInventorySource()
{
    return new DriverInventorySource;
}

} // namespace un
