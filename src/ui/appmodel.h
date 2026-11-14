#pragma once

// ============================================================================
//  appmodel.h —— 列表数据模型与过滤/排序代理
//  把"已安装程序"表直接喂给 QTableView，排序与过滤都交给代理模型，
//  这样界面代码里就不用写任何筛选逻辑了。
// ============================================================================

#include "core/appinfo.h"

#include <QAbstractTableModel>
#include <QSortFilterProxyModel>
#include <QVector>

namespace un {

class AppTableModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        Name = 0,
        Publisher,
        Version,
        InstallDate,
        Size,
        Type,
        ColumnCount
    };

    explicit AppTableModel(QObject *parent = nullptr);

    void setApps(QVector<InstalledApp> apps);
    const QVector<InstalledApp> &apps() const { return m_apps; }
    const InstalledApp &appAt(int row) const;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

private:
    QVector<InstalledApp> m_apps;
};

// ----------------------------------------------------------------------------
// 过滤 + 排序代理
// ----------------------------------------------------------------------------
class AppFilterProxy : public QSortFilterProxyModel {
    Q_OBJECT

public:
    explicit AppFilterProxy(QObject *parent = nullptr);

    void setQuery(const QString &query);          // 搜索框内容
    void setPublisherFilter(const QString &mode); // "全部" / "微软" / "第三方"
    void setShowSystemComponents(bool show);
    void setShowUpdates(bool show);

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;

private:
    QString m_query;
    QString m_publisherMode = QStringLiteral("全部");
    bool    m_showSystemComponents = false;
    bool    m_showUpdates = false;
};

} // namespace un
