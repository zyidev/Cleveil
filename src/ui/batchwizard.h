#pragma once

// ============================================================================
//  batchwizard.h —— 批量卸载向导
// ----------------------------------------------------------------------------
//  三步走：
//    第 1 步 选择程序：把主界面（或全部）程序列出来，逐项勾选，支持全选/反选/按发布者筛
//    第 2 步 卸载选项：优先静默、逐个确认、卸载后自动清理残留
//    第 3 步 执行：进度条 + 逐条日志 + 最终汇总，可随时中止（当前一项跑完就停）
//  执行复用 UninstallRunner（QProcess / UAC 两条通道），不重复造轮子。
// ============================================================================

#include "core/appinfo.h"

#include <QWizard>
#include <QVector>

QT_BEGIN_NAMESPACE
class QTableWidget;
class QProgressBar;
class QPlainTextEdit;
class QCheckBox;
class QLabel;
QT_END_NAMESPACE

namespace un {

class UninstallRunner;

class BatchWizard : public QWizard {
    Q_OBJECT

public:
    enum PageId {
        PageSelect   = 0,
        PageOptions  = 1,
        PageProgress = 2
    };

    // allApps：可选的完整清单；preselected：打开时已勾选的项（来自主界面多选）
    BatchWizard(const QVector<InstalledApp> &allApps,
                const QVector<InstalledApp> &preselected,
                QWidget *parent = nullptr);

    QVector<InstalledApp> chosenApps() const;
    QStringList succeededNames() const { return m_succeeded; }

signals:
    void finishedBatch(int success, int failed, int skipped);

protected:
    void initializePage(int id) override;
    bool validateCurrentPage() override;
    void reject() override;

private slots:
    void onRunnerFinished(bool started, quint32 exitCode, const QString &error);
    void selectAll(bool on);
    void invertSelection();
    void onStopClicked();

private:
    void buildSelectPage();
    void buildOptionPage();
    void buildProgressPage();

    void startExecution();
    void runNextApp();
    void finishExecution();
    void logLine(const QString &text);
    void setCurrentStep(const QString &text);

    QVector<InstalledApp> m_allApps;
    QVector<InstalledApp> m_preselected;
    QVector<InstalledApp> m_queue;        // 真正要执行的清单（按勾选顺序）
    QStringList           m_succeeded;

    int m_index = 0;
    int m_ok    = 0;
    int m_fail  = 0;
    int m_skip  = 0;
    bool m_stopRequested = false;
    bool m_running = false;

    // 选项
    bool m_optPreferSilent   = true;
    bool m_optConfirmEach    = false;
    bool m_optCleanLeftover  = true;

    // 控件
    QTableWidget  *m_table = nullptr;
    QCheckBox     *m_chkSilent = nullptr;
    QCheckBox     *m_chkConfirm = nullptr;
    QCheckBox     *m_chkLeftover = nullptr;
    QProgressBar  *m_progress = nullptr;
    QPlainTextEdit*m_log = nullptr;
    QLabel        *m_stepLabel = nullptr;
    QLabel        *m_summaryLabel = nullptr;

    UninstallRunner *m_runner = nullptr;
};

} // namespace un
