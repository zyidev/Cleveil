// ============================================================================
//  winutil.cpp —— Win32 底层能力实现
//  说明：所有函数都做了失败兜底，出错时返回 false/空值并通过 error 出参
//        带回一句中文原因，调用方据此提示用户。
// ============================================================================

#include "core/winutil.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <winsvc.h>
#include <psapi.h>

#include <functional>
#include <string>
#include <vector>

namespace un {

// ---------------------------------------------------------------------------
// 内部小工具
// ---------------------------------------------------------------------------
namespace {

// 把 QString 转成 std::wstring（Windows API 一律用宽字符）
inline std::wstring toW(const QString &s)
{
    return s.toStdWString();
}

// 命令行参数打包：含空格或为空时用双引号包起来
QString quoteArg(const QString &arg)
{
    if (arg.isEmpty()) return QStringLiteral("\"\"");
    if (!arg.contains(QLatin1Char(' ')) && !arg.contains(QLatin1Char('\t')))
        return arg;
    QString escaped = arg;
    escaped.replace(QLatin1String("\""), QLatin1String("\\\""));
    return QStringLiteral("\"%1\"").arg(escaped);
}

QString buildCommandLine(const QString &program, const QStringList &args)
{
    QStringList parts;
    parts << quoteArg(program);
    for (const QString &a : args) parts << quoteArg(a);
    return parts.join(QLatin1Char(' '));
}

// 统一的错误信息格式："xxx（GetLastError=5）"
QString winErr(const QString &what)
{
    const DWORD code = GetLastError();
    return QStringLiteral("%1（错误码 %2）").arg(what).arg(code);
}

// 去掉 \\?\ 前缀，再用 GetFullPathName 消掉 . / ..，最后统一成正斜杠。
// 保留原始大小写；比较时由调用方转小写。
QString normalizedPath(const QString &input)
{
    QString p = input.trimmed();
    if (p.isEmpty()) return {};

    if (p.startsWith(QLatin1String("\\\\?\\UNC\\"), Qt::CaseInsensitive))
        p = QLatin1String("\\\\") + p.mid(8);
    else if (p.startsWith(QLatin1String("\\\\?\\")) || p.startsWith(QLatin1String("\\??\\")))
        p = p.mid(4);

    const std::wstring src = p.toStdWString();
    const DWORD need = GetFullPathNameW(src.c_str(), 0, nullptr, nullptr);
    if (need > 0 && need < 32768) {
        std::wstring buf(need, L'\0');
        const DWORD got = GetFullPathNameW(src.c_str(), need, buf.data(), nullptr);
        if (got > 0 && got < need)
            p = QString::fromWCharArray(buf.data(), static_cast<int>(got));
    }

    p = QDir::cleanPath(QDir::fromNativeSeparators(p));
    // 盘符根 "C:/" 长度为 3，保留斜杠；更深的路径去掉末尾斜杠，方便做前缀比较
    if (p.size() > 3 && p.endsWith(QLatin1Char('/')))
        p.chop(1);
    return p;
}

bool pathEquals(const QString &path, const QString &root)
{
    return path.compare(root, Qt::CaseInsensitive) == 0;
}

bool pathEqualsOrUnder(const QString &path, const QString &root)
{
    if (pathEquals(path, root)) return true;
    return path.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive);
}

// 注册表删除的最后一道闸：拒绝盘符级的系统键，以及 Uninstall 键本身。
// App Paths\某个.exe、Uninstall\{GUID} 这种更深的子键仍然允许删。
bool isProtectedRegistrySubpath(const QString &subPath)
{
    QString n = subPath;
    n.replace(QLatin1Char('/'), QLatin1Char('\\'));
    while (n.startsWith(QLatin1Char('\\'))) n.remove(0, 1);
    while (n.endsWith(QLatin1Char('\\'))) n.chop(1);
    n = n.toLower();
    if (n.isEmpty()) return true;

    static const QStringList blocked = {
        QStringLiteral("software"),
        QStringLiteral("software\\microsoft"),
        QStringLiteral("software\\microsoft\\windows"),
        QStringLiteral("software\\microsoft\\windows\\currentversion"),
        QStringLiteral("software\\microsoft\\windows\\currentversion\\uninstall"),
        QStringLiteral("software\\microsoft\\windows nt"),
        QStringLiteral("software\\microsoft\\windows nt\\currentversion"),
        QStringLiteral("software\\wow6432node"),
        QStringLiteral("software\\wow6432node\\microsoft"),
        QStringLiteral("software\\wow6432node\\microsoft\\windows"),
        QStringLiteral("software\\wow6432node\\microsoft\\windows\\currentversion"),
        QStringLiteral("software\\wow6432node\\microsoft\\windows\\currentversion\\uninstall"),
        QStringLiteral("software\\classes"),
        QStringLiteral("software\\policies"),
    };
    if (blocked.contains(n)) return true;
    if (n == QLatin1String("system") || n.startsWith(QLatin1String("system\\"))) return true;
    if (n == QLatin1String("sam") || n.startsWith(QLatin1String("sam\\"))) return true;
    if (n == QLatin1String("security") || n.startsWith(QLatin1String("security\\"))) return true;
    return false;
}

} // namespace

