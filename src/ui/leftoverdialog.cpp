// ============================================================================
//  leftoverdialog.cpp —— 残留勾选与清理
// ============================================================================

#include "ui/leftoverdialog.h"
#include "core/remover.h"
#include "core/winutil.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <algorithm>
#include <QDesktopServices>
#include <QFuture>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

namespace un {

LeftoverDialog::LeftoverDialog(const InstalledApp &app, QWidget *parent)
    : QDialog(parent)
    , m_app(app)
{
    setWindowTitle(QStringLiteral("残留清理 - %1").arg(app.name));
    setModal(true);
    resize(880, 600);

    buildUi();

    // ---- 后台扫描 ----
    m_scanWatcher = new QFutureWatcher<QVector<LeftoverItem>>(this);
    connect(m_scanWatcher, &QFutureWatcher<QVector<LeftoverItem>>::finished,
            this, &LeftoverDialog::onScanFinished);

    // 进度回调来自工作线程，必须回到 UI 线程再改控件
    LeftoverScanner::ProgressFn progress = [this](const QString &stage, int percent) {
        QMetaObject::invokeMethod(this, [this, stage, percent]() {
            m_status->setText(stage);
            m_progress->setValue(percent);
        }, Qt::QueuedConnection);
    };

    m_scanWatcher->setFuture(QtConcurrent::run([app, progress]() {
        return LeftoverScanner::scan(app, progress);
    }));
}

void LeftoverDialog::buildUi()
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(10);

    m_headline = new QLabel(this);
    QFont big = m_headline->font();
    big.setPointSize(big.pointSize() + 2);
    big.setBold(true);
    m_headline->setFont(big);
    m_headline->setText(QStringLiteral("%1 的残留项").arg(m_app.name));

