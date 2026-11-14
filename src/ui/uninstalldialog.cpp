// ============================================================================
//  uninstalldialog.cpp —— 卸载执行 + 结果核验
// ============================================================================

#include "ui/uninstalldialog.h"
#include "core/winutil.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

namespace un {

UninstallDialog::UninstallDialog(const QVector<UninstallTask> &tasks, QWidget *parent)
    : QDialog(parent)
    , m_tasks(tasks)
{
    setWindowTitle(QStringLiteral("卸载"));
    setModal(true);
    resize(620, 420);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(10);

    m_currentLabel = new QLabel(this);
    QFont f = m_currentLabel->font();
    f.setPointSize(f.pointSize() + 1);
    f.setBold(true);
    m_currentLabel->setFont(f);

    m_hintLabel = new QLabel(this);
    m_hintLabel->setWordWrap(true);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, m_tasks.size());
    m_progress->setValue(0);
    m_progress->setTextVisible(true);

    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_log->setMaximumBlockCount(2000);

    m_buttons = new QDialogButtonBox(this);
    m_cancelButton = m_buttons->addButton(QStringLiteral("停止"),
                                          QDialogButtonBox::RejectRole);
    m_buttons->addButton(QStringLiteral("关闭"), QDialogButtonBox::AcceptRole);

    layout->addWidget(m_currentLabel);
    layout->addWidget(m_hintLabel);
    layout->addWidget(m_progress);
    layout->addWidget(m_log, 1);
    layout->addWidget(m_buttons);

    connect(m_buttons, &QDialogButtonBox::rejected, this, &UninstallDialog::onCancelClicked);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);

    m_runner = new UninstallRunner(this);
    connect(m_runner, &UninstallRunner::finished,
            this, &UninstallDialog::onRunnerFinished);

    // 构造完立即开始跑第一个任务
    QMetaObject::invokeMethod(this, &UninstallDialog::startNext, Qt::QueuedConnection);
}

void UninstallDialog::appendLog(const QString &text)
{
    m_log->appendPlainText(QStringLiteral("[%1] %2")
                               .arg(QDateTime::currentDateTime().toString(
                                        QStringLiteral("HH:mm:ss")), text));
}

void UninstallDialog::startNext()
{
    if (m_cancelled || m_index >= m_tasks.size()) {
        m_currentLabel->setText(m_tasks.size() > 1
                                    ? QStringLiteral("批量卸载结束")
                                    : QStringLiteral("卸载结束"));
        m_progress->setValue(m_tasks.size());
        m_cancelButton->setEnabled(false);
        m_hintLabel->setText(QStringLiteral("成功 %1 个，未完成 %2 个。")
                                 .arg(m_succeededCount)
                                 .arg(m_tasks.size() - m_succeededCount));
        return;
    }

    const UninstallTask &task = m_tasks.at(m_index);

    m_currentLabel->setText(QStringLiteral("正在卸载：%1").arg(task.app.name));
    m_progress->setValue(m_index);
    m_progress->setFormat(QStringLiteral("%1 / %2").arg(m_index + 1).arg(m_tasks.size()));

    QString hint = QStringLiteral("命令：%1").arg(task.plan.displayCommand);
    if (task.elevate) hint += QStringLiteral("（以管理员身份执行）");
    if (task.plan.unsupportedSilent)
        hint += QStringLiteral("\n该程序没有提供静默参数，将以普通方式弹出卸载界面。");
    m_hintLabel->setText(hint);

    appendLog(QStringLiteral("开始卸载 %1（%2）").arg(task.app.name, task.app.version));
    appendLog(QStringLiteral("执行命令：%1").arg(task.plan.displayCommand));

    m_runner->run(task.plan, task.elevate);
}

void UninstallDialog::onRunnerFinished(bool started, quint32 exitCode, const QString &error)
{
    if (m_index >= m_tasks.size()) return;

    const UninstallTask &task = m_tasks.at(m_index);

    if (!started) {
        appendLog(QStringLiteral("启动失败：%1").arg(error));
    } else if (exitCode == 0) {
        appendLog(QStringLiteral("卸载程序正常退出（退出码 0）"));
    } else {
        appendLog(QStringLiteral("卸载程序退出码 %1（非 0 不一定代表失败，继续核验）")
                      .arg(exitCode));
    }

    // 关键一步：用"登记项是否消失"来判定真实结果
    const bool stillRegistered = !task.app.registryPath.isEmpty()
                                 && registryKeyExists(task.app.registryPath, task.app.is64View);

    if (!stillRegistered) {
        ++m_succeededCount;
        m_succeededApps << task.app.name;
        appendLog(QStringLiteral("✔ 已确认卸载：登记项已删除"));
    } else {
        appendLog(QStringLiteral("✖ 仍在应用列表中：%1")
                      .arg(task.plan.valid ? QStringLiteral("卸载可能未完成或被取消")
                                           : task.plan.reason));
    }

    ++m_index;
    QMetaObject::invokeMethod(this, &UninstallDialog::startNext, Qt::QueuedConnection);
}

void UninstallDialog::onCancelClicked()
{
    if (m_index < m_tasks.size()) {
        m_cancelled = true;
        m_runner->terminate();
        appendLog(QStringLiteral("已请求停止：剩余任务不再执行"));
        m_index = m_tasks.size();    // 让 startNext 走"结束"分支
        startNext();
    } else {
        reject();
    }
}

} // namespace un