// ===========================================================================
// 一、字符串与环境变量
// ===========================================================================

QString expandEnv(const QString &text)
{
    if (text.isEmpty()) return text;

    const std::wstring src = toW(text);
    // 先问一次需要多大缓冲（含结尾 \0）
    DWORD need = ExpandEnvironmentStringsW(src.c_str(), nullptr, 0);
    if (need == 0) return text;

    std::wstring buf(need, L'\0');
    if (ExpandEnvironmentStringsW(src.c_str(), buf.data(), need) == 0) return text;

    while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    return QString::fromStdWString(buf);
}

QString unquote(const QString &text)
{
    QString s = text.trimmed();
    if (s.size() >= 2 && s.startsWith(QLatin1Char('"')) && s.endsWith(QLatin1Char('"')))
        s = s.mid(1, s.size() - 2).trimmed();
    return s;
}

QString normalizeToken(const QString &text)
{
    QString s = text.toLower();
    // 去掉常见"噪音词"，这些词几乎每个安装包都有，参与匹配会误伤
    static const QStringList noise = {
        QStringLiteral("(64-bit)"), QStringLiteral("(32-bit)"),
        QStringLiteral("64-bit"),   QStringLiteral("32-bit"),
        QStringLiteral("update"),   QStringLiteral("setup"),
        QStringLiteral("installer"),QStringLiteral("安装程序"),
        QStringLiteral("更新程序"), QStringLiteral("卸载")
    };
    for (const QString &n : noise) s.remove(n);

    // 只保留字母和数字（c.isLetterOrNumber() 对中文同样成立）
    QString out;
    out.reserve(s.size());
    for (const QChar c : s) {
        if (c.isLetterOrNumber()) out.append(c);
    }
    return out;
}

QString splitIconSpec(const QString &spec, int *index)
{
    if (index) *index = 0;

    QString s = unquote(spec.trimmed());
    if (s.startsWith(QLatin1Char('@'))) s.remove(0, 1);   // "@C:\x.dll,-1" 形式

    // 形如 "C:\a\b.exe,0" 或 "C:\a\b.dll,-101"：最后一段是图标索引
    const int comma = s.lastIndexOf(QLatin1Char(','));
    if (comma > 2) {   // >2 是为了避免把 "C:\x,y" 这种残缺路径也切了
        const QString tail = s.mid(comma + 1).trimmed();
        bool ok = false;
        const int v = tail.toInt(&ok);
        if (ok) {
            if (index) *index = v;
            s = unquote(s.left(comma).trimmed());
        }
    }
    return s;
}

bool isPathUnder(const QString &child, const QString &parent)
{
    const QString c = normalizedPath(child);
    QString p = normalizedPath(parent);
    if (c.isEmpty() || p.isEmpty()) return false;

    if (!p.endsWith(QLatin1Char('/'))) p += QLatin1Char('/');
    // 目录本身也算在其下。比较前补斜杠，避免 "C:/App" 命中 "C:/App2"。
    if (QString(c + QLatin1Char('/')).compare(p, Qt::CaseInsensitive) == 0) return true;
    return c.startsWith(p, Qt::CaseInsensitive);
}

