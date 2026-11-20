// ============================================================================
//  sources_heuristic.cpp —— 辅助识别绿色软件
//  LNK 快捷方式、EXE 版本信息、安装前后目录快照差分。全部标 Heuristic。
// ============================================================================

#include "core/inventory/source.h"
#include "core/inventory/whitelist.h"
#include "core/inventory/inventory_service.h"
#include "core/winutil.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QSet>

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <objbase.h>

#include <vector>

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

    struct LANGANDCODEPAGE { WORD language; WORD codePage; };
    LANGANDCODEPAGE *translate = nullptr;
    UINT translateLen = 0;
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<LPVOID *>(&translate), &translateLen)
        || !translate)
        return {};

    const QString path = QStringLiteral("\\StringFileInfo\\%1%2\\%3")
                             .arg(translate[0].language, 4, 16, QLatin1Char('0'))
                             .arg(translate[0].codePage, 4, 16, QLatin1Char('0'))
                             .arg(QString::fromWCharArray(key));
    wchar_t *value = nullptr;
    UINT valueLen = 0;
    if (!VerQueryValueW(buf.data(), path.toStdWString().c_str(),
                        reinterpret_cast<LPVOID *>(&value), &valueLen)
        || !value)
        return {};
    return QString::fromWCharArray(value);
}

QString resolveShortcut(const QString &lnkPath)
{
    IShellLinkW *link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, reinterpret_cast<void **>(&link))))
        return {};

    IPersistFile *file = nullptr;
    QString target;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&file)))) {
        if (SUCCEEDED(file->Load(lnkPath.toStdWString().c_str(), STGM_READ))) {
            wchar_t path[MAX_PATH] = {};
            WIN32_FIND_DATAW wfd{};
            if (SUCCEEDED(link->GetPath(path, MAX_PATH, &wfd, SLGP_UNCPRIORITY)))
                target = QString::fromWCharArray(path);
        }
        file->Release();
    }
    link->Release();
    return QDir::toNativeSeparators(target);
}

void addHeuristicExe(const QString &exe,
                     InventorySourceId sourceId,
                     const QString &note,
                     QVector<InventoryRecord> *out,
                     QSet<QString> *seen)
{
    if (exe.isEmpty() || !QFileInfo::exists(exe)) return;
    const QString native = QDir::toNativeSeparators(exe);
    const QString key = native.toLower();
    if (seen->contains(key)) return;

    const QString dir = QDir::toNativeSeparators(QFileInfo(native).absolutePath());
    if (isDangerousPath(dir) || isDangerousPath(native)) return;

    const QString product = readVersionString(native, L"ProductName");
    const QString company = readVersionString(native, L"CompanyName");
    const QString version = readVersionString(native, L"ProductVersion");
    const QString name = product.isEmpty() ? QFileInfo(native).completeBaseName() : product;

    if (isInventoryWhitelisted(name, company, dir)) return;

    InventoryRecord r;
    r.source = sourceId;
    r.trust = InventoryTrust::Heuristic;
    r.category = InventoryCategory::HeuristicFind;
    r.packageType = PackageType::Portable;
    r.localId = QStringLiteral("heur:%1").arg(key);
    r.identityKey = QStringLiteral("path:%1").arg(
        QDir::fromNativeSeparators(dir).toLower());
    r.name = name;
    r.version = version;
    r.publisher = company;
    r.installLocation = dir;
    r.iconSource = native;
    r.note = note;
    r.whitelistBlocked = false;
    out->push_back(r);
    seen->insert(key);
}

QVector<InventoryRecord> scanShortcuts(QVector<InventorySourceError> *errors)
{
    QVector<InventoryRecord> out;
    QSet<QString> seen;
    try {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool balance = (hr == S_OK || hr == S_FALSE);

        const QStringList roots = {
            expandEnv(QStringLiteral("%APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs")),
            expandEnv(QStringLiteral("%PROGRAMDATA%\\Microsoft\\Windows\\Start Menu\\Programs")),
            expandEnv(QStringLiteral("%USERPROFILE%\\Desktop")),
            expandEnv(QStringLiteral("%PUBLIC%\\Desktop")),
        };

        for (const QString &root : roots) {
            if (root.isEmpty() || !QDir(root).exists()) continue;
            QDirIterator it(root, {QStringLiteral("*.lnk")}, QDir::Files,
                            QDirIterator::Subdirectories);
            // 限制数量，避免开始菜单极大时拖垮刷新
            int count = 0;
            while (it.hasNext() && count < 400) {
                const QString lnk = it.next();
                ++count;
                const QString target = resolveShortcut(lnk);
                if (!target.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) continue;
                if (isPathUnder(target, expandEnv(QStringLiteral("%SystemRoot%")))) continue;
                addHeuristicExe(target, InventorySourceId::Shortcut,
                                QStringLiteral("来自快捷方式：%1").arg(QFileInfo(lnk).fileName()),
                                &out, &seen);
            }
        }

        if (balance) CoUninitialize();
    } catch (const std::exception &ex) {
        if (errors) {
            InventorySourceError e;
            e.source = InventorySourceId::Shortcut;
            e.message = QString::fromUtf8(ex.what());
            errors->push_back(e);
        }
    } catch (...) {
        if (errors) {
            InventorySourceError e;
            e.source = InventorySourceId::Shortcut;
            e.message = QStringLiteral("快捷方式解析失败");
            errors->push_back(e);
        }
    }
    return out;
}

