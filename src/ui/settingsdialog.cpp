// ============================================================================
//  settingsdialog.cpp —— 选项持久化与界面
// ============================================================================

#include "ui/settingsdialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
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
}

SettingsDialog::SettingsDialog(const AppSettings &current, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("选项"));
    setModal(true);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 14);
    layout->setSpacing(12);

    m_systemComponents = new QCheckBox(QStringLiteral("在列表中显示系统组件（通常隐藏）"), this);
    m_updates          = new QCheckBox(QStringLiteral("在列表中显示补丁与更新包"), this);
    m_autoScan      = new QCheckBox(QStringLiteral("卸载完成后自动打开残留扫描"), this);
    m_elevateSilent = new QCheckBox(QStringLiteral("静默卸载时以管理员身份执行（会弹 UAC）"), this);
    m_backupRegistry = new QCheckBox(QStringLiteral("删除注册表项前导出 .reg 备份"), this);

    m_systemComponents->setChecked(current.showSystemComponents);
    m_updates->setChecked(current.showUpdates);
    m_autoScan->setChecked(current.autoScanLeftovers);
    m_elevateSilent->setChecked(current.elevateSilent);
    m_backupRegistry->setChecked(current.backupRegistry);

    layout->addWidget(m_systemComponents);
    layout->addWidget(m_updates);
    layout->addSpacing(6);
    layout->addWidget(m_autoScan);
    layout->addWidget(m_elevateSilent);
    layout->addWidget(m_backupRegistry);
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
    return out;
}

} // namespace un
