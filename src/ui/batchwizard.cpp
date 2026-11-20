// ============================================================================
//  batchwizard.cpp —— 批量卸载向导
// ----------------------------------------------------------------------------
//  实现要点：
//   · 第 1 步用 QTableWidget 自带的勾选框（CheckStateRole），不需要自定义 delegate；
//   · 第 3 步进入页面即开始执行，按"一项跑完再跑下一项"的顺序，避免同时弹多个 UAC；
//   · 每项卸载后用注册表键是否还在来复核，比只看退出码更可靠（很多卸载器返回 0 却没删干净）。
// ============================================================================

#include "ui/batchwizard.h"

#include "core/leftovers.h"
#include "core/remover.h"
#include "core/uninstaller.h"
#include "core/winutil.h"

#include <QApplication>
#include <QCheckBox>
#include <QDateTime>
#include <QDir>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace un {

namespace {
constexpr int kColCheck   = 0;   // 勾选 + 名称
constexpr int kColPublisher = 1;
constexpr int kColVersion = 2;
constexpr int kColSize    = 3;
constexpr int kColType    = 4;
}

BatchWizard::BatchWizard(const QVector<InstalledApp> &allApps,
                         const QVector<InstalledApp> &preselected,
                         QWidget *parent)
    : QWizard(parent)
    , m_allApps(allApps)
    , m_preselected(preselected)
{
    setWindowTitle(QStringLiteral("批量卸载向导 — Cleveil"));
    setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));
    setWizardStyle(QWizard::ModernStyle);
    setOption(QWizard::NoBackButtonOnStartPage, true);
    setMinimumSize(760, 560);
    resize(860, 600);

    // windeployqt 部署时未带 Qt 翻译文件，标准按钮默认是英文，这里显式改成中文
    setButtonText(QWizard::NextButton,   QStringLiteral("下一步"));
    setButtonText(QWizard::BackButton,   QStringLiteral("上一步"));
    setButtonText(QWizard::CancelButton, QStringLiteral("取消"));
    setButtonText(QWizard::FinishButton, QStringLiteral("完成"));

    m_runner = new UninstallRunner(this);
    connect(m_runner, &UninstallRunner::finished, this, &BatchWizard::onRunnerFinished);

    buildSelectPage();
    buildOptionPage();
    buildProgressPage();
}

