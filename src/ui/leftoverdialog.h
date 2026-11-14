#pragma once

// ============================================================================
//  leftoverdialog.h —— 卸载后的"残留清理"向导
//  扫描在后台线程跑（要遍历注册表/磁盘/计划任务，不能卡界面），
//  结果按类别分组展示，用户勾选后才删除。
// ============================================================================

#include "core/appinfo.h"
#include "core/leftovers.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QVector>

QT_BEGIN_NAMESPACE
class QLabel;
class QProgressBar;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
QT_END_NAMESPACE

namespace un {

class LeftoverDialog : public QDialog {
    Q_OBJECT

public:
    explicit LeftoverDialog(const InstalledApp &app, QWidget *parent = nullptr);

private slots:
    void onScanFinished();
    void onTreeItemChanged(QTreeWidgetItem *item, int column);
    void selectAll(bool checked);
    void removeSelected();
    void onRemovalFinished();
    void killRelatedProcesses();

private:
    void buildUi();
    void populateTree();
    QVector<LeftoverItem> checkedItems() const;
    void updateSummary();

    InstalledApp m_app;
    QVector<LeftoverItem> m_items;

    QFutureWatcher<QVector<LeftoverItem>> *m_scanWatcher = nullptr;
    QFutureWatcher<RemovalResult>         *m_removeWatcher = nullptr;

    QLabel        *m_headline = nullptr;
    QLabel        *m_status = nullptr;
    QLabel        *m_summary = nullptr;
    QProgressBar  *m_progress = nullptr;
    QTreeWidget   *m_tree = nullptr;
    QPushButton   *m_selectAllBtn = nullptr;
    QPushButton   *m_selectNoneBtn = nullptr;
    QPushButton   *m_removeBtn = nullptr;
    QPushButton   *m_killBtn = nullptr;
};

} // namespace un