    m_status = new QLabel(QStringLiteral("正在准备扫描…"), this);
    m_status->setStyleSheet(QStringLiteral("color:#6b7280;"));

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(4);
    m_tree->setHeaderLabels({QStringLiteral("项目"), QStringLiteral("位置"),
                             QStringLiteral("大小"), QStringLiteral("说明")});
    m_tree->setRootIsDecorated(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(true);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(3, QHeaderView::Interactive);
    m_tree->setColumnWidth(0, 260);
    m_tree->setColumnWidth(3, 240);

    connect(m_tree, &QTreeWidget::itemChanged, this, &LeftoverDialog::onTreeItemChanged);

    m_summary = new QLabel(this);
    m_summary->setStyleSheet(QStringLiteral("color:#374151;"));

    QHBoxLayout *buttons = new QHBoxLayout;
    m_selectAllBtn  = new QPushButton(QStringLiteral("全选"), this);
    m_selectNoneBtn = new QPushButton(QStringLiteral("全不选"), this);
    m_killBtn       = new QPushButton(QStringLiteral("结束相关进程"), this);
    m_removeBtn     = new QPushButton(QStringLiteral("删除选中项"), this);
    m_removeBtn->setDefault(true);

    buttons->addWidget(m_selectAllBtn);
    buttons->addWidget(m_selectNoneBtn);
    buttons->addWidget(m_killBtn);
    buttons->addStretch(1);
    buttons->addWidget(m_removeBtn);

    QDialogButtonBox *box = new QDialogButtonBox(this);
    box->addButton(QStringLiteral("关闭"), QDialogButtonBox::AcceptRole);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    buttons->addWidget(box);

    layout->addWidget(m_headline);
    layout->addWidget(m_status);
    layout->addWidget(m_progress);
    layout->addWidget(m_tree, 1);
    layout->addWidget(m_summary);
    layout->addLayout(buttons);

    connect(m_selectAllBtn,  &QPushButton::clicked, this, [this]() { selectAll(true); });
    connect(m_selectNoneBtn, &QPushButton::clicked, this, [this]() { selectAll(false); });
    connect(m_removeBtn,     &QPushButton::clicked, this, &LeftoverDialog::removeSelected);
    connect(m_killBtn,       &QPushButton::clicked, this, &LeftoverDialog::killRelatedProcesses);

    m_removeBtn->setEnabled(false);
    m_selectAllBtn->setEnabled(false);
    m_selectNoneBtn->setEnabled(false);
}

void LeftoverDialog::onScanFinished()
{
    m_items = m_scanWatcher->result();
    m_progress->setValue(100);
    m_progress->setVisible(false);
    m_status->setText(m_items.isEmpty()
                          ? QStringLiteral("没有发现残留，系统很干净。")
                          : QStringLiteral("扫描完成，共发现 %1 项残留。").arg(m_items.size()));

    populateTree();
    m_removeBtn->setEnabled(!m_items.isEmpty());
    m_selectAllBtn->setEnabled(!m_items.isEmpty());
    m_selectNoneBtn->setEnabled(!m_items.isEmpty());
    updateSummary();
}

void LeftoverDialog::populateTree()
{
    m_tree->clear();

    const QStringList order = {
        QStringLiteral("注册表项"), QStringLiteral("安装目录"), QStringLiteral("用户数据 / 配置"),
        QStringLiteral("其它目录"), QStringLiteral("快捷方式"), QStringLiteral("开机启动"),
        QStringLiteral("服务"), QStringLiteral("计划任务")
    };

    QHash<QString, QTreeWidgetItem *> categories;

    // 用两层循环保证类别顺序固定
    for (const QString &category : order) {
        for (int i = 0; i < m_items.size(); ++i) {
            const LeftoverItem &item = m_items.at(i);
            if (item.category != category) continue;

            QTreeWidgetItem *cat = categories.value(category, nullptr);
            if (!cat) {
                int countInCategory = 0;
                for (const LeftoverItem &x : m_items) {
                    if (x.category == category) ++countInCategory;
                }

                cat = new QTreeWidgetItem(m_tree);
                cat->setText(0, QStringLiteral("%1（%2）").arg(category).arg(countInCategory));
                QFont bold = cat->font(0);
                bold.setBold(true);
                cat->setFont(0, bold);
                cat->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
                cat->setCheckState(0, Qt::Checked);
                categories.insert(category, cat);
            }

            QTreeWidgetItem *leaf = new QTreeWidgetItem(cat);
            leaf->setText(0, item.title);
            leaf->setText(1, item.detail);
            leaf->setText(2, item.sizeBytes > 0 ? formatBytes(item.sizeBytes)
                                                : QStringLiteral("—"));
            leaf->setText(3, item.note);
            leaf->setToolTip(1, item.detail);
            leaf->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
            leaf->setCheckState(0, item.checked ? Qt::Checked : Qt::Unchecked);
            leaf->setData(0, Qt::UserRole, i);   // 记住对应 m_items 的下标
        }
    }

    m_tree->expandAll();
}

void LeftoverDialog::onTreeItemChanged(QTreeWidgetItem *item, int column)
{
    if (!item || column != 0) return;

    // 程序里改状态时用信号阻塞避免递归
    const QSignalBlocker blocker(m_tree);

    if (!item->parent()) {
        // 类别节点：一次改所有子项
        for (int i = 0; i < item->childCount(); ++i)
            item->child(i)->setCheckState(0, item->checkState(0));
    } else {
        // 叶子节点：回头修正父节点的三态
        QTreeWidgetItem *parent = item->parent();
        int checked = 0;
        for (int i = 0; i < parent->childCount(); ++i) {
            if (parent->child(i)->checkState(0) == Qt::Checked) ++checked;
        }
        if (checked == 0)
            parent->setCheckState(0, Qt::Unchecked);
        else if (checked == parent->childCount())
            parent->setCheckState(0, Qt::Checked);
        else
            parent->setCheckState(0, Qt::PartiallyChecked);
    }

    updateSummary();
}

void LeftoverDialog::selectAll(bool checked)
{
    const QSignalBlocker blocker(m_tree);
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *cat = m_tree->topLevelItem(i);
        cat->setCheckState(0, checked ? Qt::Checked : Qt::Unchecked);
        for (int j = 0; j < cat->childCount(); ++j)
            cat->child(j)->setCheckState(0, checked ? Qt::Checked : Qt::Unchecked);
    }
    updateSummary();
}

QVector<LeftoverItem> LeftoverDialog::checkedItems() const
{
    QVector<LeftoverItem> result;

    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *cat = m_tree->topLevelItem(i);
        for (int j = 0; j < cat->childCount(); ++j) {
            QTreeWidgetItem *leaf = cat->child(j);
            if (leaf->checkState(0) != Qt::Checked) continue;
            const int index = leaf->data(0, Qt::UserRole).toInt();
            if (index >= 0 && index < m_items.size()) result << m_items.at(index);
        }
    }
    return result;
}

