// ============================================================================
//  remover.cpp —— 强制卸载的前置动作
// ============================================================================

#include "core/remover.h"
#include "core/leftovers.h"
#include "core/winutil.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>

namespace un {

int ForceUninstaller::terminateProcesses(const InstalledApp &app)
{
    return LeftoverRemover::killRelatedProcesses(app);
}

bool ForceUninstaller::removeRegistration(const InstalledApp &app,
                                           const QString &backupDir,
                                           QString *backupFile,
                                           QString *error)
{
    if (app.registryPath.isEmpty()) {
        if (error) *error = QStringLiteral("该程序没有注册表登记信息，无需删除");
        return false;
    }

    // 调用方要求备份时，导出失败就保留登记项
    if (!backupDir.isEmpty()) {
        QDir().mkpath(backupDir);
        const QString safeName = QStringLiteral("%1_%2.reg")
                                     .arg(app.name, QDateTime::currentDateTime()
                                                        .toString(QStringLiteral("yyyyMMdd-HHmmss")));
        QString cleaned = safeName;
        cleaned.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")),
                        QStringLiteral("_"));
        const QString file = QDir(backupDir).filePath(cleaned);
        QString backupError;
        if (!backupRegistryKey(app.registryPath, file, &backupError)) {
            if (error) {
                *error = backupError.isEmpty()
                             ? QStringLiteral("注册表备份失败，已取消删除")
                             : backupError;
            }
            return false;
        }
        if (backupFile) *backupFile = file;
    }

    return deleteRegistryKey(app.registryPath, app.is64View, error);
}

QString ForceUninstaller::makeBackupDir()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const QString dir = QDir(base.isEmpty() ? QDir::tempPath() : base)
                            .filePath(QStringLiteral("backup/%1").arg(
                                QDateTime::currentDateTime().toString(
                                    QStringLiteral("yyyyMMdd-HHmmss"))));
    QDir().mkpath(dir);
    return dir;
}

} // namespace un