bool isDangerousPath(const QString &path)
{
    const QString full = normalizedPath(path);
    if (full.isEmpty()) return true;

    const QString lower = full.toLower();
    const bool unc = lower.startsWith(QLatin1String("//"));
    const QStringList parts = lower.split(QLatin1Char('/'), Qt::SkipEmptyParts);

    // 盘符根、UNC 共享根本身一律拒绝。子目录（C:/Program Files/某程序）放行。
    if (unc) {
        if (parts.size() < 3) return true;
    } else {
        if (parts.size() < 2) return true;
    }

    if (!unc && parts.at(0).size() == 2 && parts.at(0).endsWith(QLatin1Char(':'))) {
        const QString drive = parts.at(0);          // "c:"
        const QString top = parts.at(1);

        // 这些目录连同其全部子路径都不能删（Windows\System32 也在其中）
        const QStringList treeRoots = {
            QStringLiteral("windows"),
            QStringLiteral("$recycle.bin"),
            QStringLiteral("system volume information"),
            QStringLiteral("recovery"),
            QStringLiteral("perflogs"),
            QStringLiteral("boot"),
            QStringLiteral("documents and settings"),
        };
        if (treeRoots.contains(top)) return true;

        // 卷上的这几个根目录本身不能删，它们的子目录（具体某个程序）可以
        const QStringList volumeRoots = {
            QStringLiteral("program files"),
            QStringLiteral("program files (x86)"),
            QStringLiteral("programdata"),
            QStringLiteral("users"),
        };
        if (volumeRoots.contains(top) && parts.size() == 2) return true;

        if ((top == QLatin1String("program files") || top == QLatin1String("program files (x86)"))
            && parts.size() >= 3)
        {
            const QString second = parts.at(2);
            const QStringList blockedSecond = {
                QStringLiteral("common files"),
                QStringLiteral("windowsapps"),
                QStringLiteral("windows defender"),
                QStringLiteral("windows nt"),
                QStringLiteral("internet explorer"),
                QStringLiteral("modifiablewindowsapps"),
                QStringLiteral("windows mail"),
                QStringLiteral("windows photo viewer"),
                QStringLiteral("windows sidebar"),
                QStringLiteral("windows security"),
            };
            if (blockedSecond.contains(second)) return true;
        }

        if (top == QLatin1String("programdata") && parts.size() >= 3
            && parts.at(2) == QLatin1String("microsoft"))
            return true;

        if (top == QLatin1String("users") && parts.size() >= 3) {
            const QString user = parts.at(2);
            const QStringList specialUsers = {
                QStringLiteral("public"),
                QStringLiteral("default"),
                QStringLiteral("default user"),
                QStringLiteral("all users"),
            };
            if (specialUsers.contains(user)) return true;
        }

        Q_UNUSED(drive)
    }

    // 实际的 Windows 目录（系统不一定装在 C:）
    wchar_t winDir[MAX_PATH] = {};
    const UINT winLen = GetWindowsDirectoryW(winDir, MAX_PATH);
    if (winLen > 0 && winLen < MAX_PATH) {
        const QString win = normalizedPath(QString::fromWCharArray(winDir, static_cast<int>(winLen))).toLower();
        if (!win.isEmpty() && pathEqualsOrUnder(lower, win)) return true;
    }

    const QString profile = normalizedPath(expandEnv(QStringLiteral("%USERPROFILE%"))).toLower();
    const QString roaming = normalizedPath(expandEnv(QStringLiteral("%APPDATA%"))).toLower();
    const QString localApp = normalizedPath(expandEnv(QStringLiteral("%LOCALAPPDATA%"))).toLower();
    if (!profile.isEmpty() && (pathEquals(lower, profile)
                               || pathEquals(lower, profile + QStringLiteral("/appdata"))))
        return true;
    if (!roaming.isEmpty() && pathEquals(lower, roaming)) return true;
    if (!localApp.isEmpty() && pathEquals(lower, localApp)) return true;
    if (!profile.isEmpty() && pathEquals(lower, profile + QStringLiteral("/appdata/locallow")))
        return true;

    return false;
}

bool isProtectedProcessName(const QString &exeName)
{
    const QString n = exeName.trimmed().toLower();
    static const QStringList names = {
        QStringLiteral("csrss.exe"),
        QStringLiteral("lsass.exe"),
        QStringLiteral("lsaiso.exe"),
        QStringLiteral("wininit.exe"),
        QStringLiteral("winlogon.exe"),
        QStringLiteral("services.exe"),
        QStringLiteral("smss.exe"),
        QStringLiteral("svchost.exe"),
        QStringLiteral("dwm.exe"),
        QStringLiteral("fontdrvhost.exe"),
        QStringLiteral("sihost.exe"),
        QStringLiteral("system"),
        QStringLiteral("registry"),
        QStringLiteral("secure system"),
        QStringLiteral("memory compression"),
    };
    return names.contains(n);
}

// ===========================================================================
// 二、图标
// ===========================================================================

