// ============================================================================
//  leftovers.cpp —— 残留扫描 + 安全清理
//
//  扫描思路（Geek / IObit 这类工具的基本套路）：
//    1) 从程序名、安装目录名、发布者里提炼"关键词令牌"
//    2) 拿令牌去匹配：注册表 Software 子树、安装目录、AppData 目录、
//       快捷方式、服务、Run 启动项、计划任务
//    3) 命中的条目区分"强匹配 / 弱匹配"，弱匹配默认不勾选，交给用户判断
//
//  安全原则：只列出来给用户看，绝不偷偷删；删除时目录进回收站、
//           注册表先导出 .reg 备份，系统关键路径一律拒绝。
// ============================================================================

#include "core/leftovers.h"
#include "core/winutil.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QStringList>

#include <algorithm>
#include <vector>

namespace un {

namespace {

// ===========================================================================
//  一、关键词与文本工具
// ===========================================================================

// 这些词太常见，参与匹配会造成大面积误伤，直接排除
const QStringList &stopWords()
{
    static const QStringList words = {
        QStringLiteral("setup"), QStringLiteral("update"), QStringLiteral("updater"),
        QStringLiteral("installer"), QStringLiteral("install"), QStringLiteral("uninstall"),
        QStringLiteral("microsoft"), QStringLiteral("windows"), QStringLiteral("program"),
        QStringLiteral("programs"), QStringLiteral("application"), QStringLiteral("apps"),
        QStringLiteral("client"), QStringLiteral("server"), QStringLiteral("service"),
        QStringLiteral("services"), QStringLiteral("driver"), QStringLiteral("drivers"),
        QStringLiteral("tool"), QStringLiteral("tools"), QStringLiteral("manager"),
        QStringLiteral("suite"), QStringLiteral("edition"), QStringLiteral("version"),
        QStringLiteral("beta"), QStringLiteral("alpha"), QStringLiteral("trial"),
        QStringLiteral("free"), QStringLiteral("pro"), QStringLiteral("lite"),
        QStringLiteral("portable"), QStringLiteral("runtime"), QStringLiteral("framework"),
        QStringLiteral("library"), QStringLiteral("libraries"), QStringLiteral("plugin"),
        QStringLiteral("plugins"), QStringLiteral("codec"), QStringLiteral("player"),
        QStringLiteral("viewer"), QStringLiteral("editor"), QStringLiteral("reader"),
        QStringLiteral("studio"), QStringLiteral("code"), QStringLiteral("helper"),
        QStringLiteral("component"), QStringLiteral("components"), QStringLiteral("module"),
        QStringLiteral("x64"), QStringLiteral("x86"), QStringLiteral("win32"), QStringLiteral("win64")
    };
    return words;
}

// 原始候选词 → 归一化令牌（去停用词）
QStringList prepareTokens(const QStringList &raw)
{
    QStringList out;
    for (const QString &r : raw) {
        const QString t = normalizeToken(r);
        if (t.size() < 3) continue;
        if (stopWords().contains(t)) continue;
        if (!out.contains(t)) out << t;
    }
    return out;
}

// 拆词：按空白和常见分隔符切开，只保留长度 >= 4 的词
QStringList splitWords(const QString &text)
{
    QString s = text;
    s.replace(QRegularExpression(QStringLiteral("[\\s\\-_/\\\\|,;:.()\\[\\]{}+&]+")),
              QStringLiteral(" "));

    QStringList out;
    for (const QString &w : s.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        if (w.size() >= 4) out << w;
    }
    return out;
}

// 0 = 不匹配；1 = 名称完全相同；2 = 发布者完全相同。
// 不做子串互含：否则 "Google Chrome" 会命中整棵 HKLM\SOFTWARE\Google。
int matchStrength(const QString &text, const QStringList &strong, const QStringList &soft)
{
    const QString t = normalizeToken(text);
    if (t.size() < 3) return 0;

    for (const QString &c : strong) {
        if (t == c) return 1;
    }
    for (const QString &c : soft) {
        if (t == c) return 2;
    }
    return 0;
}

void markMatch(LeftoverItem *item, bool pathBound, int strength)
{
    if (pathBound) {
        item->match = LeftoverMatch::PathBound;
        item->checked = true;
        return;
    }
    if (strength == 2) {
        item->match = LeftoverMatch::Publisher;
        item->checked = false;
        return;
    }
    item->match = LeftoverMatch::Name;
    item->checked = false;
}

// 从命令行里抠出可执行文件路径
QString extractExePath(const QString &commandLine)
{
    QString s = commandLine.trimmed();
    if (s.isEmpty()) return QString();

    if (s.startsWith(QLatin1Char('"'))) {
        const int end = s.indexOf(QLatin1Char('"'), 1);
        if (end > 1) return s.mid(1, end - 1);
    }

    if (s.startsWith(QLatin1String("\\??\\"))) s = s.mid(4);

    const QString lower = s.toLower();
    const int exePos = lower.indexOf(QStringLiteral(".exe"));
    if (exePos > 0) return s.left(exePos + 4);

    const int space = s.indexOf(QLatin1Char(' '));
    return space > 0 ? s.left(space) : s;
}

// 枚举一个键下的所有子键名
QStringList enumSubKeys(HKEY parent)
{
    QStringList names;
    wchar_t buf[512];
    DWORD index = 0;

    while (true) {
        DWORD nameLen = static_cast<DWORD>(sizeof(buf) / sizeof(buf[0]));
        const LONG ret = RegEnumKeyExW(parent, index, buf, &nameLen,
                                       nullptr, nullptr, nullptr, nullptr);
        if (ret == ERROR_NO_MORE_ITEMS) break;
        if (ret != ERROR_SUCCESS) { ++index; continue; }
        names << QString::fromStdWString(std::wstring(buf, nameLen));
        ++index;
    }
    return names;
}

// 枚举一个键下所有"值"（名字 + 字符串数据）
QVector<QPair<QString, QString>> enumValues(HKEY root, const QString &subPath, bool is64)
{
    QVector<QPair<QString, QString>> result;

    const DWORD view = is64 ? KEY_WOW64_64KEY : KEY_WOW64_32KEY;
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subPath.toStdWString().c_str(), 0, KEY_READ | view, &key)
        != ERROR_SUCCESS)
    {
        return result;
    }

