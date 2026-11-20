#include "core/inventory/source.h"
#include "core/registryscanner.h"
#include "core/winutil.h"

namespace un {

class ArpInventorySource final : public IInventorySource {
public:
    InventorySourceId id() const override { return InventorySourceId::Arp; }

    QVector<InventoryRecord> scan(QVector<InventorySourceError> *errors) override
    {
        QVector<InventoryRecord> out;
        try {
            const QVector<InstalledApp> apps = RegistryScanner::scanAll();
            out.reserve(apps.size());
            for (const InstalledApp &app : apps) {
                InventoryRecord r;
                r.source = InventorySourceId::Arp;
                r.trust = InventoryTrust::Authoritative;
                r.category = InventoryCategory::Software;
                r.localId = app.id;
                if (!app.productCode.trimmed().isEmpty())
                    r.identityKey = QStringLiteral("msi:%1").arg(app.productCode.toUpper());
                else
                    r.identityKey = QStringLiteral("arp:%1").arg(app.id);
                r.name = app.name;
                r.version = app.version;
                r.publisher = app.publisher;
                r.installLocation = app.installLocation;
                r.iconSource = app.iconSource;
                r.uninstallString = app.uninstallString;
                r.quietUninstallString = app.quietUninstallString;
                r.productCode = app.productCode;
                r.registryPath = app.registryPath;
                r.installDate = app.installDate;
                r.sizeBytes = app.sizeBytes;
                r.is64View = app.is64View;
                r.systemComponent = app.systemComponent;
                r.isUpdate = app.isUpdate;
                r.packageType = app.type;
                out.push_back(r);
            }
        } catch (const std::exception &ex) {
            if (errors) {
                InventorySourceError e;
                e.source = InventorySourceId::Arp;
                e.message = QString::fromUtf8(ex.what());
                errors->push_back(e);
            }
        } catch (...) {
            if (errors) {
                InventorySourceError e;
                e.source = InventorySourceId::Arp;
                e.message = QStringLiteral("ARP 扫描出现未知异常");
                errors->push_back(e);
            }
        }
        return out;
    }
};

IInventorySource *createArpInventorySource()
{
    return new ArpInventorySource;
}

} // namespace un
