// ============================================================================
//  iconprovider.cpp —— 图标解析策略 + 缓存
// ============================================================================

#include "core/iconprovider.h"
#include "core/winutil.h"

#include <QDir>
#include <QFileInfo>
#include <QPainter>
#include <QPixmap>

namespace un {

namespace {

// 从一条卸载命令行里猜出可执行文件路径。
// 例：`"C:\Program Files\Foo\uninst.exe" /S` → C:\Program Files\Foo\uninst.exe
QString guessExecutable(const QString &cmdLine)
{
    QString s = cmdLine.trimmed();
    if (s.isEmpty()) return QString();

    if (s.startsWith(QLatin1Char('"'))) {
        const int end = s.indexOf(QLatin1Char('"'), 1);
        if (end > 1) return s.mid(1, end - 1);
    }

    // 没有引号时，以 .exe / .dll / .ico 结尾的位置为界截断
    const QString lower = s.toLower();
    for (const QString &ext : {QStringLiteral(".exe"), QStringLiteral(".dll"), QStringLiteral(".ico")}) {
        const int pos = lower.indexOf(ext);
        if (pos > 0) return s.left(pos + ext.size());
    }

    // 再退一步：取第一个空格之前的部分
    const int space = s.indexOf(QLatin1Char(' '));
    return space > 0 ? s.left(space) : s;
}

} // namespace

IconProvider &IconProvider::instance()
{
    static IconProvider provider;
    return provider;
}

void IconProvider::clear()
{
    m_cache.clear();
}

QString IconProvider::resolveIconPath(const InstalledApp &app, int *index) const
{
    if (index) *index = 0;

    // 1) DisplayIcon 是最可靠的来源（"C:\App\app.exe,0" 形式）
    if (!app.iconSource.trimmed().isEmpty()) {
        const QString path = splitIconSpec(app.iconSource, index);
        if (!path.isEmpty() && QFileInfo::exists(expandEnv(path)))
            return expandEnv(path);
    }

    // 2) 卸载程序通常和主程序同目录、同图标资源，可拿来兜底
    const QString exe = guessExecutable(app.uninstallString);
    if (!exe.isEmpty() && QFileInfo::exists(exe)) return exe;

    // 3) 最后用安装目录（Shell 会给一个文件夹图标）
    if (!app.installLocation.trimmed().isEmpty()) {
        const QString dir = expandEnv(app.installLocation.trimmed());
        if (QFileInfo::exists(dir)) return dir;
    }

    return QString();
}

QPixmap IconProvider::defaultPixmap(int size) const
{
    // 用 QPainter 现场画一个"软件盒"占位图标，避免为一张图引入资源文件
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF box(2.0, 2.0, size - 4.0, size - 4.0);
    p.setBrush(QColor(0x4a, 0x5b, 0x8c));          // 深靛蓝，和界面主色一致
    p.setPen(QPen(QColor(0x33, 0x41, 0x66), 1.0));
    p.drawRoundedRect(box, size / 6.0, size / 6.0);

    p.setPen(QPen(QColor(0xe8, 0xee, 0xff), 1.2));
    p.drawLine(QPointF(size * 0.28, size * 0.38), QPointF(size * 0.72, size * 0.38));
    p.drawLine(QPointF(size * 0.28, size * 0.52), QPointF(size * 0.72, size * 0.52));
    p.drawLine(QPointF(size * 0.28, size * 0.66), QPointF(size * 0.58, size * 0.66));
    p.end();

    return pm;
}

QIcon IconProvider::icon(const InstalledApp &app, int size)
{
    const QString key = QStringLiteral("%1|%2").arg(app.id).arg(size);
    auto it = m_cache.constFind(key);
    if (it != m_cache.constEnd()) return it.value();

    QIcon result;
    int index = 0;
    const QString path = resolveIconPath(app, &index);
    if (!path.isEmpty()) {
        const QImage img = imageFromFileIcon(path, index, size);
        if (!img.isNull()) {
            const QPixmap pm = QPixmap::fromImage(
                img.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            result = QIcon(pm);
        }
    }

    if (result.isNull()) result = QIcon(defaultPixmap(size));

    m_cache.insert(key, result);
    return result;
}

} // namespace un