    std::vector<wchar_t> nameBuf(512);
    std::vector<BYTE> dataBuf(4096);
    DWORD index = 0;

    while (true) {
        DWORD nameLen = static_cast<DWORD>(nameBuf.size());
        DWORD dataLen = static_cast<DWORD>(dataBuf.size());
        DWORD type = 0;

        const LONG ret = RegEnumValueW(key, index, nameBuf.data(), &nameLen, nullptr,
                                       &type, dataBuf.data(), &dataLen);
        if (ret == ERROR_NO_MORE_ITEMS) break;
        if (ret == ERROR_MORE_DATA) {
            bool grew = false;
            if (nameBuf.size() < 16384) {
                nameBuf.resize(nameBuf.size() * 2);
                grew = true;
            }
            if (dataLen > dataBuf.size() && dataLen < 1024 * 1024) {
                dataBuf.resize(static_cast<size_t>(dataLen) + 2);
                grew = true;
            } else if (dataBuf.size() < 1024 * 1024) {
                dataBuf.resize(dataBuf.size() * 2);
                grew = true;
            }
            if (grew) continue;
            ++index;
            continue;
        }
        if (ret != ERROR_SUCCESS) { ++index; continue; }

        const QString name = QString::fromStdWString(std::wstring(nameBuf.data(), nameLen));
        QString data;
        if (type == REG_SZ || type == REG_EXPAND_SZ) {
            const auto *chars = reinterpret_cast<const wchar_t *>(dataBuf.data());
            const size_t charsAvail = dataBuf.size() / sizeof(wchar_t);
            size_t n = dataLen / sizeof(wchar_t);
            if (n > charsAvail) n = charsAvail;
            data = QString::fromWCharArray(chars, static_cast<int>(n));
            while (!data.isEmpty() && data.back() == QChar(0)) data.chop(1);
        }
        result.push_back({name, data.trimmed()});
        ++index;
    }

    RegCloseKey(key);
    return result;
}

