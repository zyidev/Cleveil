// ============================================================================
//  monitordialog.cpp —— 安装监控窗口
// ----------------------------------------------------------------------------
//  事件行由 InstallMonitor 的 Queued 信号推进来，这里只做显示与操作。
//  新装条目用浅绿色底高亮，避免和普通行混在一起。
// ============================================================================

#include "ui/monitordialog.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace un {

namespace {
constexpr int kColTime   = 0;
constexpr int kColKind   = 1;
constexpr int kColName   = 2;
constexpr int kColDetail = 3;
// 事件表最多保留这么多行，超出丢弃最旧的，避免长时间挂机吃内存
constexpr int kMaxRows = 300;
}

MonitorDialog::MonitorDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("安装监控 — Cleveil"));
    setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));
    setMinimumSize(680, 460);

    m_monitor = new InstallMonitor(this);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    // ---- 顶部状态行 ----
    QHBoxLayout *topRow = new QHBoxLayout;
    m_stateLabel = new QLabel(QStringLiteral("未启动"), this);
    m_stateLabel->setStyleSheet(QStringLiteral("color:#4a5b8c;font-weight:600;"));
    topRow->addWidget(m_stateLabel, 1);

    m_toggleBtn = new QPushButton(QStringLiteral("开始监控"), this);
    m_toggleBtn->setObjectName(QStringLiteral("primaryButton"));
    topRow->addWidget(m_toggleBtn);

    QPushButton *clearBtn = new QPushButton(QStringLiteral("清空"), this);
    topRow->addWidget(clearBtn);

    QPushButton *driverBtn = new QPushButton(QStringLiteral("驱动模式说明…"), this);
    topRow->addWidget(driverBtn);
    layout->addLayout(topRow);

    // ---- 事件表 ----
    m_table = new QTableWidget(this);
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels({QStringLiteral("时间"), QStringLiteral("类型"),
                                        QStringLiteral("程序 / 进程"), QStringLiteral("说明")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setShowGrid(false);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(26);
    m_table->horizontalHeader()->setSectionResizeMode(kColDetail, QHeaderView::Stretch);
    m_table->horizontalHeader()->resizeSection(kColTime, 150);
    m_table->horizontalHeader()->resizeSection(kColKind, 84);
    m_table->horizontalHeader()->resizeSection(kColName, 190);
    layout->addWidget(m_table, 1);

    // ---- 底部 ----
    QHBoxLayout *bottomRow = new QHBoxLayout;
    m_counterLabel = new QLabel(this);
    m_counterLabel->setStyleSheet(QStringLiteral("color:#6b7280;"));
    bottomRow->addWidget(m_counterLabel, 1);

    m_scanBtn = new QPushButton(QStringLiteral("扫描残留…"), this);
    m_uninstallBtn = new QPushButton(QStringLiteral("卸载选中"), this);
    m_uninstallBtn->setObjectName(QStringLiteral("primaryButton"));
    m_scanBtn->setEnabled(false);
    m_uninstallBtn->setEnabled(false);
    bottomRow->addWidget(m_scanBtn);
    bottomRow->addWidget(m_uninstallBtn);
    layout->addLayout(bottomRow);

    QLabel *hint = new QLabel(
        QStringLiteral("提示：监控开启后，新安装的程序会以绿色行出现，可直接选中卸载；"
                       "“安装活动”行来自 msiexec / setup.exe 等安装器进程。"), this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:#8a90a0;"));
    layout->addWidget(hint);

    // ---- 信号连接 ----
    connect(m_monitor, &InstallMonitor::started,  this, &MonitorDialog::onStarted);
    connect(m_monitor, &InstallMonitor::stopped,  this, &MonitorDialog::onStopped);
    connect(m_monitor, &InstallMonitor::eventRecorded, this, &MonitorDialog::onEvent);
    connect(m_monitor, &InstallMonitor::scanFailed, this,
            [this](const QString &reason) { m_stateLabel->setText(QStringLiteral("扫描异常：%1").arg(reason)); });
    connect(m_toggleBtn, &QPushButton::clicked, this, &MonitorDialog::toggleMonitoring);
    connect(clearBtn,    &QPushButton::clicked, this, &MonitorDialog::clearEvents);
    connect(m_uninstallBtn, &QPushButton::clicked, this, &MonitorDialog::uninstallSelectedRow);
    connect(m_scanBtn, &QPushButton::clicked, this, [this]() {
        const InstalledApp app = rowApp(m_table->currentRow());
        if (!app.name.isEmpty()) emit scanLeftoverRequested(app);
    });
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this]() {
        const InstalledApp app = rowApp(m_table->currentRow());
        const bool usable = app.canUninstall();
        m_uninstallBtn->setEnabled(usable);
        m_scanBtn->setEnabled(!app.name.isEmpty());
    });
    connect(driverBtn, &QPushButton::clicked, this, [this]() {
        QMessageBox::information(
            this, QStringLiteral("驱动模式说明"),
            QStringLiteral(
                "<b>当前通道：注册表通知（用户态）</b><br>"
                "Cleveil 对四条 Uninstall 注册表键挂 <code>RegNotifyChangeKeyValue</code>，"
                "键被写入时由内核主动唤醒监控线程，因此不需要轮询、也几乎不占 CPU。"
                "这与驱动级方案在“能否第一时间知道安装发生”上差距很小。<br><br>"
                "<b>真正的内核驱动需要什么</b><br>"
                "1. 用 WDK 编译一个注册表回调 / 进程回调驱动（<code>CmRegisterCallbackEx</code>、"
                "<code>PsSetCreateProcessNotifyRoutineEx</code>）；<br>"
                "2. 驱动必须有有效签名：生产环境要 EV 证书，测试环境要开启"
                "<code>bcdedit /set testsigning on</code> 并重启；<br>"
                "3. 驱动崩溃等于系统蓝屏，必须先在虚拟机里长期验证。<br><br>"
                "Cleveil 已预留驱动接口：一旦系统里出现设备 <code>\\\\.\\CleveilMon</code>，"
                "监控会自动切换为“内核驱动通道”，不需要改代码。"));
    });

    // 窗口关闭时停掉后台监控线程，避免隐藏后还在周期性扫描注册表
    connect(this, &QDialog::finished, this, &MonitorDialog::stopMonitoring);

    // 默认不自动开始监听（用户点“开始监控”或从主界面打开时才启动）
    clearEvents();
}