QVector<InventoryRecord> scanExeRoots(QVector<InventorySourceError> *errors)
{
    QVector<InventoryRecord> out;
    QSet<QString> seen;
    try {
        const QStringList roots = {
            expandEnv(QStringLiteral("%LOCALAPPDATA%\\Programs")),
            expandEnv(QStringLiteral("%USERPROFILE%\\PortableApps")),
        };
        for (const QString &root : roots) {
            if (root.isEmpty() || !QDir(root).exists()) continue;
            QDir base(root);
            for (const QFileInfo &di : base.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                const QString dir = di.absoluteFilePath();
                if (isDangerousPath(dir)) continue;
                // 每个目录取第一个 exe 读版本信息
                QDir child(dir);
                const QFileInfoList exes = child.entryInfoList({QStringLiteral("*.exe")}, QDir::Files);
                if (exes.isEmpty()) continue;
                addHeuristicExe(exes.first().absoluteFilePath(), InventorySourceId::ExeVersion,
                                QStringLiteral("EXE 版本信息启发式"), &out, &seen);
            }
        }
    } catch (const std::exception &ex) {
        if (errors) {
            InventorySourceError e;
            e.source = InventorySourceId::ExeVersion;
            e.message = QString::fromUtf8(ex.what());
            errors->push_back(e);
        }
    } catch (...) {
        if (errors) {
            InventorySourceError e;
            e.source = InventorySourceId::ExeVersion;
            e.message = QStringLiteral("EXE 版本信息扫描失败");
            errors->push_back(e);
        }
    }
    return out;
}

QVector<InventoryRecord> scanSnapshotDiff(QVector<InventorySourceError> *errors)
{
    QVector<InventoryRecord> out;
    QSet<QString> seen;
    QString err;
    const QStringList added = InventoryService::snapshotNewDirectories(&err);
    if (!err.isEmpty() && errors) {
        InventorySourceError e;
        e.source = InventorySourceId::SnapshotDiff;
        e.message = err;
        errors->push_back(e);
    }
    for (const QString &dir : added) {
        if (isDangerousPath(dir) || isInventoryWhitelisted(QFileInfo(dir).fileName(), {}, dir))
            continue;
        QDir child(dir);
        const QFileInfoList exes = child.entryInfoList({QStringLiteral("*.exe")}, QDir::Files);
        if (exes.isEmpty()) {
            InventoryRecord r;
            r.source = InventorySourceId::SnapshotDiff;
            r.trust = InventoryTrust::Heuristic;
            r.category = InventoryCategory::HeuristicFind;
            r.packageType = PackageType::Portable;
            r.localId = QStringLiteral("snap:%1").arg(dir.toLower());
            r.identityKey = QStringLiteral("path:%1").arg(
                QDir::fromNativeSeparators(dir).toLower());
            r.name = QFileInfo(dir).fileName();
            r.installLocation = QDir::toNativeSeparators(dir);
            r.note = QStringLiteral("快照差分：安装后新出现的目录");
            out.push_back(r);
            continue;
        }
        addHeuristicExe(exes.first().absoluteFilePath(), InventorySourceId::SnapshotDiff,
                        QStringLiteral("快照差分：安装后新出现的目录"), &out, &seen);
    }
    return out;
}

} // namespace

class ShortcutInventorySource final : public IInventorySource {
public:
    InventorySourceId id() const override { return InventorySourceId::Shortcut; }
    QVector<InventoryRecord> scan(QVector<InventorySourceError> *errors) override
    {
        return scanShortcuts(errors);
    }
};

class ExeVersionInventorySource final : public IInventorySource {
public:
    InventorySourceId id() const override { return InventorySourceId::ExeVersion; }
    QVector<InventoryRecord> scan(QVector<InventorySourceError> *errors) override
    {
        return scanExeRoots(errors);
    }
};

class SnapshotDiffInventorySource final : public IInventorySource {
public:
    InventorySourceId id() const override { return InventorySourceId::SnapshotDiff; }
    QVector<InventoryRecord> scan(QVector<InventorySourceError> *errors) override
    {
        return scanSnapshotDiff(errors);
    }
};

IInventorySource *createShortcutInventorySource() { return new ShortcutInventorySource; }
IInventorySource *createExeVersionInventorySource() { return new ExeVersionInventorySource; }
IInventorySource *createSnapshotDiffInventorySource() { return new SnapshotDiffInventorySource; }

} // namespace un
