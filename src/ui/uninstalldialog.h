#pragma once

// ============================================================================
//  uninstalldialog.h —— 卸载执行对话框（单条或批量）
//  一次把一个卸载任务列表按顺序跑完，实时显示进度和日志。
//  判断成功的标准不是"退出码=0"，而是安装记录真的消失
//  （MSI 产品码、商店包、驱动 INF、服务或注册表登记项）。
// ============================================================================

#include "core/appinfo.h"
#include "core/uninstaller.h"

#include <QDialog>
#include <QStringList>
#include <QVector>

QT_BEGIN_NAMESPACE
class QLabel;
class QProgressBar;
class QPlainTextEdit;
class QPushButton;
class QDialogButtonBox;
QT_END_NAMESPACE

namespace un {

struct UninstallTask {
    InstalledApp  app;
    UninstallPlan plan;
    bool          elevate = false;   // 是否走 UAC 提权通道
};

class UninstallDialog : public QDialog {
    Q_OBJECT

public:
    explicit UninstallDialog(const QVector<UninstallTask> &tasks, QWidget *parent = nullptr);

    // 真正卸载成功的程序名（后面据此提示"扫描残留"）
    QStringList succeededApps() const { return m_succeededApps; }
    bool allFinished() const { return m_index >= m_tasks.size(); }

private slots:
    void startNext();
    void onRunnerFinished(bool started, quint32 exitCode, const QString &error);
    void onCancelClicked();

private:
    void appendLog(const QString &text);

    QVector<UninstallTask> m_tasks;
    int        m_index = 0;
    int        m_succeededCount = 0;
    QStringList m_succeededApps;
    bool       m_cancelled = false;

    UninstallRunner   *m_runner = nullptr;
    QLabel            *m_currentLabel = nullptr;
    QLabel            *m_hintLabel = nullptr;
    QProgressBar      *m_progress = nullptr;
    QPlainTextEdit    *m_log = nullptr;
    QPushButton       *m_cancelButton = nullptr;
    QDialogButtonBox  *m_buttons = nullptr;
};

} // namespace un