QImage imageFromHIcon(HICON hIcon)
{
    if (!hIcon) return QImage();

    ICONINFO info{};
    if (!GetIconInfo(hIcon, &info)) return QImage();

    QImage result;

    BITMAP bmp{};
    if (info.hbmColor && GetObjectW(info.hbmColor, sizeof(bmp), &bmp) == sizeof(BITMAP)
        && bmp.bmWidth > 0 && bmp.bmHeight > 0
        && bmp.bmWidth <= 1024 && bmp.bmHeight <= 1024)
    {
        const int w = bmp.bmWidth;
        const int h = bmp.bmHeight;

        result = QImage(w, h, QImage::Format_ARGB32);
        result.fill(Qt::transparent);

        BITMAPINFO bi{};
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = w;
        bi.bmiHeader.biHeight      = -h;          // 负高度表示自上而下，省掉一次上下翻转
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        HDC hdc = GetDC(nullptr);
        GetDIBits(hdc, info.hbmColor, 0, static_cast<UINT>(h),
                  result.bits(), &bi, DIB_RGB_COLORS);
        ReleaseDC(nullptr, hdc);

        // 判断是否真带 alpha 通道：24 位图标被转成 32 位后 alpha 全是 0
        bool hasAlpha = false;
        for (int y = 0; y < h && !hasAlpha; ++y) {
            const uchar *row = result.constScanLine(y);
            for (int x = 0; x < w; ++x) {
                if (row[x * 4 + 3] != 0) { hasAlpha = true; break; }
            }
        }

        if (!hasAlpha) {
            // 没有 alpha 时用 AND 掩码补齐透明区域：掩码里白色 = 透明，黑色 = 不透明
            std::vector<uchar> mask(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
            if (info.hbmMask) {
                BITMAPINFO mbi{};
                mbi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
                mbi.bmiHeader.biWidth       = w;
                mbi.bmiHeader.biHeight      = -h;
                mbi.bmiHeader.biPlanes      = 1;
                mbi.bmiHeader.biBitCount    = 32;
                mbi.bmiHeader.biCompression = BI_RGB;

                QImage maskImg(w, h, QImage::Format_ARGB32);
                maskImg.fill(Qt::black);
                HDC hdc2 = GetDC(nullptr);
                GetDIBits(hdc2, info.hbmMask, 0, static_cast<UINT>(h),
                          maskImg.bits(), &mbi, DIB_RGB_COLORS);
                ReleaseDC(nullptr, hdc2);

                for (int y = 0; y < h; ++y) {
                    const uchar *mrow = maskImg.constScanLine(y);
                    for (int x = 0; x < w; ++x)
                        mask[static_cast<size_t>(y) * w + x] = (mrow[x * 4] > 127) ? 1 : 0;
                }
            }
            for (int y = 0; y < h; ++y) {
                uchar *row = result.scanLine(y);
                for (int x = 0; x < w; ++x) {
                    row[x * 4 + 3] = mask[static_cast<size_t>(y) * w + x] ? 0 : 255;
                }
            }
        }

        result = result.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }

    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask)  DeleteObject(info.hbmMask);
    return result;
}

QImage imageFromFileIcon(const QString &path, int index, int size)
{
    Q_UNUSED(size)

    if (path.isEmpty() || !QFileInfo::exists(path)) return QImage();
    const std::wstring wpath = toW(QDir::toNativeSeparators(path));

    // 第一选择：直接从 PE 资源里按索引抽图标。
    // ExtractIconEx 的 nIconIndex 为负数时，按"资源 ID"解释，正好对应 ,-101 这种写法。
    HICON hIcon = nullptr;
    if (ExtractIconExW(wpath.c_str(), index, &hIcon, nullptr, 1) > 0 && hIcon) {
        QImage img = imageFromHIcon(hIcon);
        DestroyIcon(hIcon);
        if (!img.isNull()) return img;
    }

    // 第二选择：交给 Shell 判断类型（对 .lnk / 目录 / 取不到图标的 exe 都有效）
    SHFILEINFOW sfi{};
    if (SHGetFileInfoW(wpath.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON)
        && sfi.hIcon)
    {
        QImage img = imageFromHIcon(sfi.hIcon);
        DestroyIcon(sfi.hIcon);
        if (!img.isNull()) return img;
    }

    return QImage();
}

// ===========================================================================
// 三、权限
// ===========================================================================

bool isRunningAsAdmin()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

    TOKEN_ELEVATION elev{};
    DWORD size = sizeof(elev);
    const bool ok = GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &size)
                    && elev.TokenIsElevated != 0;
    CloseHandle(token);
    return ok;
}

bool runAndWait(const QString &program, const QStringList &args,
                quint32 *exitCode, QString *error)
{
    if (program.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("命令为空");
        return false;
    }

    const QString cmdLine = args.isEmpty() ? QString() : buildCommandLine(program, args);

    // 注意：lpFile/lpParameters 必须指向生命周期覆盖 ShellExecuteEx 的缓冲，
    // 所以这里先声明成具名局部变量，不能直接用临时对象 .c_str()
    const std::wstring wprogram = toW(QDir::toNativeSeparators(program));
    const std::wstring wargs    = toW(cmdLine);

    SHELLEXECUTEINFOW sei{};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb       = L"open";
    sei.lpFile       = wprogram.c_str();
    sei.lpParameters = cmdLine.isEmpty() ? nullptr : wargs.c_str();
    sei.nShow        = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        if (error) *error = winErr(QStringLiteral("无法启动卸载程序"));
        return false;
    }
    if (!sei.hProcess) {
        if (error) *error = QStringLiteral("卸载程序已启动，但无法等待其结束");
        return false;
    }

    WaitForSingleObject(sei.hProcess, INFINITE);

    DWORD code = 0;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);

    if (exitCode) *exitCode = static_cast<quint32>(code);
    return true;
}