// CSV 行解析（schtasks 输出带引号，需要正确处理 "" 转义）
QStringList parseCsvRow(const QString &line)
{
    QStringList fields;
    QString cur;
    bool inQuotes = false;

    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (c == QLatin1Char('"')) {
            if (inQuotes && i + 1 < line.size() && line.at(i + 1) == QLatin1Char('"')) {
                cur.append(QLatin1Char('"'));
                ++i;
            } else {
                inQuotes = !inQuotes;
            }
            continue;
        }
        if (c == QLatin1Char(',') && !inQuotes) {
            fields << cur;
            cur.clear();
            continue;
        }
        cur.append(c);
    }
    fields << cur;
    return fields;
}

// 文件名里的非法字符替换掉，用于生成备份文件名
QString sanitizeFileName(const QString &name)
{
    QString s = name;
    s.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
    if (s.size() > 60) s = s.left(60);
    return s.isEmpty() ? QStringLiteral("backup") : s;
}

// 目录所属的类别标签
QString folderCategory(const QString &path)
{
    const QString p = QDir::toNativeSeparators(path).toLower();
    if (p.contains(QStringLiteral("program files"))) return QStringLiteral("安装目录");
    if (p.contains(QStringLiteral("appdata")) || p.contains(QStringLiteral("programdata")))
        return QStringLiteral("用户数据 / 配置");
    return QStringLiteral("其它目录");
}

} // namespace

// ===========================================================================
//  二、扫描
// ===========================================================================

