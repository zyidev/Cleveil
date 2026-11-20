// ============================================================================
//  main.cpp —— 程序入口
// ============================================================================

#include "core/exporter.h"
#include "core/inventory/inventory_service.h"
#include "core/leftovers.h"
#include "core/uninstaller.h"
#include "core/winutil.h"      // 顺带引入 <windows.h>
#include "ui/mainwindow.h"
#include "ui/settingsdialog.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFont>
#include <QIcon>
#include <QStandardPaths>
#include <QStyleFactory>
#include <QThread>

#include <Ole2.h>
#include <cstdio>

int main(int argc, char *argv[])
{
    // 关掉高 DPI 缩放带来的模糊：Qt6 默认已开启高 DPI 支持，这里只做取整策略
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication app(argc, argv);

    QApplication::setOrganizationName(QStringLiteral("Cleveil"));
    QApplication::setOrganizationDomain(QStringLiteral("cleveil.local"));
    QApplication::setApplicationName(QStringLiteral("Cleveil"));
    QApplication::setApplicationVersion(QStringLiteral("1.2.0"));

    // 应用图标：/icons/app.png 来自 resources/resources.qrc（编译进 exe）
    // 这样即使 exe 被改名、或从别处启动，窗口与任务栏图标依然是 Cleveil 的图
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));

    // Shell API（SHFileOperation / SHGetFileInfo）需要 OLE 初始化
    if (FAILED(OleInitialize(nullptr))) {
        // 初始化失败也继续，只是个别 Shell 功能会退化
    }

    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QFont font = app.font();
    font.setFamily(QStringLiteral("Microsoft YaHei UI"));
    font.setPointSize(9);
    app.setFont(font);

    // 统一的浅色扁平风格
    app.setStyleSheet(QStringLiteral(R"(
        QMainWindow, QDialog { background: #f5f6fa; }
        QTableView {
            background: #ffffff;
            border: 1px solid #e3e6ee;
            border-radius: 8px;
            selection-background-color: #e8eefc;
            selection-color: #1f2a44;
            gridline-color: transparent;
        }
        QTableView::item { padding: 2px 6px; }
        QHeaderView::section {
            background: #ffffff;
            border: none;
            border-bottom: 1px solid #e3e6ee;
            padding: 6px 8px;
            color: #6b7280;
            font-weight: 600;
        }
        QPushButton {
            padding: 6px 14px;
            border: 1px solid #d7dbe4;
            border-radius: 6px;
            background: #ffffff;
            color: #23262e;
        }
        QPushButton:hover { background: #f2f4f9; }
        QPushButton:disabled { color: #a6acb8; background: #f7f8fb; }
        QPushButton#primaryButton {
            background: #4a5b8c;
            color: #ffffff;
            border: 1px solid #4a5b8c;
        }
        QPushButton#primaryButton:hover { background: #3f4f7d; border-color: #3f4f7d; }
        QLineEdit, QComboBox, QSpinBox {
            padding: 5px 8px;
            border: 1px solid #d7dbe4;
            border-radius: 6px;
            background: #ffffff;
        }
        QComboBox::drop-down { border: none; width: 18px; }
        QToolBar { background: #ffffff; border-bottom: 1px solid #e3e6ee; spacing: 4px; padding: 4px; }
        QStatusBar { background: #ffffff; border-top: 1px solid #e3e6ee; }
        QTabWidget::pane { border: 1px solid #e3e6ee; border-radius: 6px; background: #ffffff; }
    )"));

    // ------------------------------------------------------------------
    // 无界面导出：Cleveil.exe --export-html <路径> / --export-csv <路径>
    // 与主窗口同一套多源扫描（读当前设置）。不弹窗口，落盘后退出。
    // 返回码 0 = 成功，2 = 导出失败（错误原因打印到 stderr）。
    // ------------------------------------------------------------------
    const QStringList args = app.arguments();
    QString exportKind;
    QString exportPath;
    for (int i = 1; i < args.size(); ++i) {
        if (args.at(i) == QStringLiteral("--export-html")) {
            exportKind = QStringLiteral("html");
            exportPath = args.value(i + 1);
        } else if (args.at(i) == QStringLiteral("--export-csv")) {
            exportKind = QStringLiteral("csv");
            exportPath = args.value(i + 1);
        }
    }
    auto loadApps = []() {
        const un::AppSettings settings = un::AppSettings::load();
        un::InventoryScanOptions opt;
        opt.arp = true;
        opt.msi = settings.scanExtraSources;
        opt.appx = settings.scanExtraSources;
        opt.driverPackages = settings.scanExtraSources;
        opt.services = settings.scanExtraSources;
        opt.installerFiles = settings.scanExtraSources;
        opt.shortcuts = settings.scanShortcuts;
        opt.exeVersion = settings.scanExeVersion;
        opt.snapshotDiff = settings.scanSnapshotDiff;
        opt.includeHeuristicInSoftware = settings.showHeuristic;
        const un::InventoryScanResult bundle = un::InventoryService::scan(opt);
        for (const un::InventorySourceError &err : bundle.errors) {
            std::fprintf(stderr, "%s\n", qPrintable(QStringLiteral("数据源异常：") + err.message));
        }
        return un::InventoryService::projectApps(bundle, settings.showSystemComponents);
    };

    if (!exportKind.isEmpty() && !exportPath.isEmpty()) {
        const QVector<un::InstalledApp> apps = loadApps();
        QString error;
        const bool ok = (exportKind == QStringLiteral("html"))
                            ? un::Exporter::exportHtml(apps, exportPath, &error)
                            : un::Exporter::exportCsv(apps, exportPath, &error);
        if (!ok) {
            std::fprintf(stderr, "%s\n", qPrintable(QStringLiteral("导出失败：") + error));
            OleUninitialize();
            return 2;
        }
        std::printf("%s\n", qPrintable(QStringLiteral("已导出 %1 个程序到：%2")
                                           .arg(apps.size()).arg(exportPath)));
        OleUninitialize();
        return 0;
    }

    // ------------------------------------------------------------------
    // 命令行卸载：Cleveil.exe --uninstall "<名称关键词>" [--silent] [--elevate]
    // 复用界面里同一套 Uninstaller / UninstallRunner，用来在没有窗口的情况下
    // 走通"构建命令 → 执行（含 UAC 提权）→ 复核登记项"的完整链路。
    // 返回码：0 成功、3 匹配到多个或没有匹配、4 命令无法构建、5 执行失败。
    // ------------------------------------------------------------------
    QString uninstallMatch;
    bool    silentFlag  = false;
    bool    elevateFlag = false;
    for (int i = 1; i < args.size(); ++i) {
        const QString a = args.at(i);
        if (a == QStringLiteral("--uninstall")) {
            uninstallMatch = args.value(i + 1);
        } else if (a == QStringLiteral("--silent")) {
            silentFlag = true;
        } else if (a == QStringLiteral("--elevate")) {
            elevateFlag = true;
        }
    }
    if (!uninstallMatch.isEmpty()) {
        const QVector<un::InstalledApp> apps = loadApps();
        QVector<un::InstalledApp> hits;
        for (const un::InstalledApp &a : apps) {
            if (a.name.contains(uninstallMatch, Qt::CaseInsensitive)) hits.append(a);
        }
        if (hits.size() != 1) {
            std::printf("%s\n", qPrintable(QStringLiteral("匹配到 %1 个程序：").arg(hits.size())));
            for (const un::InstalledApp &h : hits) std::printf("  - %s\n", qPrintable(h.name));
            OleUninitialize();
            return 3;
        }

        const un::InstalledApp app  = hits.first();
        const un::UninstallPlan plan = un::Uninstaller::buildPlan(app, silentFlag);
        std::printf("%s\n", qPrintable(QStringLiteral("目标：%1").arg(app.name)));
        std::printf("%s\n", qPrintable(QStringLiteral("命令：%1").arg(plan.displayCommand)));
        std::printf("%s\n", qPrintable(QStringLiteral("静默=%1  需要管理员=%2")
                                           .arg(silentFlag ? QStringLiteral("是") : QStringLiteral("否"),
                                                plan.needsAdmin ? QStringLiteral("是") : QStringLiteral("否"))));
        if (!plan.valid) {
            std::printf("%s\n", qPrintable(QStringLiteral("无法构建卸载命令：") + plan.reason));
            OleUninitialize();
            return 4;
        }

        un::UninstallRunner runner;
        QEventLoop loop;
        int  exitCode = -1;
        bool started  = false;
        QObject::connect(&runner, &un::UninstallRunner::finished, &loop,
                         [&](bool ok, quint32 code, const QString &err) {
                             started  = ok;
                             exitCode = static_cast<int>(code);
                             if (!ok)
                                 std::printf("%s\n", qPrintable(QStringLiteral("进程未能启动：") + err));
                             loop.quit();
                         });
        runner.run(plan, elevateFlag || plan.needsAdmin);
        loop.exec();

        // 卸载器返回码不可信。和界面一样按安装记录复核，最多等 5 分钟。
        bool gone = false;
        for (int i = 0; i < 120; ++i) {
            if (!un::appStillInstalled(app)) {
                gone = true;
                break;
            }
            QThread::msleep(2500);
        }
        std::printf("%s\n", qPrintable(QStringLiteral("退出码=%1  安装记录已消失=%2")
                                           .arg(exitCode)
                                           .arg(gone ? QStringLiteral("是") : QStringLiteral("否"))));
        OleUninitialize();
        return gone ? 0 : 5;
    }

    // ------------------------------------------------------------------
    // 命令行残留清理：Cleveil.exe --clean-leftovers "<安装目录>" [--apply]
    // 用于"程序已经从应用列表里卸载掉、但目录还留着"的补救场景：把这个目录
    // 伪装成一个 InstalledApp（只填名字和安装位置），交给界面同一套
    // LeftoverScanner 扫描；加 --apply 时用 LeftoverRemover 真正删除
    // （目录走回收站，注册表项删除前先导 .reg 备份）。
    // 不加 --apply 只打印清单（dry-run），返回码 0。
    // ------------------------------------------------------------------
    QString cleanDir;
    for (int i = 1; i < args.size(); ++i) {
        if (args.at(i) == QStringLiteral("--clean-leftovers")) cleanDir = args.value(i + 1);
    }
    if (!cleanDir.isEmpty()) {
        QDir dir(cleanDir);
        if (!dir.exists()) {
            std::printf("%s\n", qPrintable(QStringLiteral("目录不存在：") + cleanDir));
            OleUninitialize();
            return 3;
        }

        // 只有安装位置时，扫描器用目录名 + 目录内容做匹配
        un::InstalledApp app;
        const QString absPath = QDir::toNativeSeparators(dir.absolutePath());
        app.name = absPath.section(QLatin1Char('\\'), -1);
        app.installLocation = absPath;
        app.keywords << app.name;

        const QVector<un::LeftoverItem> items = un::LeftoverScanner::scan(app);
        qint64 totalBytes = 0;
        std::printf("%s\n", qPrintable(QStringLiteral("扫描 %1 → 共 %2 项")
                                           .arg(absPath).arg(items.size())));
        for (const un::LeftoverItem &it : items) {
            const QString sizeText = it.sizeBytes > 0
                                         ? QStringLiteral("  %1 MB").arg(double(it.sizeBytes) / 1024.0 / 1024.0, 0, 'f', 1)
                                         : QString();
            QString line = QStringLiteral("  [%1] %2  %3").arg(it.category, it.title, it.detail);
            line += sizeText;
            if (!it.note.isEmpty()) line += QStringLiteral("  (%1)").arg(it.note);
            std::printf("%s\n", qPrintable(line));
            if (it.sizeBytes > 0) totalBytes += it.sizeBytes;
        }
        std::printf("%s\n", qPrintable(QStringLiteral("合计 %1 MB").arg(double(totalBytes) / 1024.0 / 1024.0, 0, 'f', 1)));

        const bool apply = args.contains(QStringLiteral("--apply"));
        if (!apply) {
            std::printf("%s\n", qPrintable(QStringLiteral("dry-run：未做任何删除，确认无误后加 --apply 执行")));
            OleUninitialize();
            return 0;
        }

        // 只删除路径已经对上的项（LeftoverMatch::PathBound）。
        // 同名或同厂商的项默认不勾，这里也不删。危险路径再挡一次。
        QVector<un::LeftoverItem> targets;
        for (const un::LeftoverItem &it : items) {
            if (it.detail.trimmed().isEmpty()) continue;
            if (it.match != un::LeftoverMatch::PathBound) continue;
            if ((it.kind == un::LeftoverKind::Folder || it.kind == un::LeftoverKind::File)
                && un::isDangerousPath(it.detail))
                continue;
            targets.append(it);
        }
        // AppLocalDataLocation 已经是 %LOCALAPPDATA%\Cleveil\Cleveil，和界面里的备份根一致
        const QString backupRoot = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        const QString backupDir = backupRoot + QStringLiteral("/backup/")
                                  + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
        const un::RemovalResult r = un::LeftoverRemover::remove(targets, backupDir);
        std::printf("%s\n", qPrintable(QStringLiteral("成功 %1 项、失败 %2 项、释放 %3 MB")
                                           .arg(r.successCount).arg(r.failedCount)
                                           .arg(double(r.reclaimedBytes) / 1024.0 / 1024.0, 0, 'f', 1)));
        for (const QString &e : r.errors) std::printf("  错误：%s\n", qPrintable(e));
        for (const QString &b : r.backups) std::printf("  备份：%s\n", qPrintable(b));
        OleUninitialize();
        return r.failedCount == 0 ? 0 : 5;
    }

    un::MainWindow window;
    window.show();

    const int code = app.exec();
    OleUninitialize();
    return code;
}
