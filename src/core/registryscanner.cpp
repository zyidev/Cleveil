// ============================================================================
//  registryscanner.cpp —— Uninstall 键遍历与字段解析
//
//  注册表结构示意：
//    HKEY_LOCAL_MACHINE
//      SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall
//        {GUID}                        ← MSI 安装的程序（项名就是产品码）
//        Google Chrome                 ← 自带安装器的程序（项名是程序名）
//          DisplayName        REG_SZ   "Google Chrome"
//          DisplayVersion     REG_SZ   "131.0.6778.86"
//          Publisher          REG_SZ   "Google LLC"
//          InstallDate        REG_SZ   "20241120"
//          EstimatedSize      REG_DWORD  单位 KB
//          InstallLocation    REG_SZ
//          DisplayIcon        REG_SZ   "C:\…\chrome.exe,0"
//          UninstallString    REG_SZ   "C:\…\setup.exe --uninstall"
//          QuietUninstallString REG_SZ 静默卸载命令
//          SystemComponent    REG_DWORD 1 = 系统组件，默认隐藏
// ============================================================================

#include "core/registryscanner.h"
#include "core/uninstaller.h"      // msiProductInstalled：判断僵尸登记项用
#include "core/winutil.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QStringList>

#include <vector>

namespace un {

namespace {

// 相对 HKEY 的两级路径：Uninstall 键在三个位置
const wchar_t *kUninstallPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

// 读取一个值并统一成 QVariant：字符串类返回 QString，DWORD 返回 quint32
QVariant readValue(HKEY key, const wchar_t *valueName)
{
    DWORD type = 0;
    DWORD size = 0;

    // 先探大小
    if (RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &size) != ERROR_SUCCESS)
        return QVariant();

    if (type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ) {
        std::vector<wchar_t> buf(size / sizeof(wchar_t) + 2, L'\0');
        if (RegQueryValueExW(key, valueName, nullptr, &type,
                             reinterpret_cast<LPBYTE>(buf.data()), &size) != ERROR_SUCCESS)
            return QVariant();

        std::wstring text(buf.data());
        return QVariant(QString::fromStdWString(text));
    }

    if (type == REG_DWORD || type == REG_QWORD) {
        DWORD dw = 0;
        size = sizeof(dw);
        if (RegQueryValueExW(key, valueName, nullptr, &type,
                             reinterpret_cast<LPBYTE>(&dw), &size) != ERROR_SUCCESS)
            return QVariant();
        return QVariant(static_cast<quint32>(dw));
    }

