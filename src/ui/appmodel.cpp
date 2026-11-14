// ============================================================================
//  appmodel.cpp —— 表格模型实现
// ============================================================================

#include "ui/appmodel.h"
#include "core/iconprovider.h"
#include "core/winutil.h"

#include <QBrush>
#include <QColor>
#include <QFont>

namespace un {

AppTableModel::AppTableModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

void AppTableModel::setApps(QVector<InstalledApp> apps)
{
    beginResetModel();
    m_apps = std::move(apps);
    endResetModel();
}

const InstalledApp &AppTableModel::appAt(int row) const
{
    static const InstalledApp kEmpty;
    if (row < 0 || row >= m_apps.size()) return kEmpty;
    return m_apps.at(row);
}

int AppTableModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_apps.size();
}

int AppTableModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant AppTableModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_apps.size()) return QVariant();

    const InstalledApp &app = m_apps.at(index.row());

    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case Name:        return app.name;
        case Publisher:   return app.publisher;
        case Version:     return app.version;
        case InstallDate: return app.installDate.isValid()
                                 ? app.installDate.toString(QStringLiteral("yyyy-MM-dd"))
                                 : QStringLiteral("—");
        case Size:        return app.sizeBytes >= 0 ? formatBytes(app.sizeBytes)
                                                    : QStringLiteral("—");
        case Type:        return packageTypeText(app.type);
        default:          return QVariant();
        }

    case Qt::DecorationRole:
        // 只有第一列带图标；图标在 IconProvider 里缓存，滚动时不会重复解析
        if (index.column() == Name) return IconProvider::instance().icon(app, 28);
        return QVariant();

    case Qt::TextAlignmentRole:
        if (index.column() == Size)
            return QVariant(Qt::AlignRight | Qt::AlignVCenter);
        return QVariant(Qt::AlignLeft | Qt::AlignVCenter);

    case Qt::ToolTipRole:
        switch (index.column()) {
        case Name:      // 名称列经常被列宽截断，悬停时给出完整信息
            return QStringLiteral("%1\n发布者：%2\n版本：%3\n安装位置：%4")
                .arg(app.name,
                     app.publisher.isEmpty() ? QStringLiteral("未知") : app.publisher,
                     app.version.isEmpty() ? QStringLiteral("未知") : app.version,
                     app.installLocation.isEmpty() ? QStringLiteral("未记录")
                                                   : app.installLocation);
        case Type:      return app.canUninstall()
                                 ? QStringLiteral("卸载命令：\n%1").arg(app.uninstallString)
                                 : QStringLiteral("该程序没有登记卸载命令");
        default:        return QVariant();
        }

    case Qt::ForegroundRole:
        // 没有卸载命令的程序用浅灰提示"只能强制卸载"
        if (!app.canUninstall() && index.column() == Name)
            return QBrush(QColor(0x8a, 0x90, 0x9c));
        return QVariant();

    case Qt::FontRole:
        if (index.column() == Name) {
            QFont f;
            f.setBold(false);
            return f;
        }
        return QVariant();

    default:
        return QVariant();
    }
}

QVariant AppTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return QVariant();

    switch (section) {
    case Name:        return QStringLiteral("名称");
    case Publisher:   return QStringLiteral("发布者");
    case Version:     return QStringLiteral("版本");
    case InstallDate: return QStringLiteral("安装日期");
    case Size:        return QStringLiteral("占用");
    case Type:        return QStringLiteral("类型");
    default:          return QVariant();
    }
}

// ===========================================================================
//  AppFilterProxy
// ===========================================================================

AppFilterProxy::AppFilterProxy(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    setDynamicSortFilter(true);
    setSortCaseSensitivity(Qt::CaseInsensitive);
}

void AppFilterProxy::setQuery(const QString &query)
{
    m_query = query.trimmed();
    beginFilterChange();
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

void AppFilterProxy::setPublisherFilter(const QString &mode)
{
    m_publisherMode = mode;
    beginFilterChange();
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

void AppFilterProxy::setShowSystemComponents(bool show)
{
    m_showSystemComponents = show;
    beginFilterChange();
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

void AppFilterProxy::setShowUpdates(bool show)
{
    m_showUpdates = show;
    beginFilterChange();
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

bool AppFilterProxy::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    Q_UNUSED(sourceParent)

    const AppTableModel *model = qobject_cast<const AppTableModel *>(sourceModel());
    if (!model) return true;

    const InstalledApp &app = model->appAt(sourceRow);

    // 系统组件与更新补丁默认隐藏（和"设置-应用"的行为一致）
    if (app.systemComponent && !m_showSystemComponents) return false;
    if (app.isUpdate && !m_showUpdates) return false;

    // 发布者筛选
    if (m_publisherMode == QStringLiteral("微软")) {
        if (!app.publisher.contains(QStringLiteral("Microsoft"), Qt::CaseInsensitive))
            return false;
    } else if (m_publisherMode == QStringLiteral("第三方")) {
        if (app.publisher.contains(QStringLiteral("Microsoft"), Qt::CaseInsensitive))
            return false;
    }

    // 关键词搜索：名称、发布者、版本、安装路径都参与匹配
    if (m_query.isEmpty()) return true;

    const QStringList fields = {app.name, app.publisher, app.version, app.installLocation};
    for (const QString &f : fields) {
        if (f.contains(m_query, Qt::CaseInsensitive)) return true;
    }
    return false;
}

bool AppFilterProxy::lessThan(const QModelIndex &left, const QModelIndex &right) const
{
    const AppTableModel *model = qobject_cast<const AppTableModel *>(sourceModel());
    if (!model) return QSortFilterProxyModel::lessThan(left, right);

    const InstalledApp &a = model->appAt(left.row());
    const InstalledApp &b = model->appAt(right.row());

    switch (left.column()) {
    case AppTableModel::InstallDate: {
        // 无日期的排最后
        const bool va = a.installDate.isValid();
        const bool vb = b.installDate.isValid();
        if (va != vb) return vb;      // 有效日期排在无效之前
        if (!va && !vb) return false;
        return a.installDate < b.installDate;
    }
    case AppTableModel::Size: {
        const qint64 sa = a.sizeBytes < 0 ? -1 : a.sizeBytes;
        const qint64 sb = b.sizeBytes < 0 ? -1 : b.sizeBytes;
        return sa < sb;
    }
    case AppTableModel::Publisher:
        return a.publisher.compare(b.publisher, Qt::CaseInsensitive) < 0;
    case AppTableModel::Version:
        return a.version.compare(b.version, Qt::CaseInsensitive) < 0;
    case AppTableModel::Type:
        return packageTypeText(a.type).compare(packageTypeText(b.type),
                                              Qt::CaseInsensitive) < 0;
    case AppTableModel::Name:
    default:
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    }
}

} // namespace un
