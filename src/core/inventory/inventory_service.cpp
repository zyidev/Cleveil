// ============================================================================
//  inventory_service.cpp —— 多源调度、合并去重、快照差分
// ============================================================================

#include "core/inventory/inventory_service.h"
#include "core/inventory/factories.h"
#include "core/inventory/whitelist.h"
#include "core/winutil.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>
#include <memory>

namespace un {

namespace {

QString snapshotFilePath()
{
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(root);
    return QDir(root).filePath(QStringLiteral("inventory-snapshot.json"));
}

QStringList collectTopLevelDirs()
{
    QStringList dirs;
    const QStringList roots = {
        expandEnv(QStringLiteral("%ProgramFiles%")),
        expandEnv(QStringLiteral("%ProgramFiles(x86)%")),
        expandEnv(QStringLiteral("%LOCALAPPDATA%\\Programs")),
    };
    for (const QString &root : roots) {
        if (root.isEmpty() || !QDir(root).exists()) continue;
        QDir base(root);
        for (const QFileInfo &fi : base.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString full = QDir::toNativeSeparators(fi.absoluteFilePath());
            if (!isDangerousPath(full))
                dirs << full;
        }
    }
    dirs.sort(Qt::CaseInsensitive);
    return dirs;
}

int sourceRank(InventorySourceId id)
{
    switch (id) {
    case InventorySourceId::Arp:           return 100;
    case InventorySourceId::Msi:           return 90;
    case InventorySourceId::Appx:          return 85;
    case InventorySourceId::InstallerFile: return 70;
    case InventorySourceId::Service:       return 40;
    case InventorySourceId::DriverPackage: return 30;
    case InventorySourceId::Shortcut:      return 20;
    case InventorySourceId::ExeVersion:    return 15;
    case InventorySourceId::SnapshotDiff:  return 10;
    }
    return 0;
}

void mergeInto(InventoryRecord *dst, const InventoryRecord &src)
{
    if (dst->name.isEmpty()) dst->name = src.name;
    if (dst->version.isEmpty()) dst->version = src.version;
    if (dst->publisher.isEmpty()) dst->publisher = src.publisher;
    if (dst->installLocation.isEmpty()) dst->installLocation = src.installLocation;
    if (dst->iconSource.isEmpty()) dst->iconSource = src.iconSource;
    if (dst->uninstallString.isEmpty()) dst->uninstallString = src.uninstallString;
    if (dst->quietUninstallString.isEmpty()) dst->quietUninstallString = src.quietUninstallString;
    if (dst->productCode.isEmpty()) dst->productCode = src.productCode;
    if (dst->registryPath.isEmpty()) dst->registryPath = src.registryPath;
    if (dst->sizeBytes < 0 && src.sizeBytes >= 0) dst->sizeBytes = src.sizeBytes;
    if (dst->packageType == PackageType::Unknown) dst->packageType = src.packageType;
    dst->systemComponent = dst->systemComponent || src.systemComponent;
    dst->isUpdate = dst->isUpdate || src.isUpdate;
    dst->whitelistBlocked = dst->whitelistBlocked || src.whitelistBlocked;
    if (sourceRank(src.source) > sourceRank(dst->source)) {
        // 更高权威来源覆盖类型与卸载命令（若对方有）
        if (!src.uninstallString.isEmpty()) dst->uninstallString = src.uninstallString;
        if (!src.quietUninstallString.isEmpty())
            dst->quietUninstallString = src.quietUninstallString;
        if (src.packageType != PackageType::Unknown) dst->packageType = src.packageType;
        dst->source = src.source;
        dst->trust = src.trust;
    }
}

InstalledApp toInstalledApp(const InventoryRecord &r, const QStringList &sourceTags)
{
    InstalledApp app;
    app.id = r.localId.isEmpty() ? r.identityKey : r.localId;
    app.name = r.name;
    app.version = r.version;
    app.publisher = r.publisher;
    app.installLocation = r.installLocation;
    app.iconSource = r.iconSource;
    app.uninstallString = r.uninstallString;
    app.quietUninstallString = r.quietUninstallString;
    app.productCode = r.productCode;
    app.registryPath = r.registryPath;
    app.sizeBytes = r.sizeBytes;
    app.is64View = r.is64View;
    app.systemComponent = r.systemComponent;
    app.isUpdate = r.isUpdate;
    app.type = r.packageType;
    app.sources = sourceTags;
    app.heuristic = (r.trust == InventoryTrust::Heuristic);
    app.whitelistBlocked = r.whitelistBlocked
                           || isInventoryWhitelisted(r.name, r.publisher, r.installLocation,
                                                     r.productCode);
    app.inventoryNote = r.note;
    if (app.whitelistBlocked && app.inventoryNote.isEmpty())
        app.inventoryNote = QStringLiteral("命中系统共享组件白名单");
    if (!app.sources.isEmpty()) {
        const QString tag = QStringLiteral("来源：%1").arg(app.sources.join(QLatin1Char('+')));
        app.inventoryNote = app.inventoryNote.isEmpty() ? tag
                                                        : app.inventoryNote + QStringLiteral(" · ") + tag;
    }
    if (!app.name.isEmpty())
        app.keywords << app.name;
    if (!app.publisher.isEmpty())
        app.keywords << app.publisher;
    return app;
}

struct MergeBucket {
    InventoryRecord primary;
    QSet<QString> sourceTags;
};

InventoryScanResult mergeAll(const QVector<InventoryRecord> &raw,
                             const InventoryScanOptions &options)
{
    InventoryScanResult result;
    QHash<QString, MergeBucket> softwareBuckets;
    QSet<QString> knownPaths;

    auto rememberPath = [&](const QString &loc) {
        if (loc.trimmed().isEmpty()) return;
        knownPaths.insert(QDir::fromNativeSeparators(loc).toLower());
    };

    // 第一遍：权威软件（ARP / MSI / AppX）
    for (const InventoryRecord &r : raw) {
        if (r.category == InventoryCategory::DriverPackage) {
            result.drivers.push_back(r);
            continue;
        }
        if (r.trust == InventoryTrust::Heuristic) {
            result.heuristic.push_back(r);
            continue;
        }
        if (r.source == InventorySourceId::Service) {
            // 稍后处理
            continue;
        }
        if (r.source == InventorySourceId::InstallerFile) {
            // 稍后：能挂靠则挂靠，否则升格为软件（仍非启发式）
            continue;
        }

        const QString key = r.identityKey.isEmpty() ? r.localId : r.identityKey;
        auto it = softwareBuckets.find(key);
        if (it == softwareBuckets.end()) {
            MergeBucket b;
            b.primary = r;
            b.sourceTags.insert(inventorySourceText(r.source));
            softwareBuckets.insert(key, b);
        } else {
            mergeInto(&it->primary, r);
            it->sourceTags.insert(inventorySourceText(r.source));
            // ARP 与 MSI 同时命中时，保留 ARP 的 registryPath / id 观感
            if (r.source == InventorySourceId::Arp) {
                if (!r.registryPath.isEmpty()) it->primary.registryPath = r.registryPath;
                if (!r.localId.isEmpty()) it->primary.localId = r.localId;
            }
        }
        rememberPath(r.installLocation);
    }

    // 安装器文件：目录已存在则只印证，否则独立成条
    for (const InventoryRecord &r : raw) {
        if (r.source != InventorySourceId::InstallerFile) continue;
        const QString pathKey = r.identityKey;
        bool attached = false;
        for (auto it = softwareBuckets.begin(); it != softwareBuckets.end(); ++it) {
            if (it->primary.installLocation.isEmpty()) continue;
            if (isPathUnder(r.installLocation, it->primary.installLocation)
                || isPathUnder(it->primary.installLocation, r.installLocation)
                || pathKey == QStringLiteral("path:%1").arg(
                       QDir::fromNativeSeparators(it->primary.installLocation).toLower()))
            {
                mergeInto(&it->primary, r);
                it->sourceTags.insert(inventorySourceText(r.source));
                attached = true;
                break;
            }
        }
        if (!attached) {
            MergeBucket b;
            b.primary = r;
            b.primary.trust = InventoryTrust::Authoritative; // 有真实卸载器，可进入主列表
            b.sourceTags.insert(inventorySourceText(r.source));
            softwareBuckets.insert(pathKey, b);
            rememberPath(r.installLocation);
        }
    }

    // 服务：能挂到已知安装目录则只记来源；否则进孤儿服务
    for (const InventoryRecord &r : raw) {
        if (r.source != InventorySourceId::Service) continue;
        bool attached = false;
        if (!r.installLocation.isEmpty()) {
            const QString norm = QDir::fromNativeSeparators(r.installLocation).toLower();
            for (auto it = softwareBuckets.begin(); it != softwareBuckets.end(); ++it) {
                if (it->primary.installLocation.isEmpty()) continue;
                if (isPathUnder(r.installLocation, it->primary.installLocation)
                    || knownPaths.contains(norm))
                {
                    it->sourceTags.insert(QStringLiteral("Service"));
                    attached = true;
                    break;
                }
            }
        }
        if (!attached)
            result.orphanServices.push_back(r);
    }

    // 启发式：若目录已被权威覆盖则丢弃；否则留在 heuristic 分区，可选并入主列表
    QVector<InventoryRecord> filteredHeuristic;
    for (const InventoryRecord &r : result.heuristic) {
        bool covered = false;
        for (auto it = softwareBuckets.constBegin(); it != softwareBuckets.constEnd(); ++it) {
            if (it->primary.installLocation.isEmpty() || r.installLocation.isEmpty()) continue;
            if (isPathUnder(r.installLocation, it->primary.installLocation)
                || isPathUnder(it->primary.installLocation, r.installLocation))
            {
                covered = true;
                break;
            }
        }
        if (covered) continue;
        if (isInventoryWhitelisted(r.name, r.publisher, r.installLocation, r.productCode)) {
            InventoryRecord blocked = r;
            blocked.whitelistBlocked = true;
            blocked.note = QStringLiteral("命中系统共享组件白名单");
            filteredHeuristic.push_back(blocked);
            continue;
        }
        filteredHeuristic.push_back(r);
        if (options.includeHeuristicInSoftware) {
            MergeBucket b;
            b.primary = r;
            b.sourceTags.insert(inventorySourceText(r.source));
            softwareBuckets.insert(r.identityKey.isEmpty() ? r.localId : r.identityKey, b);
        }
    }
    result.heuristic = filteredHeuristic;

    // 投影软件列表
    for (auto it = softwareBuckets.constBegin(); it != softwareBuckets.constEnd(); ++it) {
        QStringList tags = QStringList(it->sourceTags.values());
        tags.sort(Qt::CaseInsensitive);
        result.software.push_back(toInstalledApp(it->primary, tags));
    }

    std::sort(result.software.begin(), result.software.end(),
              [](const InstalledApp &a, const InstalledApp &b) {
                  return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
              });
    return result;
}

} // namespace

bool InventoryService::captureDirectorySnapshot(QString *error)
{
    try {
        const QStringList dirs = collectTopLevelDirs();
        QJsonObject root;
        root.insert(QStringLiteral("capturedAt"),
                    QDateTime::currentDateTime().toString(Qt::ISODate));
        QJsonArray arr;
        for (const QString &d : dirs) arr.append(d);
        root.insert(QStringLiteral("directories"), arr);

        QSaveFile file(snapshotFilePath());
        if (!file.open(QIODevice::WriteOnly)) {
            if (error) *error = file.errorString();
            return false;
        }
        file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        if (!file.commit()) {
            if (error) *error = file.errorString();
            return false;
        }
        return true;
    } catch (const std::exception &ex) {
        if (error) *error = QString::fromUtf8(ex.what());
        return false;
    } catch (...) {
        if (error) *error = QStringLiteral("写入快照失败");
        return false;
    }
}

QStringList InventoryService::snapshotNewDirectories(QString *error)
{
    QStringList added;
    try {
        QFile file(snapshotFilePath());
        if (!file.exists()) {
            if (error) *error = QStringLiteral("尚无快照，请先在监控或选项中捕获基线");
            return added;
        }
        if (!file.open(QIODevice::ReadOnly)) {
            if (error) *error = file.errorString();
            return added;
        }
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        QSet<QString> before;
        for (const QJsonValue &v : root.value(QStringLiteral("directories")).toArray())
            before.insert(v.toString().toLower());

        for (const QString &d : collectTopLevelDirs()) {
            if (!before.contains(d.toLower()))
                added << d;
        }
    } catch (const std::exception &ex) {
        if (error) *error = QString::fromUtf8(ex.what());
    } catch (...) {
        if (error) *error = QStringLiteral("读取快照失败");
    }
    return added;
}

InventoryScanResult InventoryService::scan(const InventoryScanOptions &options)
{
    InventoryScanResult result;
    QVector<InventoryRecord> raw;

    struct Entry {
        bool enabled;
        IInventorySource *(*factory)();
    };

    const Entry entries[] = {
        {options.arp, createArpInventorySource},
        {options.msi, createMsiInventorySource},
        {options.appx, createAppxInventorySource},
        {options.driverPackages, createDriverInventorySource},
        {options.services, createServiceInventorySource},
        {options.installerFiles, createInstallerFileInventorySource},
        {options.shortcuts, createShortcutInventorySource},
        {options.exeVersion, createExeVersionInventorySource},
        {options.snapshotDiff, createSnapshotDiffInventorySource},
    };

    for (const Entry &e : entries) {
        if (!e.enabled) continue;
        std::unique_ptr<IInventorySource> src(e.factory());
        try {
            QVector<InventorySourceError> localErrors;
            const QVector<InventoryRecord> items = src->scan(&localErrors);
            raw += items;
            result.errors += localErrors;
        } catch (const std::exception &ex) {
            InventorySourceError err;
            err.source = src->id();
            err.message = QString::fromUtf8(ex.what());
            result.errors.push_back(err);
        } catch (...) {
            InventorySourceError err;
            err.source = src->id();
            err.message = QStringLiteral("数据源扫描失败");
            result.errors.push_back(err);
        }
    }

    InventoryScanResult merged = mergeAll(raw, options);
    merged.errors = result.errors;
    return merged;
}

QVector<InstalledApp> InventoryService::scanSoftware(const InventoryScanOptions &options)
{
    return scan(options).software;
}

QVector<InstalledApp> InventoryService::projectApps(const InventoryScanResult &bundle,
                                                    bool includeOrphanServices)
{
    QVector<InstalledApp> apps = bundle.software;
    for (const InventoryRecord &d : bundle.drivers) {
        InstalledApp a = toInstalledApp(d, {inventorySourceText(d.source)});
        a.type = PackageType::Driver;
        a.uninstallString.clear();
        a.quietUninstallString.clear();
        apps.push_back(a);
    }
    if (includeOrphanServices) {
        for (const InventoryRecord &s : bundle.orphanServices) {
            if (s.whitelistBlocked) continue;
            InstalledApp a = toInstalledApp(s, {inventorySourceText(s.source)});
            a.type = PackageType::Service;
            apps.push_back(a);
        }
    }
    return apps;
}

} // namespace un