QVector<LeftoverItem> LeftoverScanner::scan(const InstalledApp &app, const ProgressFn &progress)
{
    QVector<LeftoverItem> items;
    QSet<QString> seen;

    auto report = [&progress](const QString &stage, int percent) {
        if (progress) progress(stage, percent);
    };

    auto addItem = [&items, &seen](LeftoverItem item) {
        const QString key = QStringLiteral("%1|%2|%3")
                                .arg(static_cast<int>(item.kind))
                                .arg(item.detail.toLower(), item.extra.toLower());
        if (seen.contains(key)) return;
        seen.insert(key);
        items.push_back(item);
    };

    // ---------- 提炼关键词 ----------
    const QString cleanLocation = QDir::cleanPath(
        QDir::toNativeSeparators(expandEnv(app.installLocation.trimmed())));

    QStringList rawStrong;
    rawStrong << app.name;
    for (const QString &w : splitWords(app.name)) rawStrong << w;
    if (!app.installLocation.isEmpty() && QFileInfo(cleanLocation).isDir())
        rawStrong << QFileInfo(cleanLocation).fileName();

    QStringList rawSoft;
    if (!app.publisher.isEmpty()) {
        rawSoft << app.publisher;
        for (const QString &w : splitWords(app.publisher)) rawSoft << w;
    }

    const QStringList strong = prepareTokens(rawStrong);
    const QStringList soft   = prepareTokens(rawSoft);
    // 安装目录落在 Windows / Program Files 根等危险路径上时，不能拿它做“路径命中”
    const bool installUsable = !cleanLocation.isEmpty()
                               && QFileInfo(cleanLocation).isDir()
                               && !isDangerousPath(cleanLocation);

    // ---------- 1. 卸载登记项本身 ----------
    {
        LeftoverItem it;
        it.kind      = LeftoverKind::RegistryKey;
        it.category  = QStringLiteral("注册表项");
        it.title     = QStringLiteral("卸载登记项");
        it.detail    = app.registryPath;
        it.is64View  = app.is64View;
        it.match     = LeftoverMatch::PathBound;
        it.checked   = !app.registryPath.trimmed().isEmpty();
        it.note      = QStringLiteral("程序在\"应用列表\"里的登记项");
        addItem(it);
    }

    // ---------- 2. 注册表 Software 子树 ----------
    report(QStringLiteral("正在扫描注册表…"), 8);

    struct HiveSpec { HKEY root; bool is64; QString hive; };
    const HiveSpec hives[] = {
        {HKEY_LOCAL_MACHINE, true,  QStringLiteral("HKEY_LOCAL_MACHINE")},
        {HKEY_LOCAL_MACHINE, false, QStringLiteral("HKEY_LOCAL_MACHINE")},
        {HKEY_CURRENT_USER,  true,  QStringLiteral("HKEY_CURRENT_USER")},
        {HKEY_CURRENT_USER,  false, QStringLiteral("HKEY_CURRENT_USER")},
    };

    // 这些顶层键属于系统自身，不参与匹配
    const QStringList skipTopKeys = {
        QStringLiteral("Microsoft"), QStringLiteral("Classes"), QStringLiteral("Policies"),
        QStringLiteral("WOW6432Node"), QStringLiteral("RegisteredApplications"),
        QStringLiteral("Windows"), QStringLiteral("Windows NT"), QStringLiteral("Clients"),
        QStringLiteral("ODBC"), QStringLiteral("Khronos"), QStringLiteral("Vulkan")
    };

    for (const HiveSpec &hive : hives) {
        const DWORD view = hive.is64 ? KEY_WOW64_64KEY : KEY_WOW64_32KEY;

        HKEY softwareKey = nullptr;
        if (RegOpenKeyExW(hive.root, L"SOFTWARE", 0, KEY_READ | view, &softwareKey)
            != ERROR_SUCCESS)
            continue;

        const QStringList subs = enumSubKeys(softwareKey);
        for (const QString &sub : subs) {
            if (skipTopKeys.contains(sub, Qt::CaseInsensitive)) continue;

            const int strength = matchStrength(sub, strong, soft);
            if (strength == 0) continue;

            LeftoverItem it;
            it.kind     = LeftoverKind::RegistryKey;
            it.category = QStringLiteral("注册表项");
            it.title    = sub;
            it.detail   = QStringLiteral("%1\\SOFTWARE\\%2").arg(hive.hive, sub);
            it.is64View = hive.is64;
            markMatch(&it, false, strength);
            if (strength == 2)
                it.note = QStringLiteral("按发布者匹配，删除可能影响同厂商其它软件");
            else
                it.note = QStringLiteral("仅名称相同，请确认后再清理");
            addItem(it);
        }
        RegCloseKey(softwareKey);
    }

    // ---------- 2b. App Paths（程序路径登记）----------
    for (const bool is64 : {true, false}) {
        const DWORD view = is64 ? KEY_WOW64_64KEY : KEY_WOW64_32KEY;
        const QString sub = QStringLiteral(
            "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths");

        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub.toStdWString().c_str(), 0,
                          KEY_READ | view, &key) != ERROR_SUCCESS)
            continue;

        const QStringList subs = enumSubKeys(key);
        for (const QString &exe : subs) {
            HKEY itemKey = nullptr;
            if (RegOpenKeyExW(key, exe.toStdWString().c_str(), 0, KEY_READ | view, &itemKey)
                != ERROR_SUCCESS)
                continue;

            // 默认值就是 exe 的完整路径
            wchar_t pathBuf[1024] = {};
            DWORD len = sizeof(pathBuf);
            DWORD type = 0;
            QString pathValue;
            if (RegQueryValueExW(itemKey, nullptr, nullptr, &type,
                                 reinterpret_cast<LPBYTE>(pathBuf), &len) == ERROR_SUCCESS
                && type == REG_SZ)
            {
                pathValue = QString::fromStdWString(std::wstring(pathBuf));
            }
            RegCloseKey(itemKey);

            const bool underInstall = installUsable
                                      && !pathValue.isEmpty()
                                      && isPathUnder(pathValue, cleanLocation);
            const int strength = matchStrength(exe, strong, soft);
            if (!underInstall && strength == 0) continue;

            LeftoverItem it;
            it.kind     = LeftoverKind::RegistryKey;
            it.category = QStringLiteral("注册表项");
            it.title    = QStringLiteral("App Paths: %1").arg(exe);
            it.detail   = QStringLiteral("HKEY_LOCAL_MACHINE\\%1\\%2").arg(sub, exe);
            it.is64View = is64;
            markMatch(&it, underInstall, strength);
            if (!underInstall)
                it.note = QStringLiteral("仅名称相同，请确认后再清理");
            addItem(it);
        }
        RegCloseKey(key);
    }

    // ---------- 2c. 开机启动项（Run / RunOnce）----------
    report(QStringLiteral("正在扫描开机启动项…"), 22);

    {
        const QStringList runSubPaths = {
            QStringLiteral("Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
            QStringLiteral("Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce"),
            QStringLiteral("Software\\Microsoft\\Windows\\CurrentVersion\\RunServices"),
            QStringLiteral("Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Run")
        };

        struct RunHive { HKEY root; bool is64; QString hive; };
        const RunHive runHives[] = {
            {HKEY_LOCAL_MACHINE, true,  QStringLiteral("HKEY_LOCAL_MACHINE")},
            {HKEY_LOCAL_MACHINE, false, QStringLiteral("HKEY_LOCAL_MACHINE")},
            {HKEY_CURRENT_USER,  true,  QStringLiteral("HKEY_CURRENT_USER")},
            {HKEY_CURRENT_USER,  false, QStringLiteral("HKEY_CURRENT_USER")},
        };

        for (const RunHive &rh : runHives) {
            for (const QString &runSub : runSubPaths) {
                const auto values = enumValues(rh.root, runSub, rh.is64);
                for (const auto &pair : values) {
                    const QString &valueName = pair.first;
                    const QString &valueData = pair.second;
                    if (valueName.isEmpty()) continue;

                    const QString runExe = expandEnv(extractExePath(valueData));
                    const bool underInstall = installUsable && !runExe.isEmpty()
                                              && isPathUnder(runExe, cleanLocation);
                    const int strength = matchStrength(valueName, strong, soft);
                    if (!underInstall && strength == 0) continue;

                    LeftoverItem it;
                    it.kind     = LeftoverKind::RegistryValue;
                    it.category = QStringLiteral("开机启动");
                    it.title    = valueName;
                    it.detail   = QStringLiteral("%1\\%2").arg(rh.hive, runSub);
                    it.extra    = valueName;
                    it.is64View = rh.is64;
                    markMatch(&it, underInstall, strength);
                    it.note     = valueData;
                    addItem(it);
                }
            }
        }
    }

    // ---------- 3. 安装目录 ----------
    report(QStringLiteral("正在统计安装目录…"), 35);

    if (installUsable) {
        LeftoverItem it;
        it.kind      = LeftoverKind::Folder;
        it.category  = QStringLiteral("安装目录");
        it.title     = QFileInfo(cleanLocation).fileName();
        it.detail    = QDir::toNativeSeparators(cleanLocation);
        it.sizeBytes = directorySize(it.detail);
        it.match     = LeftoverMatch::PathBound;
        it.checked   = true;
        addItem(it);
    }

    // ---------- 4. 常见的用户数据 / 公共目录 ----------
    report(QStringLiteral("正在扫描用户数据目录…"), 48);

    {
        // %ProgramFiles% 这类变量在某些环境下取不到，取不到就跳过
        const QStringList roots = {
            expandEnv(QStringLiteral("%LOCALAPPDATA%")),
            expandEnv(QStringLiteral("%LOCALAPPDATA%\\Programs")),
            expandEnv(QStringLiteral("%APPDATA%")),
            expandEnv(QStringLiteral("%PROGRAMDATA%")),
            expandEnv(QStringLiteral("%ProgramFiles%")),
            expandEnv(QStringLiteral("%ProgramFiles(x86)%"))
        };

        for (const QString &root : roots) {
            if (root.isEmpty() || !QDir(root).exists()) continue;

            QDir dir(root);
            const QFileInfoList entries = dir.entryInfoList(
                QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);

            for (const QFileInfo &fi : entries) {
                const QString full = QDir::toNativeSeparators(fi.absoluteFilePath());
                if (isDangerousPath(full)) continue;

                // 已经作为"安装目录"收录过的，跳过
                if (installUsable && isPathUnder(full, cleanLocation)) continue;

                const int strength = matchStrength(fi.fileName(), strong, soft);
                if (strength == 0) continue;

                LeftoverItem it;
                it.kind      = LeftoverKind::Folder;
                it.category  = folderCategory(full);
                it.title     = fi.fileName();
                it.detail    = full;
                it.sizeBytes = directorySize(full);
                markMatch(&it, false, strength);
                it.note = strength == 2
                              ? QStringLiteral("按发布者匹配，请确认是否属于该程序")
                              : QStringLiteral("仅名称相同，请确认后再清理");
                addItem(it);
            }
        }
    }

    // ---------- 5. 快捷方式 ----------
    report(QStringLiteral("正在扫描快捷方式…"), 66);

    {
        const QString startUser = expandEnv(QStringLiteral(
            "%APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs"));
        const QString startAll = expandEnv(QStringLiteral(
            "%PROGRAMDATA%\\Microsoft\\Windows\\Start Menu\\Programs"));
        const QString desktopUser = expandEnv(QStringLiteral("%USERPROFILE%\\Desktop"));
        const QString desktopAll  = expandEnv(QStringLiteral("%PUBLIC%\\Desktop"));

        const QStringList lnkRoots = {startUser, startAll, desktopUser, desktopAll};

        for (const QString &root : lnkRoots) {
            if (root.isEmpty() || !QDir(root).exists()) continue;

            QDirIterator it(root, {QStringLiteral("*.lnk")}, QDir::Files,
                            QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString path = it.next();
                const QFileInfo fi(path);

                const int strength = matchStrength(fi.completeBaseName(), strong, soft);
                if (strength != 1) continue;   // 快捷方式只认强匹配，避免误删

                LeftoverItem item;
                item.kind     = LeftoverKind::File;
                item.category = QStringLiteral("快捷方式");
                item.title    = fi.fileName();
                item.detail   = QDir::toNativeSeparators(path);
                item.sizeBytes = fi.size();
                markMatch(&item, false, 1);
                item.note = QStringLiteral("仅快捷方式名称相同，未核对目标路径");
                addItem(item);
            }
        }
    }

    // ---------- 6. 服务 ----------
    report(QStringLiteral("正在扫描服务…"), 80);

    if (!strong.isEmpty() || installUsable) {
        const QVector<ServiceRef> services = enumerateServices();
        for (const ServiceRef &svc : services) {
            const QString exe = expandEnv(extractExePath(svc.binaryPath));

            const bool underInstall = installUsable && !exe.isEmpty()
                                      && isPathUnder(exe, cleanLocation);
            const int strength = qMax(matchStrength(svc.displayName, strong, soft),
                                      matchStrength(svc.name, strong, soft));
            if (!underInstall && strength == 0) continue;

            LeftoverItem it;
            it.kind          = LeftoverKind::Service;
            it.category      = QStringLiteral("服务");
            it.title         = svc.name;
            it.detail        = svc.displayName.isEmpty() ? svc.binaryPath
                                                         : QStringLiteral("%1 — %2")
                                                               .arg(svc.displayName, svc.binaryPath);
            it.extra         = svc.name;
            it.requiresAdmin = true;
            markMatch(&it, underInstall, strength);
            it.note          = QStringLiteral("删除服务需要管理员权限");
            addItem(it);
        }
    }

    // ---------- 7. 计划任务 ----------
    report(QStringLiteral("正在扫描计划任务…"), 92);

    {
        QProcess proc;
        proc.setProgram(QStringLiteral("schtasks.exe"));
        proc.setArguments({QStringLiteral("/query"), QStringLiteral("/fo"), QStringLiteral("CSV"),
                           QStringLiteral("/v")});
        proc.start();

        if (proc.waitForFinished(20000)) {
            // schtasks 走管道输出的是系统 ANSI 编码（中文系统即 GBK）
            const QString text = QString::fromLocal8Bit(proc.readAllStandardOutput());
            const QStringList lines = text.split(QRegularExpression(QStringLiteral("[\r\n]+")),
                                                 Qt::SkipEmptyParts);

            int nameIdx = -1;
            int runIdx  = -1;

            for (int i = 0; i < lines.size(); ++i) {
                const QStringList fields = parseCsvRow(lines.at(i));

                if (i == 0) {
                    // 表头：定位"任务名"和"要运行的任务"两列的位置
                    for (int c = 0; c < fields.size(); ++c) {
                        const QString h = fields.at(c).trimmed().toLower();
                        // 中文 Windows 的表头是“任务名 / 要运行的任务”，不是英文
                        if (h == QStringLiteral("taskname") || h == QStringLiteral("任务名"))
                            nameIdx = c;
                        if (h == QStringLiteral("task to run") || h == QStringLiteral("要运行的任务"))
                            runIdx = c;
                    }
                    if (nameIdx < 0 && !fields.isEmpty()) nameIdx = 0;
                    continue;
                }

                if (nameIdx < 0 || nameIdx >= fields.size()) continue;

                const QString taskName = fields.at(nameIdx).trimmed();
                const QString taskRun  = (runIdx >= 0 && runIdx < fields.size())
                                             ? fields.at(runIdx).trimmed() : QString();
                if (taskName.isEmpty()) continue;
                // 微软自带任务一律跳过
                if (taskName.startsWith(QLatin1String("\\Microsoft"))) continue;

                const QString runExe = expandEnv(extractExePath(taskRun));
                const bool underInstall = installUsable && !runExe.isEmpty()
                                          && isPathUnder(runExe, cleanLocation);
                const int strength = matchStrength(taskName, strong, soft);
                if (!underInstall && strength == 0) continue;

                LeftoverItem it;
                it.kind          = LeftoverKind::ScheduledTask;
                it.category      = QStringLiteral("计划任务");
                it.title         = taskName;
                it.detail        = taskRun.isEmpty() ? taskName : taskRun;
                it.extra         = taskName;
                it.requiresAdmin = true;
                markMatch(&it, underInstall, strength);
                addItem(it);
            }
        } else {
            proc.kill();
        }
    }

    report(QStringLiteral("扫描完成"), 100);

    // ---------- 按类别排序，界面分组显示更整齐 ----------
    const QStringList order = {
        QStringLiteral("注册表项"), QStringLiteral("安装目录"), QStringLiteral("用户数据 / 配置"),
        QStringLiteral("其它目录"), QStringLiteral("快捷方式"), QStringLiteral("开机启动"),
        QStringLiteral("服务"), QStringLiteral("计划任务")
    };

    std::stable_sort(items.begin(), items.end(),
                     [&order](const LeftoverItem &a, const LeftoverItem &b) {
                         int ia = order.indexOf(a.category);
                         int ib = order.indexOf(b.category);
                         if (ia < 0) ia = 999;
                         if (ib < 0) ib = 999;
                         if (ia != ib) return ia < ib;
                         return a.title.compare(b.title, Qt::CaseInsensitive) < 0;
                     });

    return items;
}