bool runAsAdminAndWait(const QString &program, const QStringList &args,
                       quint32 *exitCode, QString *error)
{
    if (program.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("命令为空");
        return false;
    }

    const QString cmdLine = args.isEmpty() ? QString() : buildCommandLine(program, args);

    const std::wstring wprogram = toW(QDir::toNativeSeparators(program));
    const std::wstring wargs    = toW(cmdLine);

    SHELLEXECUTEINFOW sei{};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb       = L"runas";     // 触发 UAC 提权
    sei.lpFile       = wprogram.c_str();
    sei.lpParameters = cmdLine.isEmpty() ? nullptr : wargs.c_str();
    sei.nShow        = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        const DWORD code = GetLastError();
        if (error) {
            *error = (code == ERROR_CANCELLED)
                         ? QStringLiteral("用户取消了管理员授权")
                         : winErr(QStringLiteral("以管理员身份启动失败"));
        }
        return false;
    }

    if (!sei.hProcess) return true;    // 已交接给系统，无法等待

    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    if (exitCode) *exitCode = static_cast<quint32>(code);
    return true;
}

bool relaunchSelfAsAdmin(const QStringList &args)
{
    const QString exe = QCoreApplication::applicationFilePath();

    const std::wstring wexe   = toW(QDir::toNativeSeparators(exe));
    const std::wstring wargs  = toW(args.join(QLatin1Char(' ')));

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask  = SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = wexe.c_str();
    sei.lpParameters = args.isEmpty() ? nullptr : wargs.c_str();
    sei.nShow = SW_SHOWNORMAL;

    return ShellExecuteExW(&sei) != FALSE;
}

// ===========================================================================
// 四、文件系统
// ===========================================================================

bool recyclePaths(const QStringList &paths, QString *error)
{
    if (paths.isEmpty()) return true;

    // SHFileOperation 要求：绝对路径、路径之间用 \0 分隔、整个列表以 \0\0 结尾
    std::wstring from;
    for (const QString &p : paths) {
        const QString normalized = normalizedPath(p);
        const QString full = QDir::toNativeSeparators(normalized);

        // 最后一道安全防线：危险路径直接拒绝
        if (normalized.isEmpty() || isDangerousPath(normalized)) {
            if (error) *error = QStringLiteral("已阻止删除受保护的系统路径：%1").arg(full.isEmpty() ? p : full);
            return false;
        }

        // 目录连接点 / 符号链接的目标可能在系统目录里，删链接有时会波及目标
        const DWORD attr = GetFileAttributesW(full.toStdWString().c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
            if (error) *error = QStringLiteral("已拒绝删除符号链接或目录连接点：%1").arg(full);
            return false;
        }

        from += full.toStdWString();
        from.push_back(L'\0');
    }
    from.push_back(L'\0');

    SHFILEOPSTRUCTW op{};
    op.hwnd   = nullptr;
    op.wFunc  = FO_DELETE;
    op.pFrom  = from.c_str();
    op.pTo    = nullptr;
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;

    const int ret = SHFileOperationW(&op);
    if (ret != 0) {
        if (error) *error = QStringLiteral("移入回收站失败（SHFileOperation 返回 %1）").arg(ret);
        return false;
    }
    return true;
}

qint64 directorySize(const QString &path, int maxDepth, int *fileCount)
{
    qint64 total = 0;
    int count = 0;

    if (!QFileInfo::exists(path)) {
        if (fileCount) *fileCount = 0;
        return 0;
    }

    const QFileInfo rootInfo(path);
    if (rootInfo.isFile()) {
        if (fileCount) *fileCount = 1;
        return rootInfo.size();
    }

    // 手写递归而不是 QDirIterator，好处是能精确控制深度、能跳过符号链接
    std::function<void(const QString &, int)> walk = [&](const QString &dir, int depth) {
        QDir d(dir);
        const QFileInfoList entries = d.entryInfoList(
            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
            QDir::Name);

        for (const QFileInfo &fi : entries) {
            if (fi.isSymLink()) continue;              // 防止链接成环
            if (fi.isDir()) {
                if (depth < maxDepth) walk(fi.absoluteFilePath(), depth + 1);
            } else {
                total += fi.size();
                ++count;
            }
        }
    };
    walk(path, 0);

    if (fileCount) *fileCount = count;
    return total;
}

