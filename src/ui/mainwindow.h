#pragma once

// ============================================================================
//  mainwindow.h —— 主窗口
//  左侧程序列表（可搜索/排序/多选），右侧详情面板，底部状态栏。
//  卸载入口三个：普通卸载、静默卸载、强制卸载（带残留清理）。
// ============================================================================

#include "core/appinfo.h"
#include "ui/settingsdialog.h"

#include <QMainWindow>
#include <QVector>

QT_BEGIN_NAMESPACE
class QLineEdit;
class QComboBox;
class QTableView;
class QLabel;
class QProgressBar;
class QPushButton;
class QPlainTextEdit;
class QAction;
class QCloseEvent;
QT_END_NAMESPACE

namespace un {

class AppTableModel;
class AppFilterProxy;
class MonitorDialog;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void refresh();
    void onSearchChanged(const QString &text);
    void onPublisherFilterChanged();
    void onSelectionChanged();
    void showContextMenu(const QPoint &pos);

    void uninstallSelected();          // 标准卸载
    void silentUninstallSelected();    // 静默卸载
    void forceUninstallSelected();     // 强制卸载 + 残留清理

    void openInstallFolder();
    void openRegistryEditor();
    void copyDetails();
    void exportCsv();
    void exportHtml();
    void editSettings();
    void showAbout();
    void relaunchAsAdmin();

    void openBatchWizard();      // 批量卸载向导
    void openMonitor();          // 安装监控窗口

private:
    void buildUi();
    void buildActions();
    void updateDetailPanel();
    void updateStatusBar();
    void runUninstall(const QVector<InstalledApp> &apps, bool silent);
    void startLeftoverScan(const InstalledApp &app);
    void scanLeftoverOfCurrent();
    bool confirmUninstall(const QVector<InstalledApp> &apps, bool silent);
    QVector<InstalledApp> selectedApps() const;
    InstalledApp appAtProxyRow(int proxyRow) const;

    AppTableModel  *m_model = nullptr;
    AppFilterProxy *m_proxy = nullptr;
    QTableView     *m_table = nullptr;
    QLineEdit      *m_search = nullptr;
    QComboBox      *m_publisherFilter = nullptr;

    // 详情面板
    QLabel        *m_detailIcon = nullptr;
    QLabel        *m_detailName = nullptr;
    QLabel        *m_detailPublisher = nullptr;
    QLabel        *m_valueVersion = nullptr;
    QLabel        *m_valueDate = nullptr;
    QLabel        *m_valueSize = nullptr;
    QLabel        *m_valueType = nullptr;
    QLabel        *m_valueLocation = nullptr;
    QLabel        *m_valueRegistry = nullptr;
    QPlainTextEdit*m_valueCommand = nullptr;
    QPushButton   *m_btnUninstall = nullptr;
    QPushButton   *m_btnSilent = nullptr;
    QPushButton   *m_btnForce = nullptr;

    QLabel       *m_statusCount = nullptr;
    QProgressBar *m_busyBar = nullptr;

    QAction *m_actUninstall = nullptr;
    QAction *m_actSilent = nullptr;
    QAction *m_actForce = nullptr;
    QAction *m_actOpenFolder = nullptr;
    QAction *m_actScanLeftover = nullptr;
    QAction *m_actCopy = nullptr;
    QAction *m_actBatch = nullptr;
    QAction *m_actMonitor = nullptr;

    AppSettings m_settings;
    bool m_admin = false;
    MonitorDialog *m_monitorDialog = nullptr;   // 非模态，懒创建
};

} // namespace un