// ===========================================================================
//  三、清理
// ===========================================================================

namespace {

bool writeBackupFile(const QString &path, const QByteArray &data, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("无法写入备份：%1").arg(file.errorString());
        return false;
    }
    if (file.write(data) != data.size()) {
        if (error) *error = QStringLiteral("写入备份不完整：%1").arg(path);
        return false;
    }
    return true;
}

bool backupServiceConfig(const QString &serviceName, const QString &file, QString *error)
{
    QProcess proc;
    proc.setProgram(QStringLiteral("sc.exe"));
    proc.setArguments({QStringLiteral("qc"), serviceName});
    proc.start();
    if (!proc.waitForFinished(15000) || proc.exitCode() != 0) {
        if (error) *error = QStringLiteral("服务配置备份失败，已取消删除");
        return false;
    }
    return writeBackupFile(file, proc.readAllStandardOutput(), error);
}

bool backupScheduledTask(const QString &taskName, const QString &file, QString *error)
{
    QProcess proc;
    proc.setProgram(QStringLiteral("schtasks.exe"));
    proc.setArguments({QStringLiteral("/query"), QStringLiteral("/tn"), taskName,
                       QStringLiteral("/xml")});
    proc.start();
    if (!proc.waitForFinished(15000) || proc.exitCode() != 0) {
        if (error) *error = QStringLiteral("计划任务备份失败，已取消删除");
        return false;
    }
    return writeBackupFile(file, proc.readAllStandardOutput(), error);
}

} // namespace