QString formatBytes(qint64 bytes)
{
    if (bytes < 0) return QStringLiteral("未知");
    if (bytes < 1024) return QStringLiteral("%1 B").arg(bytes);

    double v = static_cast<double>(bytes) / 1024.0;
    if (v < 1024.0) return QStringLiteral("%1 KB").arg(v, 0, 'f', 0);

    v /= 1024.0;
    if (v < 1024.0) return QStringLiteral("%1 MB").arg(v, 0, 'f', 1);

    v /= 1024.0;
    return QStringLiteral("%1 GB").arg(v, 0, 'f', 2);
}

// ===========================================================================
// 五、注册表
// ===========================================================================

bool splitRegistryPath(const QString &fullPath, HKEY *root, QString *subPath)
{
    QString p = fullPath.trimmed();
    while (p.startsWith(QLatin1Char('\\'))) p.remove(0, 1);

    const int slash = p.indexOf(QLatin1Char('\\'));
    const QString head = (slash < 0 ? p : p.left(slash)).toUpper();
    const QString rest = (slash < 0 ? QString() : p.mid(slash + 1));

    struct Entry { const char *name; HKEY key; };
    static const Entry kEntries[] = {
        {"HKEY_LOCAL_MACHINE",  HKEY_LOCAL_MACHINE},
        {"HKLM",                HKEY_LOCAL_MACHINE},
        {"HKEY_CURRENT_USER",   HKEY_CURRENT_USER},
        {"HKCU",                HKEY_CURRENT_USER},
        {"HKEY_CLASSES_ROOT",   HKEY_CLASSES_ROOT},
        {"HKCR",                HKEY_CLASSES_ROOT},
        {"HKEY_USERS",          HKEY_USERS},
        {"HKU",                 HKEY_USERS},
        {"HKEY_CURRENT_CONFIG", HKEY_CURRENT_CONFIG},
        {"HKCC",                HKEY_CURRENT_CONFIG},
    };

    for (const Entry &e : kEntries) {
        if (head == QLatin1String(e.name)) {
            if (root) *root = e.key;
            if (subPath) *subPath = rest;
            return true;
        }
    }
    return false;
}

bool deleteRegistryKey(const QString &fullPath, bool is64View, QString *error)
{
    HKEY root = nullptr;
    QString sub;
    if (!splitRegistryPath(fullPath, &root, &sub) || sub.isEmpty()) {
        if (error) *error = QStringLiteral("注册表路径无法解析：%1").arg(fullPath);
        return false;
    }
    if (isProtectedRegistrySubpath(sub)) {
        if (error) *error = QStringLiteral("已拒绝删除受保护的注册表项：%1").arg(fullPath);
        return false;
    }

    // 32 位与 64 位视图是两份独立数据，必须带对应的 KEY_WOW64 标志
    const DWORD view = is64View ? KEY_WOW64_64KEY : KEY_WOW64_32KEY;

    HKEY key = nullptr;
    const LONG openRet = RegOpenKeyExW(root, toW(sub).c_str(), 0,
                                       KEY_READ | KEY_WRITE | view, &key);
    if (openRet != ERROR_SUCCESS) {
        if (error) *error = QStringLiteral("打开注册表项失败（%1）").arg(openRet);
        return false;
    }

    bool ok = true;
    // RegDeleteTree 会删掉所有子键和值，但保留 key 自身
    const LONG delRet = RegDeleteTreeW(key, nullptr);
    if (delRet != ERROR_SUCCESS && delRet != ERROR_FILE_NOT_FOUND) {
        ok = false;
        if (error) *error = QStringLiteral("删除子键失败（%1）").arg(delRet);
    }
    RegCloseKey(key);

    // 再把 key 自身删掉
    const LONG keyRet = RegDeleteKeyExW(root, toW(sub).c_str(), view, 0);
    if (keyRet != ERROR_SUCCESS) {
        ok = false;
        if (error && error->isEmpty())
            *error = QStringLiteral("删除注册表项失败（%1）").arg(keyRet);
    }
    return ok;
}

// 判断注册表键是否存在
bool registryKeyExists(const QString &fullPath, bool is64View)
{
    HKEY root = nullptr;
    QString sub;
    if (!splitRegistryPath(fullPath, &root, &sub) || sub.isEmpty()) return false;

    const DWORD view = is64View ? KEY_WOW64_64KEY : KEY_WOW64_32KEY;

    HKEY key = nullptr;
    const LONG ret = RegOpenKeyExW(root, toW(sub).c_str(), 0, KEY_READ | view, &key);
    if (ret == ERROR_SUCCESS) {
        RegCloseKey(key);
        return true;
    }
    return false;
}

