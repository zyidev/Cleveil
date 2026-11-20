// ============================================================================
//  settingsdialog.cpp —— 选项持久化与界面
// ============================================================================

#include "ui/settingsdialog.h"
#include "core/inventory/inventory_service.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

namespace un {

AppSettings AppSettings::load()
{
    QSettings s;
    AppSettings out;
    out.showSystemComponents = s.value(QStringLiteral("view/systemComponents"), false).toBool();
    out.showUpdates          = s.value(QStringLiteral("view/updates"), false).toBool();
    out.autoScanLeftovers    = s.value(QStringLiteral("behavior/autoScan"), true).toBool();
    out.elevateSilent        = s.value(QStringLiteral("behavior/elevateSilent"), true).toBool();
    out.backupRegistry       = s.value(QStringLiteral("behavior/backupRegistry"), true).toBool();
    out.scanExtraSources     = s.value(QStringLiteral("inventory/extraSources"), true).toBool();
    out.showHeuristic        = s.value(QStringLiteral("inventory/showHeuristic"), false).toBool();
    out.scanShortcuts        = s.value(QStringLiteral("inventory/scanShortcuts"), false).toBool();
    out.scanExeVersion       = s.value(QStringLiteral("inventory/scanExeVersion"), false).toBool();
    out.scanSnapshotDiff     = s.value(QStringLiteral("inventory/scanSnapshotDiff"), false).toBool();
    return out;
}

void AppSettings::save() const
{
    QSettings s;
    s.setValue(QStringLiteral("view/systemComponents"), showSystemComponents);
    s.setValue(QStringLiteral("view/updates"), showUpdates);
    s.setValue(QStringLiteral("behavior/autoScan"), autoScanLeftovers);
    s.setValue(QStringLiteral("behavior/elevateSilent"), elevateSilent);
    s.setValue(QStringLiteral("behavior/backupRegistry"), backupRegistry);
    s.setValue(QStringLiteral("inventory/extraSources"), scanExtraSources);
    s.setValue(QStringLiteral("inventory/showHeuristic"), showHeuristic);
    s.setValue(QStringLiteral("inventory/scanShortcuts"), scanShortcuts);
    s.setValue(QStringLiteral("inventory/scanExeVersion"), scanExeVersion);
    s.setValue(QStringLiteral("inventory/scanSnapshotDiff"), scanSnapshotDiff);
}

SettingsDialog::SettingsDialog(const AppSettings &current, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("选项"));
    setModal(true);
    resize(480, 520);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 14);
    layout->setSpacing(10);

    m_systemComponents = new QCheckBox(QStringLiteral("在列表中显示系统组件（通常隐藏）"), this);
    m_updates          = new QCheckBox(QStringLiteral("在列表中显示补丁与更新包"), this);
    m_autoScan         = new QCheckBox(QStringLiteral("卸载完成后自动打开残留扫描"), this);
    m_elevateSilent    = new QCheckBox(QStringLiteral("静默卸载时以管理员身份执行（会弹 UAC）"), this);
    m_backupRegistry   = new QCheckBox(QStringLiteral("删除注册表项前导出 .reg 备份"), this);

    m_scanExtra      = new QCheckBox(QStringLiteral("启用多源枚举（MSI / AppX / 驱动包 / 服务 / 安装器文件）"), this);
    m_showHeuristic  = new QCheckBox(QStringLiteral("在列表中显示启发式识别的绿色软件"), this);
    m_scanShortcuts  = new QCheckBox(QStringLiteral("启发式：解析开始菜单与桌面快捷方式"), this);
    m_scanExeVersion = new QCheckBox(QStringLiteral("启发式：读取 LocalAppData\\Programs 等目录的 EXE 版本信息"), this);
    m_scanSnapshot   = new QCheckBox(QStringLiteral("启发式：与上次目录快照做差分"), this);

    m_systemComponents->setChecked(current.showSystemComponents);
    m_updates->setChecked(current.showUpdates);
    m_autoScan->setChecked(current.autoScanLeftovers);
    m_elevateSilent->setChecked(current.elevateSilent);
    m_backupRegistry->setChecked(current.backupRegistry);
    m_scanExtra->setChecked(current.scanExtraSources);
    m_showHeuristic->setChecked(current.showHeuristic);
    m_scanShortcuts->setChecked(current.scanShortcuts);
    m_scanExeVersion->setChecked(current.scanExeVersion);
    m_scanSnapshot->setChecked(current.scanSnapshotDiff);

    layout->addWidget(m_systemComponents);
    layout->addWidget(m_updates);
    layout->addSpacing(4);
    layout->addWidget(m_autoScan);
    layout->addWidget(m_elevateSilent);
    layout->addWidget(m_backupRegistry);
    layout->addSpacing(8);
    layout->addWidget(m_scanExtra);
    layout->addWidget(m_showHeuristic);
    layout->addWidget(m_scanShortcuts);
    layout->addWidget(m_scanExeVersion);
    layout->addWidget(m_scanSnapshot);

    QPushButton *captureBtn = new QPushButton(QStringLiteral("捕获当前安装目录快照…"), this);
    connect(captureBtn, &QPushButton::clicked, this, [this]() {
        QString error;
        if (!InventoryService::captureDirectorySnapshot(&error)) {
            QMessageBox::warning(this, QStringLiteral("快照"),
                                 error.isEmpty() ? QStringLiteral("写入失败") : error);
            return;
        }
        QMessageBox::information(this, QStringLiteral("快照"),
                                 QStringLiteral("已保存 Program Files / LocalAppData\\Programs 顶层目录基线。\n"
                                                "之后勾选“快照差分”并刷新列表，即可看到新出现的目录。"));
    });
    layout->addWidget(captureBtn);
    layout->addStretch(1);

    QDialogButtonBox *box = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    box->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
    box->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(box);
}

AppSettings SettingsDialog::settings() const
{
    AppSettings out;
    out.showSystemComponents = m_systemComponents->isChecked();
    out.showUpdates          = m_updates->isChecked();
    out.autoScanLeftovers    = m_autoScan->isChecked();
    out.elevateSilent        = m_elevateSilent->isChecked();
    out.backupRegistry       = m_backupRegistry->isChecked();
    out.scanExtraSources     = m_scanExtra->isChecked();
    out.showHeuristic        = m_showHeuristic->isChecked();
    out.scanShortcuts        = m_scanShortcuts->isChecked();
    out.scanExeVersion       = m_scanExeVersion->isChecked();
    out.scanSnapshotDiff     = m_scanSnapshot->isChecked();
    return out;
}

} // namespace un