RemovalResult LeftoverRemover::remove(const QVector<LeftoverItem> &items,
                                       const QString &backupDir)
{
    RemovalResult result;

    if (!backupDir.isEmpty()) QDir().mkpath(backupDir);

    int seq = 0;
    for (const LeftoverItem &item : items) {
        ++seq;
        QString error;
        bool ok = false;

        switch (item.kind) {
        case LeftoverKind::RegistryKey: {
            // 要求备份时，导出失败就保留原键
            if (!backupDir.isEmpty()) {
                const QString file = QDir(backupDir).filePath(
                    QStringLiteral("%1_%2.reg").arg(seq).arg(sanitizeFileName(item.title)));
                QString backupError;
                if (!backupRegistryKey(item.detail, file, &backupError)) {
                    error = backupError.isEmpty()
                                ? QStringLiteral("注册表备份失败，已取消删除")
                                : backupError;
                    break;
                }
                result.backups << file;
            }
            ok = deleteRegistryKey(item.detail, item.is64View, &error);
            break;
        }
        case LeftoverKind::RegistryValue: {
            if (!backupDir.isEmpty()) {
                const QString file = QDir(backupDir).filePath(
                    QStringLiteral("%1_%2.reg").arg(seq).arg(sanitizeFileName(item.title)));
                QString backupError;
                if (!backupRegistryKey(item.detail, file, &backupError)) {
                    error = backupError.isEmpty()
                                ? QStringLiteral("注册表备份失败，已取消删除")
                                : backupError;
                    break;
                }
                result.backups << file;
            }
            ok = deleteRegistryValue(item.detail, item.extra, item.is64View, &error);
            break;
        }

        case LeftoverKind::Folder:
        case LeftoverKind::File:
            if (isDangerousPath(item.detail)) {
                error = QStringLiteral("已阻止删除受保护的系统路径");
                break;
            }
            // 走回收站，误删可恢复
            ok = recyclePaths({item.detail}, &error);
            break;

        case LeftoverKind::Service: {
            if (!backupDir.isEmpty()) {
                const QString file = QDir(backupDir).filePath(
                    QStringLiteral("%1_%2.service.txt").arg(seq).arg(sanitizeFileName(item.extra)));
                if (!backupServiceConfig(item.extra, file, &error)) break;
                result.backups << file;
            }
            ok = deleteService(item.extra, &error);
            break;
        }

        case LeftoverKind::ScheduledTask: {
            if (!backupDir.isEmpty()) {
                const QString file = QDir(backupDir).filePath(
                    QStringLiteral("%1_%2.task.xml").arg(seq).arg(sanitizeFileName(item.title)));
                if (!backupScheduledTask(item.extra, file, &error)) break;
                result.backups << file;
            }
            QProcess proc;
            proc.setProgram(QStringLiteral("schtasks.exe"));
            proc.setArguments({QStringLiteral("/delete"), QStringLiteral("/tn"),
                               item.extra, QStringLiteral("/f")});
            proc.start();
            if (proc.waitForFinished(15000) && proc.exitCode() == 0) {
                ok = true;
            } else {
                error = QStringLiteral("删除计划任务失败（需要管理员权限）");
            }
            break;
        }
        }

        if (ok) {
            ++result.successCount;
            if (item.sizeBytes > 0) result.reclaimedBytes += item.sizeBytes;
        } else {
            ++result.failedCount;
            result.errors << QStringLiteral("%1：%2")
                                 .arg(item.detail, error.isEmpty() ? QStringLiteral("失败") : error);
        }
    }

    return result;
}

