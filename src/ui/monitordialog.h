#pragma once

// ============================================================================
//  monitordialog.h —— 安装监控窗口（非模态）
//  左边是实时事件流：安装完成 / 已卸载 / 安装活动；
//  新装条目整行高亮，选中后可"立即卸载"或"扫描残留"。
// ============================================================================

#include "core/appinfo.h"
#include "core/installmonitor.h"

#include <QDialog>
#include <QHash>
#include <QVector>

QT_BEGIN_NAMESPACE
class QTableWidget;
class QLabel;
class QPushButton;
QT_END_NAMESPACE

namespace un {

class MonitorDialog : public QDialog {
    Q_OBJECT

public:
    explicit MonitorDialog(QWidget *parent = nullptr);

    void startMonitoring();
    bool isMonitoring() const { return m_monitor->isRunning(); }
    void stopMonitoring();

signals:
    void uninstallRequested(const InstalledApp &app);   // 交给主窗口执行卸载
    void scanLeftoverRequested(const InstalledApp &app);

private slots:
    void onStarted(MonitorChannel channel);
    void onStopped();
    void onEvent(const MonitorEvent &ev);
    void toggleMonitoring();
    void clearEvents();
    void uninstallSelectedRow();

private:
    InstalledApp rowApp(int row) const;

    InstallMonitor *m_monitor = nullptr;
    QTableWidget   *m_table = nullptr;
    QLabel         *m_stateLabel = nullptr;
    QLabel         *m_counterLabel = nullptr;
    QPushButton    *m_toggleBtn = nullptr;
    QPushButton    *m_uninstallBtn = nullptr;
    QPushButton    *m_scanBtn = nullptr;

    int m_installCount = 0;
    int m_removeCount  = 0;
    int m_activityCount = 0;

    // 事件里携带的完整记录（按唯一键存），供"卸载选中/扫描残留"使用
    QHash<QString, InstalledApp> m_apps;
};

} // namespace un
