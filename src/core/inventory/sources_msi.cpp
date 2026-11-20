// ============================================================================
//  sources_msi.cpp —— MSI 产品枚举（MsiEnumProductsEx，禁止 Win32_Product）
// ============================================================================

#include "core/inventory/source.h"
#include "core/winutil.h"

#include <QDate>
#include <QDir>

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <msi.h>
#include <msiquery.h>

#include <vector>

#pragma comment(lib, "msi.lib")

namespace un {

namespace {

QDate parseYmdInstallDate(const QString &raw)
{
    const QString s = raw.trimmed();
    if (s.size() != 8) return QDate();
    const QDate d = QDate::fromString(s, QStringLiteral("yyyyMMdd"));
    return d.isValid() ? d : QDate();
}

QString msiProp(const wchar_t *product, const wchar_t *userSid, MSIINSTALLCONTEXT ctx,
                const wchar_t *prop)
{
    DWORD n = 0;
    const UINT need = MsiGetProductInfoExW(product, userSid, ctx, prop, nullptr, &n);
    if (need != ERROR_SUCCESS && need != ERROR_MORE_DATA) return {};
    if (n == 0) return {};
    std::vector<wchar_t> buf(n + 1, L'\0');
    DWORD size = n;
    if (MsiGetProductInfoExW(product, userSid, ctx, prop, buf.data(), &size) != ERROR_SUCCESS)
        return {};
    return QString::fromWCharArray(buf.data());
}

void appendProduct(const wchar_t *product, const wchar_t *sid, MSIINSTALLCONTEXT ctx,
                   QVector<InventoryRecord> *out)
{
    InventoryRecord r;
    r.source = InventorySourceId::Msi;
    r.trust = InventoryTrust::Authoritative;
    r.category = InventoryCategory::Software;
    r.packageType = PackageType::Msi;
    r.productCode = QString::fromWCharArray(product).toUpper();
    r.localId = QStringLiteral("msi:%1").arg(r.productCode);
    r.identityKey = r.localId;
    r.name = msiProp(product, sid, ctx, INSTALLPROPERTY_INSTALLEDPRODUCTNAME);
    if (r.name.isEmpty())
        r.name = msiProp(product, sid, ctx, INSTALLPROPERTY_PRODUCTNAME);
    r.version = msiProp(product, sid, ctx, INSTALLPROPERTY_VERSIONSTRING);
    r.publisher = msiProp(product, sid, ctx, INSTALLPROPERTY_PUBLISHER);
    r.installLocation = QDir::toNativeSeparators(
        unquote(expandEnv(msiProp(product, sid, ctx, INSTALLPROPERTY_INSTALLLOCATION))));
    r.installDate = parseYmdInstallDate(
        msiProp(product, sid, ctx, INSTALLPROPERTY_INSTALLDATE));
    r.uninstallString = QStringLiteral("msiexec.exe /X%1").arg(r.productCode);
    r.quietUninstallString =
        QStringLiteral("msiexec.exe /X%1 /qn /norestart").arg(r.productCode);
    if (!r.name.trimmed().isEmpty())
        out->push_back(r);
}

void enumContext(MSIINSTALLCONTEXT ctx, QVector<InventoryRecord> *out,
                 QVector<InventorySourceError> *errors)
{
    for (DWORD index = 0;; ++index) {
        wchar_t product[39] = {};
        MSIINSTALLCONTEXT installedCtx = MSIINSTALLCONTEXT_NONE;
        wchar_t sidBuf[256] = {};
        DWORD sidLen = static_cast<DWORD>(sizeof(sidBuf) / sizeof(sidBuf[0]));

        const UINT rc = MsiEnumProductsExW(nullptr, nullptr, ctx, index, product,
                                          &installedCtx, sidBuf, &sidLen);
        if (rc == ERROR_NO_MORE_ITEMS) break;
        if (rc == ERROR_ACCESS_DENIED) {
            if (errors) {
                InventorySourceError e;
                e.source = InventorySourceId::Msi;
                e.accessDenied = true;
                e.message = QStringLiteral("枚举 MSI 产品时权限不足");
                errors->push_back(e);
            }
            break;
        }
        if (rc != ERROR_SUCCESS) continue;

        const wchar_t *sid = (sidLen > 0 && sidBuf[0] != L'\0') ? sidBuf : nullptr;
        appendProduct(product, sid, installedCtx, out);
    }
}

} // namespace

class MsiInventorySource final : public IInventorySource {
public:
    InventorySourceId id() const override { return InventorySourceId::Msi; }

    QVector<InventoryRecord> scan(QVector<InventorySourceError> *errors) override
    {
        QVector<InventoryRecord> out;
        try {
            enumContext(MSIINSTALLCONTEXT_MACHINE, &out, errors);
            enumContext(MSIINSTALLCONTEXT_USERMANAGED, &out, errors);
            enumContext(MSIINSTALLCONTEXT_USERUNMANAGED, &out, errors);
        } catch (const std::exception &ex) {
            if (errors) {
                InventorySourceError e;
                e.source = InventorySourceId::Msi;
                e.message = QString::fromUtf8(ex.what());
                errors->push_back(e);
            }
        } catch (...) {
            if (errors) {
                InventorySourceError e;
                e.source = InventorySourceId::Msi;
                e.message = QStringLiteral("MSI 枚举出现未知异常");
                errors->push_back(e);
            }
        }
        return out;
    }
};

IInventorySource *createMsiInventorySource()
{
    return new MsiInventorySource;
}

} // namespace un