int LeftoverRemover::killRelatedProcesses(const InstalledApp &app)
{
    if (app.installLocation.isEmpty()) return 0;

    const QString dir = expandEnv(app.installLocation.trimmed());
    if (dir.isEmpty() || isDangerousPath(dir)) return 0;

    const QVector<ProcessRef> procs = processesUnder(dir);
    const quint32 selfPid = static_cast<quint32>(QCoreApplication::applicationPid());

    int killed = 0;
    for (const ProcessRef &p : procs) {
        if (p.pid == selfPid || p.pid <= 4) continue;
        if (isProtectedProcessName(p.name)) continue;
        if (killProcessIfUnder(p.pid, dir, nullptr)) ++killed;
    }
    return killed;
}

QStringList LeftoverRemover::relatedProcessNames(const InstalledApp &app)
{
    QStringList names;
    if (app.installLocation.isEmpty()) return names;

    const QString dir = expandEnv(app.installLocation.trimmed());
    if (dir.isEmpty() || isDangerousPath(dir)) return names;

    const quint32 selfPid = static_cast<quint32>(QCoreApplication::applicationPid());
    for (const ProcessRef &p : processesUnder(dir)) {
        if (p.pid == selfPid || p.pid <= 4) continue;
        if (isProtectedProcessName(p.name)) continue;
        names << QStringLiteral("%1 (PID %2)").arg(p.name).arg(p.pid);
    }
    return names;
}

} // namespace un