    return QVariant();
}

QString readString(HKEY key, const wchar_t *valueName)
{
    return readValue(key, valueName).toString().trimmed();
}

quint32 readDword(HKEY key, const wchar_t *valueName, quint32 fallback = 0)
{
    const QVariant v = readValue(key, valueName);
    return v.isValid() ? v.toUInt() : fallback;
}

// 枚举一个键下所有子键名
QStringList enumSubKeys(HKEY parent)
{
    QStringList names;

    DWORD index = 0;
    wchar_t buf[512];

    while (true) {
        DWORD nameLen = static_cast<DWORD>(std::size(buf));
        const LONG ret = RegEnumKeyExW(parent, index, buf, &nameLen,
                                       nullptr, nullptr, nullptr, nullptr);
        if (ret == ERROR_NO_MORE_ITEMS) break;
        if (ret != ERROR_SUCCESS) {
            ++index;    // 单个子键读失败不影响整体，跳过继续
            continue;
        }
        names << QString::fromStdWString(std::wstring(buf, nameLen));
        ++index;
    }
    return names;
}

// 从字符串里抠出 MSI 产品码 {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}
QString extractGuid(const QString &text)
{
    static const QRegularExpression re(
        QStringLiteral("\\{[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-"
                       "[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\\}"));
    const QRegularExpressionMatch m = re.match(text);
    return m.hasMatch() ? m.captured(0).toUpper() : QString();
}

// 判断注册表项名本身就是 GUID（MSI 安装的标志之一）
bool looksLikeGuid(const QString &text)
{
    const QString g = extractGuid(text);
    return !g.isEmpty() && text.trimmed().size() == g.size();
}

// "20241120" → QDate(2024,11,20)；解析失败返回无效日期
QDate parseInstallDate(const QString &raw)
{
    const QString s = raw.trimmed();
    if (s.size() != 8) return QDate();
    const QDate d = QDate::fromString(s, QStringLiteral("yyyyMMdd"));
    return d.isValid() ? d : QDate();
}

// 生成用于"残留匹配"的候选关键词
QStringList buildKeywords(const QString &name, const QString &publisher,
                          const QString &installLocation, const QString &uninstallCmd)
{
    QStringList keys;

    auto add = [&keys](const QString &s) {
        const QString t = s.trimmed();
        if (t.size() >= 3 && !keys.contains(t)) keys << t;
    };

    add(name);
    add(publisher);

    // 名称去掉版本号和括号后剩下的主体，例如 "Adobe Acrobat DC (64-bit)" → "Adobe Acrobat DC"
    QString base = name;
    base.remove(QRegularExpression(QStringLiteral("\\s*\\(\\s*(?:x?64|x86|32)[- ]?bit\\s*\\)"),
                                  QRegularExpression::CaseInsensitiveOption));
    base.remove(QRegularExpression(QStringLiteral("\\s*v?\\d+\\.\\d+.*$")));
    add(base);

    // 安装目录末级目录名：很多程序的 AppData 残留就是按这个名字建目录的
    if (!installLocation.isEmpty()) {
        const QString dirName = QFileInfo(QDir::cleanPath(
            QDir::toNativeSeparators(installLocation))).fileName();
        add(dirName);
    }

    // 卸载程序文件名（去掉扩展名），例如 "unins000"
    if (!uninstallCmd.isEmpty()) {
        QString cmd = unquote(uninstallCmd);
        if (cmd.startsWith(QLatin1Char('"'))) {
            const int end = cmd.indexOf(QLatin1Char('"'), 1);
            if (end > 1) cmd = cmd.mid(1, end - 1);
        } else {
            const int sp = cmd.indexOf(QLatin1Char(' '));
            if (sp > 0) cmd = cmd.left(sp);
        }
        const QFileInfo fi(cmd);
        if (!fi.completeBaseName().isEmpty() && fi.completeBaseName().size() >= 3)
            add(fi.completeBaseName());
    }

    return keys;
}

// 单个注册表项的解析结果
bool parseEntry(HKEY key, const QString &subKeyName, const QString &fullPath,
                bool is64View, InstalledApp *out)
{
    const QString name = readString(key, L"DisplayName");
    if (name.isEmpty()) return false;          // 没有显示名的项一律不列（多半是残留垃圾）

    const QString version   = readString(key, L"DisplayVersion");
    const QString publisher = readString(key, L"Publisher");
    // InstallLocation 有些安装器会写成带引号的形式（"C:\Program Files\xxx"），
    // 直接拿来当路径用会出现引号，这里统一去引号并转成 Windows 分隔符。
    const QString loc       = QDir::toNativeSeparators(unquote(expandEnv(readString(key, L"InstallLocation"))));
    const QString iconSrc   = readString(key, L"DisplayIcon");
    const QString uninstall = readString(key, L"UninstallString");
    const QString quiet     = readString(key, L"QuietUninstallString");

    out->id              = QStringLiteral("%1|%2").arg(fullPath, is64View ? QStringLiteral("64")
                                                                          : QStringLiteral("32"));
    out->name            = name;
    out->version         = version;
    out->publisher       = publisher;
    out->installDate     = parseInstallDate(readString(key, L"InstallDate"));
    out->installLocation = loc;
    out->iconSource      = iconSrc;
    out->uninstallString = uninstall;
    out->quietUninstallString = quiet;
    out->registryPath    = fullPath;
    out->is64View        = is64View;
    out->systemComponent = readDword(key, L"SystemComponent") == 1;

    // EstimatedSize 单位是 KB
    const QVariant sizeVar = readValue(key, L"EstimatedSize");
    out->sizeBytes = sizeVar.isValid()
                         ? static_cast<qint64>(sizeVar.toUInt()) * 1024
                         : -1;

    // 补丁/更新包的三个特征字段
    const QString parentKeyName = readString(key, L"ParentKeyName");
    const QString parentDisplay = readString(key, L"ParentDisplayName");
    const QString releaseType   = readString(key, L"ReleaseType");
    out->isUpdate = !parentKeyName.isEmpty() || !parentDisplay.isEmpty()
                    || releaseType.contains(QStringLiteral("Update"), Qt::CaseInsensitive)
                    || releaseType.contains(QStringLiteral("Hotfix"), Qt::CaseInsensitive)
                    || releaseType.contains(QStringLiteral("ServicePack"), Qt::CaseInsensitive);

    // 判定安装类型
    const bool windowsInstaller = readDword(key, L"WindowsInstaller") == 1;
    const QString guidFromCmd   = extractGuid(uninstall);
    out->productCode = !guidFromCmd.isEmpty()
                           ? guidFromCmd
                           : (looksLikeGuid(subKeyName) ? extractGuid(subKeyName) : QString());

    if (windowsInstaller || (uninstall.contains(QStringLiteral("msiexec"), Qt::CaseInsensitive))
        || looksLikeGuid(subKeyName))
    {
        out->type = PackageType::Msi;
    } else if (!uninstall.isEmpty() || !quiet.isEmpty()) {
        out->type = PackageType::Exe;
    } else if (!out->productCode.isEmpty()) {
        out->type = PackageType::Msi;
    } else {
        out->type = PackageType::Unknown;
    }

    out->keywords = buildKeywords(name, publisher, loc, uninstall);
    return true;
}

// 去重键：同名同版本同发布者视为同一个程序（32 位视图和 64 位视图常重复登记）
QString dedupeKeyOf(const InstalledApp &a)
{
    return QStringLiteral("%1|%2|%3")
        .arg(normalizeToken(a.name), normalizeToken(a.version), normalizeToken(a.publisher));
}

} // namespace

