// ============================================================================
//  sources_installer.cpp —— Inno / NSIS / InstallShield 自带卸载文件
//  只扫 Program Files / LocalAppData\Programs 的一层子目录，不做全盘递归。
// ============================================================================

#include "core/inventory/source.h"
#include "core/inventory/whitelist.h"
#include "core/winutil.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

namespace un {

namespace {

QString readVersionString(const QString &exePath, const wchar_t *key)
{
    const std::wstring wpath = QDir::toNativeSeparators(exePath).toStdWString();
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(wpath.c_str(), &handle);
    if (size == 0) return {};

    QByteArray buf(static_cast<int>(size), 0);
    if (!GetFileVersionInfoW(wpath.c_str(), 0, size, buf.data())) return {};

    struct LANGANDCODEPAGE { WORD wLanguage; WORD wCodePage; };
    LANGANDCODEPAGE *translate = nullptr;
    UINT translateLen = 0;
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<LPVOID *>(&translate), &translateLen)
        || !translate || translateLen < sizeof(LANGANDCODEPAGE))
        return {};

    const QString sub = QStringLiteral("\\StringFileInfo\\%1%2\\%3")
                            .arg(translate[0].wLanguage, 4, 16, QLatin1Char('0'))
                            .arg(translate[0].wCodePage, 4, 16, QLatin1Char('0'))
                            .arg(QString::fromWCharArray(key));
    wchar_t *value = nullptr;
    UINT valueLen = 0;
    if (!VerQueryValueW(buf.data(), sub.toStdWString().c_str(),
                        reinterpret_cast<LPVOID *>(&value), &valueLen)
        || !value)
        return {};
    return QString::fromWCharArray(value);
}

enum class VendorKind { None, Inno, Nsis, InstallShield };

VendorKind detectVendor(const QDir &dir, QString *uninstaller)
{
    // Inno Setup：unins000.exe + unins000.dat
    if (dir.exists(QStringLiteral("unins000.exe")) && dir.exists(QStringLiteral("unins000.dat"))) {
        *uninstaller = dir.filePath(QStringLiteral("unins000.exe"));
        return VendorKind::Inno;
    }

    const QFileInfoList exes = dir.entryInfoList({QStringLiteral("*.exe")}, QDir::Files);
    for (const QFileInfo &fi : exes) {
        const QString name = fi.fileName().toLower();
        const QString path = fi.absoluteFilePath();

        // NSIS：卸载程序名常见 uninst.exe / uninstall.exe，公司名含 Nullsoft
        if (name.contains(QStringLiteral("uninst")) || name == QStringLiteral("uninstall.exe")) {
            const QString company = readVersionString(path, L"CompanyName");
            const QString product = readVersionString(path, L"ProductName");
            if (company.contains(QStringLiteral("Nullsoft"), Qt::CaseInsensitive)
                || product.contains(QStringLiteral("Nullsoft"), Qt::CaseInsensitive)
                || company.contains(QStringLiteral("NSIS"), Qt::CaseInsensitive))
            {
                *uninstaller = path;
                return VendorKind::Nsis;
            }

            // InstallShield：常见 InstallShield 字符串
            if (company.contains(QStringLiteral("InstallShield"), Qt::CaseInsensitive)
                || product.contains(QStringLiteral("InstallShield"), Qt::CaseInsensitive)
                || name.contains(QStringLiteral("uninstall")))
            {
                // 再看同目录是否有 InstallShield 特征文件
                if (dir.exists(QStringLiteral("InstallShield Installation Information"))
                    || company.contains(QStringLiteral("InstallShield"), Qt::CaseInsensitive))
                {
                    *uninstaller = path;
                    return VendorKind::InstallShield;
                }
            }
        }
    }

    // InstallShield 传统布局
    if (QDir(dir.filePath(QStringLiteral("InstallShield Installation Information"))).exists()) {
        for (const QFileInfo &fi : exes) {
            if (fi.fileName().toLower().contains(QStringLiteral("uninst"))) {
                *uninstaller = fi.absoluteFilePath();
                return VendorKind::InstallShield;
            }
        }
    }
    return VendorKind::None;
}

void scanRoot(const QString &root, QVector<InventoryRecord> *out)
{
    if (root.isEmpty() || !QDir(root).exists()) return;
    QDir base(root);
    const QFileInfoList dirs = base.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &di : dirs) {
        const QString full = QDir::toNativeSeparators(di.absoluteFilePath());
        if (isDangerousPath(full) || isInventoryWhitelisted(di.fileName(), {}, full))
            continue;

        QDir child(full);
        QString uninstaller;
        const VendorKind kind = detectVendor(child, &uninstaller);
        if (kind == VendorKind::None || uninstaller.isEmpty()) continue;

        InventoryRecord r;
        r.source = InventorySourceId::InstallerFile;
        r.trust = InventoryTrust::Corroborating;
        r.category = InventoryCategory::Software;
        r.packageType = PackageType::Exe;
        r.localId = QStringLiteral("vendor:%1").arg(full.toLower());
        r.identityKey = QStringLiteral("path:%1").arg(
            QDir::fromNativeSeparators(full).toLower());
        r.name = di.fileName();
        r.installLocation = full;
        r.uninstallString = QStringLiteral("\"%1\"").arg(
            QDir::toNativeSeparators(uninstaller));
        r.iconSource = uninstaller;
        switch (kind) {
        case VendorKind::Inno:
            r.note = QStringLiteral("检测到 Inno Setup 卸载程序");
            r.quietUninstallString = r.uninstallString + QStringLiteral(" /VERYSILENT /NORESTART");
            break;
        case VendorKind::Nsis:
            r.note = QStringLiteral("检测到 NSIS 卸载程序");
            r.quietUninstallString = r.uninstallString + QStringLiteral(" /S");
            break;
        case VendorKind::InstallShield:
            r.note = QStringLiteral("检测到 InstallShield 卸载程序");
            break;
        default:
            break;
        }
        r.whitelistBlocked = isInventoryWhitelisted(r.name, r.publisher, r.installLocation);
        out->push_back(r);
    }
}

} // namespace

class InstallerFileInventorySource final : public IInventorySource {
public:
    InventorySourceId id() const override { return InventorySourceId::InstallerFile; }

    QVector<InventoryRecord> scan(QVector<InventorySourceError> *errors) override
    {
        QVector<InventoryRecord> out;
        try {
            scanRoot(expandEnv(QStringLiteral("%ProgramFiles%")), &out);
            scanRoot(expandEnv(QStringLiteral("%ProgramFiles(x86)%")), &out);
            scanRoot(expandEnv(QStringLiteral("%LOCALAPPDATA%\\Programs")), &out);
        } catch (const std::exception &ex) {
            if (errors) {
                InventorySourceError e;
                e.source = InventorySourceId::InstallerFile;
                e.message = QString::fromUtf8(ex.what());
                errors->push_back(e);
            }
        } catch (...) {
            if (errors) {
                InventorySourceError e;
                e.source = InventorySourceId::InstallerFile;
                e.message = QStringLiteral("安装器文件探测出现未知异常");
                errors->push_back(e);
            }
        }
        return out;
    }
};

IInventorySource *createInstallerFileInventorySource()
{
    return new InstallerFileInventorySource;
}

} // namespace un