void MonitorDialog::startMonitoring()
{
    if (m_monitor->isRunning()) return;
    m_monitor->start();
}

void MonitorDialog::stopMonitoring()
{
    if (m_monitor->isRunning()) m_monitor->stop();
}

void MonitorDialog::toggleMonitoring()
{
    if (m_monitor->isRunning()) {
        m_monitor->stop();
    } else {
        m_monitor->start();
    }
}

void MonitorDialog::onStarted(MonitorChannel channel)
{
    m_toggleBtn->setText(QStringLiteral("停止监控"));
    m_stateLabel->setText(QStringLiteral("监听中 · %1").arg(monitorChannelText(channel)));
}

void MonitorDialog::onStopped()
{
    m_toggleBtn->setText(QStringLiteral("开始监控"));
    m_stateLabel->setText(QStringLiteral("已停止"));
}

void MonitorDialog::clearEvents()
{
    m_table->setRowCount(0);
    m_installCount = m_removeCount = m_activityCount = 0;
    m_counterLabel->setText(QStringLiteral("安装 0 · 卸载 0 · 安装活动 0"));
}

void MonitorDialog::onEvent(const MonitorEvent &ev)
{
    // 超出上限时丢掉最旧的一行
    if (m_table->rowCount() >= kMaxRows) m_table->removeRow(0);

    const int row = m_table->rowCount();
    m_table->insertRow(row);

    auto makeItem = [](const QString &text) {
        QTableWidgetItem *item = new QTableWidgetItem(text);
        item->setToolTip(text);
        return item;
    };

    m_table->setItem(row, kColTime,   makeItem(ev.time.toString(QStringLiteral("HH:mm:ss"))));
    m_table->setItem(row, kColKind,   makeItem(ev.kind));
    m_table->setItem(row, kColName,   makeItem(ev.name));
    m_table->setItem(row, kColDetail, makeItem(ev.detail));

    // 记录完整数据，供"卸载/扫描残留"还原
    if (!ev.app.name.isEmpty()) {
        m_table->item(row, kColName)->setData(Qt::UserRole, QVariant::fromValue(ev.app.id));
        m_apps.insert(ev.app.id, ev.app);
    }
    m_table->item(row, kColName)->setData(Qt::UserRole + 1, ev.kind);

    if (ev.kind == QStringLiteral("安装完成")) {
        ++m_installCount;
        const QColor green(232, 247, 235);
        for (int c = 0; c < 4; ++c) m_table->item(row, c)->setBackground(green);
    } else if (ev.kind == QStringLiteral("已卸载")) {
        ++m_removeCount;
    } else {
        ++m_activityCount;
    }

    m_counterLabel->setText(QStringLiteral("安装 %1 · 卸载 %2 · 安装活动 %3")
                                .arg(m_installCount).arg(m_removeCount).arg(m_activityCount));

    m_table->scrollToBottom();
}

InstalledApp MonitorDialog::rowApp(int row) const
{
    InstalledApp app;
    if (row < 0 || !m_table->item(row, kColName)) return app;

    const QString id = m_table->item(row, kColName)->data(Qt::UserRole).toString();
    if (id.isEmpty()) return app;

    // 事件里的记录可能比当前系统状态"旧"，但用于卸载足够：卸载命令就来自这条记录
    app = m_apps.value(id);
    return app;
}

void MonitorDialog::uninstallSelectedRow()
{
    const InstalledApp app = rowApp(m_table->currentRow());
    if (app.name.isEmpty()) return;
    emit uninstallRequested(app);
}

} // namespace un