QVector<InstalledApp> RegistryScanner::scanAll()
{
    QVector<InstalledApp> raw;

    struct RootSpec {
        HKEY root;
        bool  is64;
        QString label;      // 只用于 id 与调试
    };

    const RootSpec specs[] = {
        {HKEY_LOCAL_MACHINE, true,  QStringLiteral("HKLM64")},
        {HKEY_LOCAL_MACHINE, false, QStringLiteral("HKLM32")},
        {HKEY_CURRENT_USER,  true,  QStringLiteral("HKCU64")},
        {HKEY_CURRENT_USER,  false, QStringLiteral("HKCU32")},
    };

    for (const RootSpec &spec : specs) {
        const DWORD view = spec.is64 ? KEY_WOW64_64KEY : KEY_WOW64_32KEY;

        HKEY uninstallRoot = nullptr;
        if (RegOpenKeyExW(spec.root, kUninstallPath, 0,
                          KEY_READ | view, &uninstallRoot) != ERROR_SUCCESS)
            continue;   // 这个视图不存在（例如纯 32 位系统没有 WOW6432Node）就跳过

        const QStringList subKeys = enumSubKeys(uninstallRoot);

        for (const QString &sub : subKeys) {
            HKEY itemKey = nullptr;
            if (RegOpenKeyExW(uninstallRoot, sub.toStdWString().c_str(), 0,
                              KEY_READ | view, &itemKey) != ERROR_SUCCESS)
                continue;

            InstalledApp app;
            const QString hiveName = (spec.root == HKEY_LOCAL_MACHINE)
                                         ? QStringLiteral("HKEY_LOCAL_MACHINE")
                                         : QStringLiteral("HKEY_CURRENT_USER");
            const QString fullPath = QStringLiteral("%1\\SOFTWARE\\Microsoft\\Windows\\"
                                                    "CurrentVersion\\Uninstall\\%2")
                                         .arg(hiveName, sub);

            if (parseEntry(itemKey, sub, fullPath, spec.is64, &app))
                raw.push_back(app);

            RegCloseKey(itemKey);
        }

        RegCloseKey(uninstallRoot);
    }

    // ---- 去重：机器级优先于用户级，64 位视图优先于 32 位视图 ----
    QVector<InstalledApp> result;
    QHash<QString, int> seen;      // dedupeKey → result 下标

    for (const InstalledApp &a : raw) {
        const QString k = dedupeKeyOf(a);
        auto it = seen.find(k);
        if (it == seen.end()) {
            seen.insert(k, result.size());
            result.push_back(a);
            continue;
        }

        // 已存在：决定是否用新的替换旧的
        InstalledApp &existing = result[it.value()];
        const bool existingIsMachine = existing.registryPath.startsWith(QStringLiteral("HKEY_LOCAL"));
        const bool newIsMachine      = a.registryPath.startsWith(QStringLiteral("HKEY_LOCAL"));

        bool preferNew = false;
        if (newIsMachine && !existingIsMachine)            preferNew = true;       // 机器级更权威
        else if (newIsMachine == existingIsMachine && !existing.is64View && a.is64View)
                                                            preferNew = true;     // 64 位视图更完整
        else if (existing.sizeBytes <= 0 && a.sizeBytes > 0) preferNew = true;    // 首选有大小信息的
        // 同名同版本两条登记项时，优先保留"产品码真的装着"的那条：
        // 安装器升级/卸载失败常留下一条指向已不存在产品码的僵尸键，
        // 选中它去 msiexec /X 只会拿到 1605。
        else if (msiProductInstalled(a.productCode) && !msiProductInstalled(existing.productCode))
                                                            preferNew = true;

        if (preferNew) existing = a;
    }

    return result;
}

} // namespace un
