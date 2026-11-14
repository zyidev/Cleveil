#pragma once

// ============================================================================
//  settingsdialog.h —— 选项设置
//  设置用 QSettings 持久化（注册表 HKCU\Software\Cleveil）。
// ============================================================================

#include <QDialog>

QT_BEGIN_NAMESPACE
class QCheckBox;
QT_END_NAMESPACE

namespace un {

// 全局选项
struct AppSettings {
    bool showSystemComponents = false;   // 显示 SystemComponent=1 的隐藏项
    bool showUpdates          = false;   // 显示补丁/更新包
    bool autoScanLeftovers    = true;    // 卸载完自动打开残留扫描
    bool elevateSilent        = true;    // 静默卸载时以管理员身份执行
    bool backupRegistry       = true;    // 删除注册表前导出 .reg 备份

    static AppSettings load();
    void save() const;
};

class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(const AppSettings &current, QWidget *parent = nullptr);

    AppSettings settings() const;

private:
    QCheckBox *m_systemComponents = nullptr;
    QCheckBox *m_updates = nullptr;
    QCheckBox *m_autoScan = nullptr;
    QCheckBox *m_elevateSilent = nullptr;
    QCheckBox *m_backupRegistry = nullptr;
};

} // namespace un