// 只删除键下的某一个值（清理 Run 启动项用）
bool deleteRegistryValue(const QString &keyPath, const QString &valueName,
                         bool is64View, QString *error)
{
    HKEY root = nullptr;
    QString sub;
    if (!splitRegistryPath(keyPath, &root, &sub) || sub.isEmpty()) {
        if (error) *error = QStringLiteral("注册表路径无法解析：%1").arg(keyPath);
        return false;
    }

    const DWORD view = is64View ? KEY_WOW64_64KEY : KEY_WOW64_32KEY;

    HKEY key = nullptr;
    const LONG openRet = RegOpenKeyExW(root, toW(sub).c_str(), 0, KEY_SET_VALUE | view, &key);
    if (openRet != ERROR_SUCCESS) {
        if (error) *error = QStringLiteral("打开注册表项失败（%1）").arg(openRet);
        return false;
    }

    const LONG delRet = RegDeleteValueW(key, toW(valueName).c_str());
    RegCloseKey(key);

    if (delRet != ERROR_SUCCESS) {
        if (error) *error = QStringLiteral("删除注册表值失败（%1）").arg(delRet);
        return false;
    }
    return true;
}

bool backupRegistryKey(const QString &fullPath, const QString &regFile, QString *error)
{
    if (fullPath.isEmpty()) return false;

    // 用系统自带的 reg.exe 导出，产出的 .reg 用户双击就能还原，比二进制 hive 更友好
    QProcess proc;
    proc.setProgram(QStringLiteral("reg.exe"));
    proc.setArguments({QStringLiteral("export"), fullPath,
                       QDir::toNativeSeparators(regFile), QStringLiteral("/y")});
    proc.start();
    if (!proc.waitForFinished(15000)) {
        proc.kill();
        proc.waitForFinished(2000);
        if (error) *error = QStringLiteral("reg export 超时");
        return false;
    }
    if (proc.exitCode() != 0) {
        if (error) {
            *error = QStringLiteral("注册表备份失败：%1")
                         .arg(QString::fromLocal8Bit(proc.readAllStandardError()).trimmed());
        }
        return false;
    }
    return QFileInfo::exists(regFile);
}

// ===========================================================================
// 六、进程与服务
// ===========================================================================

QVector<ProcessRef> enumerateProcesses()
{
    QVector<ProcessRef> result;

    // EnumProcesses 需要先给一块缓冲区，不够再翻倍重试
    std::vector<DWORD> pids(1024);
    DWORD bytesNeeded = 0;
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (!EnumProcesses(pids.data(), static_cast<DWORD>(pids.size() * sizeof(DWORD)),
                           &bytesNeeded)) {
            return result;
        }
        if (bytesNeeded < pids.size() * sizeof(DWORD)) break;
        pids.resize(pids.size() * 2);
    }

    const size_t count = bytesNeeded / sizeof(DWORD);
    result.reserve(static_cast<int>(count));

    std::vector<wchar_t> pathBuf(32768);

    for (size_t i = 0; i < count && i < pids.size(); ++i) {
        const DWORD pid = pids[i];
        if (pid == 0) continue;

        ProcessRef ref;
        ref.pid = static_cast<quint32>(pid);

        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h) {
            DWORD len = static_cast<DWORD>(pathBuf.size());
            if (QueryFullProcessImageNameW(h, 0, pathBuf.data(), &len) && len > 0) {
                ref.path = QString::fromStdWString(std::wstring(pathBuf.data(), len));
                ref.name = QFileInfo(ref.path).fileName();
            }
            CloseHandle(h);
        }
        if (!ref.path.isEmpty()) result.push_back(ref);
    }
    return result;
}

QVector<ProcessRef> processesUnder(const QString &directory)
{
    QVector<ProcessRef> result;
    if (directory.isEmpty()) return result;

    const QVector<ProcessRef> all = enumerateProcesses();
    for (const ProcessRef &p : all) {
        if (isPathUnder(p.path, directory)) result.push_back(p);
    }
    return result;
}

bool killProcess(quint32 pid, QString *error)
{
    HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) {
        if (error) *error = winErr(QStringLiteral("打开进程失败（可能权限不足）"));
        return false;
    }
    const bool ok = TerminateProcess(h, 1) != 0;
    if (!ok && error) *error = winErr(QStringLiteral("终止进程失败"));
    CloseHandle(h);
    return ok;
}

bool killProcessIfUnder(quint32 pid, const QString &directory, QString *error)
{
    if (pid <= 4 || directory.trimmed().isEmpty() || isDangerousPath(directory)) {
        if (error) *error = QStringLiteral("已拒绝结束该进程");
        return false;
    }

    HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) {
        if (error) *error = winErr(QStringLiteral("打开进程失败（可能权限不足）"));
        return false;
    }

    wchar_t buf[32768] = {};
    DWORD len = static_cast<DWORD>(sizeof(buf) / sizeof(buf[0]));
    if (!QueryFullProcessImageNameW(h, 0, buf, &len) || len == 0) {
        CloseHandle(h);
        if (error) *error = QStringLiteral("无法确认进程路径，已拒绝结束");
        return false;
    }

    const QString image = QString::fromWCharArray(buf, static_cast<int>(len));
    const QString name = QFileInfo(image).fileName();
    if (isProtectedProcessName(name) || isDangerousPath(image) || !isPathUnder(image, directory)) {
        CloseHandle(h);
        if (error) *error = QStringLiteral("已拒绝结束系统进程或不在安装目录内的进程");
        return false;
    }

    const bool ok = TerminateProcess(h, 1) != 0;
    if (!ok && error) *error = winErr(QStringLiteral("终止进程失败"));
    CloseHandle(h);
    return ok;
}