// ===========================================================================
//  第 1 步：选择程序
// ===========================================================================
void BatchWizard::buildSelectPage()
{
    QWizardPage *page = new QWizardPage;
    page->setTitle(QStringLiteral("第 1 步：选择要卸载的程序"));
    page->setSubTitle(QStringLiteral("勾选要批量卸载的程序。主界面上已选中的项已自动勾好。"));

    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setSpacing(8);

    QHBoxLayout *tools = new QHBoxLayout;
    QPushButton *btnAll = new QPushButton(QStringLiteral("全选"), page);
    QPushButton *btnNone = new QPushButton(QStringLiteral("全不选"), page);
    QPushButton *btnInvert = new QPushButton(QStringLiteral("反选"), page);
    tools->addWidget(btnAll);
    tools->addWidget(btnNone);
    tools->addWidget(btnInvert);
    tools->addStretch(1);
    QLabel *hint = new QLabel(QStringLiteral("共 %1 个可选程序").arg(m_allApps.size()), page);
    hint->setStyleSheet(QStringLiteral("color:#6b7280;"));
    tools->addWidget(hint);
    layout->addLayout(tools);

    m_table = new QTableWidget(page);
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels({QStringLiteral("程序"), QStringLiteral("发布者"),
                                        QStringLiteral("版本"), QStringLiteral("大小"),
                                        QStringLiteral("类型")});
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(26);
    m_table->setAlternatingRowColors(true);
    m_table->setShowGrid(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->horizontalHeader()->setSectionResizeMode(kColCheck, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(kColPublisher, QHeaderView::Interactive);
    m_table->horizontalHeader()->resizeSection(kColPublisher, 190);
    m_table->horizontalHeader()->setSectionResizeMode(kColVersion, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(kColSize, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(kColType, QHeaderView::ResizeToContents);

    // 记录主界面已选中的唯一键，用于预勾选
    QSet<QString> preIds;
    for (const InstalledApp &a : m_preselected) preIds.insert(a.id);

    m_table->setRowCount(m_allApps.size());
    for (int row = 0; row < m_allApps.size(); ++row) {
        const InstalledApp &app = m_allApps.at(row);

        QTableWidgetItem *nameItem = new QTableWidgetItem(app.name);
        nameItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        nameItem->setCheckState(preIds.contains(app.id) ? Qt::Checked : Qt::Unchecked);
        nameItem->setData(Qt::UserRole, app.id);
        nameItem->setToolTip(app.installLocation);
        m_table->setItem(row, kColCheck, nameItem);

        m_table->setItem(row, kColPublisher, new QTableWidgetItem(app.publisher));
        m_table->setItem(row, kColVersion, new QTableWidgetItem(app.version));
        m_table->setItem(row, kColSize, new QTableWidgetItem(
            app.sizeBytes > 0 ? formatBytes(app.sizeBytes) : QStringLiteral("—")));
        m_table->setItem(row, kColType, new QTableWidgetItem(packageTypeText(app.type)));
    }

    layout->addWidget(m_table, 1);

    connect(btnAll,   &QPushButton::clicked, this, [this]() { selectAll(true); });
    connect(btnNone,  &QPushButton::clicked, this, [this]() { selectAll(false); });
    connect(btnInvert,&QPushButton::clicked, this, &BatchWizard::invertSelection);

    addPage(page);
}

void BatchWizard::selectAll(bool on)
{
    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (QTableWidgetItem *item = m_table->item(row, kColCheck))
            item->setCheckState(on ? Qt::Checked : Qt::Unchecked);
    }
}

void BatchWizard::invertSelection()
{
    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (QTableWidgetItem *item = m_table->item(row, kColCheck))
            item->setCheckState(item->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
    }
}

// ===========================================================================
//  第 2 步：卸载选项
// ===========================================================================
void BatchWizard::buildOptionPage()
{
    QWizardPage *page = new QWizardPage;
    page->setTitle(QStringLiteral("第 2 步：卸载选项"));
    page->setSubTitle(QStringLiteral("决定卸载方式，以及卸载后要不要顺手清理残留。"));

    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setSpacing(12);

    m_chkSilent = new QCheckBox(QStringLiteral("优先使用静默卸载（程序提供 QuietUninstallString 时）"), page);
    m_chkSilent->setChecked(true);
    layout->addWidget(m_chkSilent);

    m_chkConfirm = new QCheckBox(QStringLiteral("每个程序卸载前都弹一次确认"), page);
    m_chkConfirm->setChecked(false);
    layout->addWidget(m_chkConfirm);

    m_chkLeftover = new QCheckBox(QStringLiteral("卸载成功后自动扫描并清理残留（注册表、目录、快捷方式、服务、计划任务）"), page);
    m_chkLeftover->setChecked(true);
    layout->addWidget(m_chkLeftover);

    QLabel *note = new QLabel(
        QStringLiteral("<b>说明</b><br>"
                       "· 静默卸载不会显示安装器界面，部分程序不支持，会自动退回常规卸载；<br>"
                       "· 自动清理只删除路径已经对上该程序的项（安装目录、启动项、服务、计划任务）；只是同名或同厂商的项会留下来；<br>"
                       "· 注册表删除前会自动导出 .reg 备份，文件删除走回收站。"), page);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color:#6b7280;"));
    layout->addWidget(note);
    layout->addStretch(1);

    addPage(page);
}

// ===========================================================================
//  第 3 步：执行
// ===========================================================================
void BatchWizard::buildProgressPage()
{
    QWizardPage *page = new QWizardPage;
    page->setTitle(QStringLiteral("第 3 步：执行卸载"));
    page->setSubTitle(QStringLiteral("请保持窗口打开。卸载器可能弹自己的窗口，按提示操作即可。"));

    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setSpacing(10);

    m_stepLabel = new QLabel(QStringLiteral("准备中…"), page);
    m_stepLabel->setStyleSheet(QStringLiteral("color:#3f4653;font-weight:600;"));
    layout->addWidget(m_stepLabel);

    m_progress = new QProgressBar(page);
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    m_progress->setTextVisible(true);
    layout->addWidget(m_progress);

    m_log = new QPlainTextEdit(page);
    m_log->setReadOnly(true);
    m_log->setStyleSheet(QStringLiteral(
        "QPlainTextEdit{background:#ffffff;border:1px solid #e3e6ee;border-radius:6px;"
        "font-family:Consolas,'Microsoft YaHei UI',monospace;font-size:12px;}"));
    layout->addWidget(m_log, 1);

    m_summaryLabel = new QLabel(QString(), page);
    m_summaryLabel->setWordWrap(true);
    m_summaryLabel->setStyleSheet(QStringLiteral("color:#4a5b8c;font-weight:600;"));
    layout->addWidget(m_summaryLabel);

    addPage(page);
}

void BatchWizard::initializePage(int id)
{
    QWizard::initializePage(id);

    if (id == PageProgress) {
        // 进入执行页：把按钮改成"停止/完成"，并开始跑队列
        button(QWizard::BackButton)->setEnabled(false);
        button(QWizard::FinishButton)->setEnabled(false);
        if (QPushButton *cancel = qobject_cast<QPushButton *>(button(QWizard::CancelButton))) {
            cancel->setText(QStringLiteral("停止"));
            disconnect(cancel, nullptr, this, nullptr);
            connect(cancel, &QPushButton::clicked, this, &BatchWizard::onStopClicked);
        }
        startExecution();
    } else if (id == PageOptions) {
        // 选项要等用户离开本页再读。这里只锁定已勾选的程序。
        m_queue = chosenApps();
    }
}

void BatchWizard::reject()
{
    if (m_running) {
        m_stopRequested = true;
        logLine(QStringLiteral("已请求停止：当前程序结束后不再继续。"));
        return;
    }
    QWizard::reject();
}

QVector<InstalledApp> BatchWizard::chosenApps() const
{
    QVector<InstalledApp> result;
    if (!m_table) return result;

    for (int row = 0; row < m_table->rowCount(); ++row) {
        QTableWidgetItem *item = m_table->item(row, kColCheck);
        if (!item || item->checkState() != Qt::Checked) continue;

        const QString id = item->data(Qt::UserRole).toString();
        for (const InstalledApp &app : m_allApps) {
            if (app.id == id) { result.append(app); break; }
        }
    }
    return result;
}

void BatchWizard::onStopClicked()
{
    if (m_running) {
        m_stopRequested = true;
        logLine(QStringLiteral("已请求停止：当前程序结束后不再继续。"));
    } else {
        reject();
    }
}

// 进入第 2 步前检查至少勾选了一项，否则不让继续（避免跑一个空队列）
bool BatchWizard::validateCurrentPage()
{
    if (currentId() == PageSelect && chosenApps().isEmpty()) {
        QMessageBox::information(this, QStringLiteral("批量卸载"),
                                 QStringLiteral("请至少勾选一个要卸载的程序。"));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
//  执行
// ---------------------------------------------------------------------------
void BatchWizard::startExecution()
{
    if (m_running) return;

    // 在真正开跑时读取选项。initializePage 发生在页面显示之前，读不到用户随后的修改。
    if (m_chkSilent) m_optPreferSilent = m_chkSilent->isChecked();
    if (m_chkConfirm) m_optConfirmEach = m_chkConfirm->isChecked();
    if (m_chkLeftover) m_optCleanLeftover = m_chkLeftover->isChecked();

    if (m_queue.isEmpty()) m_queue = chosenApps();

    m_index = m_ok = m_fail = m_skip = 0;
    m_succeeded.clear();
    m_stopRequested = false;

    m_progress->setRange(0, m_queue.size());
    m_progress->setValue(0);
    logLine(QStringLiteral("[%1] 开始批量卸载，共 %2 个程序。")
                .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")))
                .arg(m_queue.size()));

    m_running = true;
    runNextApp();
}

void BatchWizard::runNextApp()
{
    if (!m_running) return;

    if (m_stopRequested || m_index >= m_queue.size()) {
        finishExecution();
        return;
    }

    const InstalledApp app = m_queue.at(m_index);
    setCurrentStep(QStringLiteral("正在处理 %1 / %2：%3")
                       .arg(m_index + 1).arg(m_queue.size()).arg(app.name));

    // ---- 拼装卸载命令：优先静默，静默不可用则退回常规 ----
    UninstallPlan plan = Uninstaller::buildPlan(app, m_optPreferSilent);
    if (!plan.valid) {
        const UninstallPlan fallback = Uninstaller::buildPlan(app, false);
        if (!fallback.valid) {
            ++m_skip;
            logLine(QStringLiteral("  ✗ %1：注册表里没有可用的卸载命令，已跳过（可改用强制卸载）")
                        .arg(app.name));
            ++m_index;
            m_progress->setValue(m_index);
            QTimer::singleShot(0, this, &BatchWizard::runNextApp);
            return;
        }
        plan = fallback;
        logLine(QStringLiteral("  · %1：没有静默命令，改为常规卸载").arg(app.name));
    } else if (m_optPreferSilent && !app.hasQuietCommand()
               && !plan.program.contains(QStringLiteral("msiexec"), Qt::CaseInsensitive)) {
        logLine(QStringLiteral("  · %1：程序未提供静默参数，已退回常规卸载界面").arg(app.name));
    } else if (m_optPreferSilent && !app.hasQuietCommand()) {
        logLine(QStringLiteral("  · %1：没有 QuietUninstallString，改用 msiexec 的 /qn 静默卸载")
                    .arg(app.name));
    }

    // ---- 逐项确认 ----
    if (m_optConfirmEach) {
        const auto ret = QMessageBox::question(
            this, QStringLiteral("批量卸载"),
            QStringLiteral("继续卸载「%1」吗？\n\n命令：%2").arg(app.name, plan.displayCommand),
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::Yes);
        if (ret == QMessageBox::Cancel) {
            m_stopRequested = true;
            finishExecution();
            return;
        }
        if (ret == QMessageBox::No) {
            ++m_skip;
            logLine(QStringLiteral("  · %1：已按用户要求跳过").arg(app.name));
            ++m_index;
            m_progress->setValue(m_index);
            QTimer::singleShot(0, this, &BatchWizard::runNextApp);
            return;
        }
    }

    logLine(QStringLiteral("  → %1：执行 %2").arg(app.name, plan.displayCommand));

    // 静默 + 需要管理员时主动走 UAC 通道，其余交给安装器自己弹
    // 驱动包的 pnputil 不会自己弹 UAC，需要管理员时直接提权。
    const bool elevate = plan.needsAdmin && (app.type == PackageType::Driver || plan.silent);
    m_runner->run(plan, elevate);
}

void BatchWizard::onRunnerFinished(bool started, quint32 exitCode, const QString &error)
{
    if (m_index >= m_queue.size()) return;      // 防御：理论上不会发生
    const InstalledApp app = m_queue.at(m_index);

    if (!started) {
        ++m_fail;
        logLine(QStringLiteral("  ✗ %1：卸载程序未能启动（%2）").arg(app.name, error));
    } else {
        // 复核：以"产品是否还装着"为准。MSI 优先查产品码状态——升级遗留的
        // 僵尸登记项会让"键还在不在"永远为真，从而把成功误判成失败。
        const bool stillThere = appStillInstalled(app);
        if (!stillThere) {
            ++m_ok;
            m_succeeded << app.name;
            logLine(QStringLiteral("  ✓ %1：卸载完成（退出码 %2）").arg(app.name).arg(exitCode));

            // ---- 卸载成功后自动清理残留 ----
            if (m_optCleanLeftover) {
                logLine(QStringLiteral("      正在扫描 %1 的残留…").arg(app.name));
                setCurrentStep(QStringLiteral("正在扫描 %1 的残留…").arg(app.name));
                // 扫描在界面线程进行。这里不能 processEvents，否则用户点“完成”会把向导拆掉。
                const QVector<LeftoverItem> items = LeftoverScanner::scan(app);

                QVector<LeftoverItem> toRemove;
                for (const LeftoverItem &item : items) {
                    // 只自动清理“路径已经对上”的项。同名/同厂商的项留给用户在残留窗口里确认。
                    if (item.match != LeftoverMatch::PathBound) continue;
                    if ((item.kind == LeftoverKind::Folder || item.kind == LeftoverKind::File)
                        && isDangerousPath(item.detail))
                        continue;
                    toRemove.append(item);
                }

                if (toRemove.isEmpty()) {
                    logLine(QStringLiteral("      没有需要清理的残留项"));
                } else {
                    const QString backupDir = ForceUninstaller::makeBackupDir();
                    const RemovalResult result = LeftoverRemover::remove(toRemove, backupDir);
                    logLine(QStringLiteral("      残留清理：成功 %1 项，失败 %2 项，释放 %3")
                                .arg(result.successCount)
                                .arg(result.failedCount)
                                .arg(formatBytes(result.reclaimedBytes)));
                    for (const QString &e : result.errors) logLine(QStringLiteral("      ! %1").arg(e));
                }
                setCurrentStep(QStringLiteral("正在处理第 %1 / %2 项").arg(m_index + 1).arg(m_queue.size()));
            }
        } else {
            ++m_fail;
            logLine(QStringLiteral("  ✗ %1：卸载器返回 %2，但注册表登记项仍然存在").arg(app.name).arg(exitCode));
        }
    }

    ++m_index;
    m_progress->setValue(m_index);
    QTimer::singleShot(0, this, &BatchWizard::runNextApp);
}

void BatchWizard::finishExecution()
{
    if (!m_running) return;
    m_running = false;

    m_summaryLabel->setText(
        QStringLiteral("完成：成功 %1 · 失败 %2 · 跳过 %3%4")
            .arg(m_ok).arg(m_fail).arg(m_skip)
            .arg(m_stopRequested ? QStringLiteral("（已被你中止）") : QString()));
    logLine(QStringLiteral("[%1] 批量卸载结束：成功 %2，失败 %3，跳过 %4")
                .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")))
                .arg(m_ok).arg(m_fail).arg(m_skip));
    setCurrentStep(QStringLiteral("已结束"));

    button(QWizard::CancelButton)->setEnabled(false);
    button(QWizard::FinishButton)->setEnabled(true);
    button(QWizard::FinishButton)->setVisible(true);
    emit finishedBatch(m_ok, m_fail, m_skip);
}

void BatchWizard::logLine(const QString &text)
{
    m_log->appendPlainText(text);
}

void BatchWizard::setCurrentStep(const QString &text)
{
    if (m_stepLabel) m_stepLabel->setText(text);
}

} // namespace un