void LeftoverDialog::updateSummary()
{
    const QVector<LeftoverItem> checked = checkedItems();

    // 同一棵树可能既列出父目录（C:\Program Files\Bandizip）又列出子目录（…\Bandizip），
    // 直接累加会把同一份体积算两遍。做法：先按路径长度从小到大排序（父目录必然在子目录前），
    // 已计入的路径记为"外层路径"，之后凡是落在某个外层路径之下的项都只算一次。
    QVector<const LeftoverItem *> sized;
    for (const LeftoverItem &it : checked) {
        if (it.sizeBytes > 0) sized.append(&it);
    }
    std::sort(sized.begin(), sized.end(), [](const LeftoverItem *a, const LeftoverItem *b) {
        return a->detail.size() < b->detail.size();
    });

    qint64 bytes = 0;
    QStringList countedPaths;
    for (const LeftoverItem *it : sized) {
        bool covered = false;
        for (const QString &outer : countedPaths) {
            if (isPathUnder(it->detail, outer)) { covered = true; break; }
        }
        if (covered) continue;
        bytes += it->sizeBytes;
        countedPaths << it->detail;
    }

    m_summary->setText(QStringLiteral("已选 %1 项，预计释放 %2")
                           .arg(checked.size())
                           .arg(formatBytes(bytes)));
    m_removeBtn->setEnabled(!checked.isEmpty());
}

void LeftoverDialog::killRelatedProcesses()
{
    const QStringList names = LeftoverRemover::relatedProcessNames(m_app);
    if (names.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("没有找到相关进程"),
                                 QStringLiteral("安装目录下没有正在运行的程序。"));
        return;
    }

    const auto ret = QMessageBox::question(
        this, QStringLiteral("结束进程"),
        QStringLiteral("以下进程将被强制结束：\n\n%1\n\n继续？").arg(names.join(QLatin1Char('\n'))),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ret != QMessageBox::Yes) return;

    const int killed = LeftoverRemover::killRelatedProcesses(m_app);
    QMessageBox::information(this, QStringLiteral("已结束进程"),
                             QStringLiteral("共结束了 %1 个进程。").arg(killed));
}

void LeftoverDialog::removeSelected()
{
    const QVector<LeftoverItem> checked = checkedItems();
    if (checked.isEmpty()) return;

    qint64 bytes = 0;
    int adminNeeded = 0;
    for (const LeftoverItem &it : checked) {
        if (it.sizeBytes > 0) bytes += it.sizeBytes;
        if (it.requiresAdmin) ++adminNeeded;
    }

    QString message = QStringLiteral("即将删除 %1 项，预计释放 %2。\n\n"
                                     "· 目录与文件会移入回收站（可恢复）\n"
                                     "· 注册表项会先导出 .reg 备份")
                          .arg(checked.size())
                          .arg(formatBytes(bytes));
    if (adminNeeded > 0)
        message += QStringLiteral("\n· 其中 %1 项（服务/计划任务）需要管理员权限").arg(adminNeeded);
    message += QStringLiteral("\n\n继续删除？");

    if (QMessageBox::question(this, QStringLiteral("确认清理"), message,
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes)
        return;

    const QString backupDir = ForceUninstaller::makeBackupDir();

    m_removeBtn->setEnabled(false);
    m_progress->setVisible(true);
    m_progress->setRange(0, 0);
    m_status->setText(QStringLiteral("正在清理…"));

    m_removeWatcher = new QFutureWatcher<RemovalResult>(this);
    connect(m_removeWatcher, &QFutureWatcher<RemovalResult>::finished,
            this, &LeftoverDialog::onRemovalFinished);
    m_removeWatcher->setFuture(QtConcurrent::run([checked, backupDir]() {
        return LeftoverRemover::remove(checked, backupDir);
    }));
}

void LeftoverDialog::onRemovalFinished()
{
    const RemovalResult result = m_removeWatcher->result();

    m_progress->setVisible(false);
    m_progress->setRange(0, 100);
    m_status->setText(QStringLiteral("清理完成：成功 %1 项，失败 %2 项")
                          .arg(result.successCount)
                          .arg(result.failedCount));

    QString detail = QStringLiteral("成功清理 %1 项，释放磁盘空间 %2。")
                         .arg(result.successCount)
                         .arg(formatBytes(result.reclaimedBytes));

    if (!result.backups.isEmpty())
        detail += QStringLiteral("\n\n已备份 %1 个注册表项到：\n%2")
                      .arg(result.backups.size())
                      .arg(result.backups.first().section(QLatin1Char('/'), 0, -2));

    if (!result.errors.isEmpty()) {
        detail += QStringLiteral("\n\n失败原因（前 8 条）：\n%1")
                      .arg(QStringList(result.errors.mid(0, 8)).join(QLatin1Char('\n')));
        if (result.failedCount > 0 && !isRunningAsAdmin())
            detail += QStringLiteral("\n\n提示：服务/计划任务类残留需要以管理员身份运行本工具。");
    }

    QMessageBox::information(this, QStringLiteral("清理结果"), detail);

    // 清理后建议重新扫描一次，让用户看到剩余项
    m_items.clear();
    m_tree->clear();
    m_status->setText(QStringLiteral("如需复查，请重新打开本窗口。"));
    updateSummary();
}

} // namespace un