QVector<ServiceRef> enumerateServices()
{
    QVector<ServiceRef> result;

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) return result;

    DWORD bytesNeeded = 0;
    DWORD servicesReturned = 0;
    DWORD resume = 0;

    // 先用空缓冲拿所需大小
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
                          nullptr, 0, &bytesNeeded, &servicesReturned, &resume, nullptr);

    std::vector<BYTE> buffer(bytesNeeded + 1024);
    resume = 0;
    if (EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
                              buffer.data(), static_cast<DWORD>(buffer.size()),
                              &bytesNeeded, &servicesReturned, &resume, nullptr))
    {
        auto *items = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW *>(buffer.data());
        for (DWORD i = 0; i < servicesReturned; ++i) {
            ServiceRef ref;
            ref.name        = QString::fromStdWString(items[i].lpServiceName ? items[i].lpServiceName : L"");
            ref.displayName = QString::fromStdWString(items[i].lpDisplayName ? items[i].lpDisplayName : L"");

            // 取服务的可执行文件路径（ImagePath）
            SC_HANDLE svc = OpenServiceW(scm, items[i].lpServiceName, SERVICE_QUERY_CONFIG);
            if (svc) {
                DWORD cfgSize = 0;
                QueryServiceConfigW(svc, nullptr, 0, &cfgSize);
                if (cfgSize > 0) {
                    std::vector<BYTE> cfg(cfgSize);
                    if (QueryServiceConfigW(svc,
                                            reinterpret_cast<QUERY_SERVICE_CONFIGW *>(cfg.data()),
                                            cfgSize, &cfgSize))
                    {
                        auto *qsc = reinterpret_cast<QUERY_SERVICE_CONFIGW *>(cfg.data());
                        if (qsc->lpBinaryPathName)
                            ref.binaryPath = QString::fromStdWString(qsc->lpBinaryPathName);
                    }
                }
                CloseServiceHandle(svc);
            }
            result.push_back(ref);
        }
    }

    CloseServiceHandle(scm);
    return result;
}

bool deleteService(const QString &serviceName, QString *error)
{
    static const QStringList protectedServices = {
        QStringLiteral("WinDefend"), QStringLiteral("WdNisSvc"), QStringLiteral("Sense"),
        QStringLiteral("SecurityHealthService"), QStringLiteral("wuauserv"),
        QStringLiteral("UsoSvc"), QStringLiteral("EventLog"), QStringLiteral("RpcSs"),
        QStringLiteral("Schedule"), QStringLiteral("SamSs"), QStringLiteral("CryptSvc"),
        QStringLiteral("Winmgmt"), QStringLiteral("Dhcp"), QStringLiteral("Dnscache"),
        QStringLiteral("BFE"), QStringLiteral("mpssvc"), QStringLiteral("wscsvc"),
        QStringLiteral("DcomLaunch"), QStringLiteral("LSM"), QStringLiteral("Power"),
        QStringLiteral("ProfSvc"), QStringLiteral("Themes"), QStringLiteral("AudioSrv"),
        QStringLiteral("BrokerInfrastructure"), QStringLiteral("SystemEventsBroker"),
    };
    if (protectedServices.contains(serviceName, Qt::CaseInsensitive)) {
        if (error) *error = QStringLiteral("已拒绝删除系统服务：%1").arg(serviceName);
        return false;
    }

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        if (error) *error = winErr(QStringLiteral("无法连接服务管理器（需要管理员权限）"));
        return false;
    }

    bool ok = false;
    SC_HANDLE svc = OpenServiceW(scm, toW(serviceName).c_str(), DELETE | SERVICE_STOP);
    if (svc) {
        SERVICE_STATUS st{};
        ControlService(svc, SERVICE_CONTROL_STOP, &st);   // 先尝试停止（失败也无所谓）
        ok = DeleteService(svc) != 0;
        if (!ok && error) *error = winErr(QStringLiteral("删除服务失败"));
        CloseServiceHandle(svc);
    } else if (error) {
        *error = winErr(QStringLiteral("打开服务失败"));
    }

    CloseServiceHandle(scm);
    return ok;
}

} // namespace un
