// ============================================================================
//  mainwindow.cpp —— 主界面
// ============================================================================

#include "ui/mainwindow.h"

#include "core/exporter.h"
#include "core/iconprovider.h"
#include "core/inventory/inventory_service.h"
#include "core/leftovers.h"
#include "core/registryscanner.h"
#include "core/remover.h"
#include "core/signature.h"
#include "core/uninstaller.h"
#include "core/winutil.h"

#include "ui/appmodel.h"
#include "ui/batchwizard.h"
#include "ui/leftoverdialog.h"
#include "ui/monitordialog.h"
#include "ui/uninstalldialog.h"
#include "ui/settingsdialog.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableView>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>

namespace un {

namespace {

// 生成一个可选中、可换行的"值"标签
QLabel *makeValueLabel()
{
    QLabel *label = new QLabel(QStringLiteral("—"));
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setStyleSheet(QStringLiteral("color:#3f4653;"));
    return label;
}

// QLabel 换行只在空格处断开，像 HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\... 这种
// 没有空格的长路径会把详情面板的最小宽度撑大，导致右侧内容被裁掉。
// 这里在路径分隔符后插入零宽空格（U+200B），让它可以在任意分段处折行。
QString breakablePath(const QString &path)
{
    QString out;
    out.reserve(path.size() * 2);
    for (const QChar ch : path) {
        out.append(ch);
        if (ch == QLatin1Char('\\') || ch == QLatin1Char('/') || ch == QLatin1Char('_'))
            out.append(QChar(0x200B));
    }
    return out;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("Cleveil"));

    m_settings = AppSettings::load();
    m_admin = isRunningAsAdmin();

    m_model = new AppTableModel(this);
    m_proxy = new AppFilterProxy(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setShowSystemComponents(m_settings.showSystemComponents);
    m_proxy->setShowUpdates(m_settings.showUpdates);
    m_proxy->setShowHeuristic(m_settings.showHeuristic);

    buildActions();
    buildUi();

    QSettings s;
    const QByteArray geometry = s.value(QStringLiteral("ui/geometry")).toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    } else {
        // 默认尺寸要照顾高 DPI：系统缩放 125% 时 1536 宽的屏幕只有 ~1228 逻辑像素，
        // 直接用 1180x720 会顶出屏幕，所以按"可用区域减一圈"来算并居中摆放。
        const QRect avail = screen() ? screen()->availableGeometry() : QRect(0, 0, 1200, 760);

        const int w = qMin(1120, qMax(820, avail.width() - 80));
        const int h = qMin(700,  qMax(520, avail.height() - 80));
        resize(w, h);
        move(avail.left() + qMax(0, (avail.width() - w) / 2),
             avail.top() + qMax(0, (avail.height() - h) / 2));
    }

    refresh();
}

// ===========================================================================
// 界面搭建
// ===========================================================================

void MainWindow::buildActions()
{
    // ----- 文件 -----
    QAction *actExportCsv = new QAction(QStringLiteral("导出为 CSV…"), this);
    connect(actExportCsv, &QAction::triggered, this, &MainWindow::exportCsv);

    QAction *actExportHtml = new QAction(QStringLiteral("导出为 HTML 报告…"), this);
    connect(actExportHtml, &QAction::triggered, this, &MainWindow::exportHtml);

    QAction *actRelaunch = new QAction(QStringLiteral("以管理员身份重新启动"), this);
    connect(actRelaunch, &QAction::triggered, this, &MainWindow::relaunchAsAdmin);

    QAction *actQuit = new QAction(QStringLiteral("退出"), this);
    actQuit->setShortcut(QKeySequence::Quit);
    connect(actQuit, &QAction::triggered, this, &QWidget::close);

    // ----- 操作 -----
    m_actUninstall = new QAction(QStringLiteral("卸载"), this);
    m_actUninstall->setShortcut(QKeySequence(QStringLiteral("Ctrl+U")));
    connect(m_actUninstall, &QAction::triggered, this, &MainWindow::uninstallSelected);

    m_actSilent = new QAction(QStringLiteral("静默卸载"), this);
    connect(m_actSilent, &QAction::triggered, this, &MainWindow::silentUninstallSelected);

    m_actForce = new QAction(QStringLiteral("强制卸载"), this);
    connect(m_actForce, &QAction::triggered, this, &MainWindow::forceUninstallSelected);

    m_actOpenFolder = new QAction(QStringLiteral("打开安装目录"), this);
    connect(m_actOpenFolder, &QAction::triggered, this, &MainWindow::openInstallFolder);

    QAction *actOpenRegistry = new QAction(QStringLiteral("定位到注册表项"), this);
    connect(actOpenRegistry, &QAction::triggered, this, &MainWindow::openRegistryEditor);

    // 不卸载也能先看"卸载后会留下什么"：只扫描、不删除，方便卸载前评估。
    m_actScanLeftover = new QAction(QStringLiteral("扫描残留文件…"), this);
    connect(m_actScanLeftover, &QAction::triggered, this, &MainWindow::scanLeftoverOfCurrent);

    m_actCopy = new QAction(QStringLiteral("复制程序信息"), this);
    connect(m_actCopy, &QAction::triggered, this, &MainWindow::copyDetails);

    // 批量卸载向导：主界面多选后一次带走，逐项执行并汇总
    m_actBatch = new QAction(QStringLiteral("批量卸载向导…"), this);
    connect(m_actBatch, &QAction::triggered, this, &MainWindow::openBatchWizard);

    // 安装监控：注册表通知通道实时捕获安装/卸载
    m_actMonitor = new QAction(QStringLiteral("安装监控…"), this);
    connect(m_actMonitor, &QAction::triggered, this, &MainWindow::openMonitor);

    // ----- 视图 -----
    QAction *actRefresh = new QAction(QStringLiteral("刷新列表"), this);
    actRefresh->setShortcut(QKeySequence::Refresh);
    connect(actRefresh, &QAction::triggered, this, &MainWindow::refresh);

    QAction *actShowSystem = new QAction(QStringLiteral("显示系统组件"), this);
    actShowSystem->setCheckable(true);
    actShowSystem->setChecked(m_settings.showSystemComponents);
    connect(actShowSystem, &QAction::toggled, this, [this](bool on) {
        m_settings.showSystemComponents = on;
        m_settings.save();
        m_proxy->setShowSystemComponents(on);
        updateStatusBar();
    });

    QAction *actShowUpdates = new QAction(QStringLiteral("显示更新与补丁"), this);
    actShowUpdates->setCheckable(true);
    actShowUpdates->setChecked(m_settings.showUpdates);
    connect(actShowUpdates, &QAction::toggled, this, [this](bool on) {
        m_settings.showUpdates = on;
        m_settings.save();
        m_proxy->setShowUpdates(on);
        updateStatusBar();
    });

    QAction *actSettings = new QAction(QStringLiteral("选项…"), this);
    connect(actSettings, &QAction::triggered, this, &MainWindow::editSettings);

    QAction *actAbout = new QAction(QStringLiteral("关于 Cleveil"), this);
    connect(actAbout, &QAction::triggered, this, &MainWindow::showAbout);

    // ----- 菜单栏 -----
    QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("文件"));
    fileMenu->addAction(actExportCsv);
    fileMenu->addAction(actExportHtml);
    fileMenu->addSeparator();
    fileMenu->addAction(actRelaunch);
    fileMenu->addSeparator();
    fileMenu->addAction(actQuit);

    QMenu *actionMenu = menuBar()->addMenu(QStringLiteral("操作"));
    actionMenu->addAction(m_actUninstall);
    actionMenu->addAction(m_actSilent);
    actionMenu->addAction(m_actForce);
    actionMenu->addSeparator();
    actionMenu->addAction(m_actBatch);
    actionMenu->addAction(m_actMonitor);
    actionMenu->addSeparator();
    actionMenu->addAction(m_actOpenFolder);
    actionMenu->addAction(actOpenRegistry);
    actionMenu->addAction(m_actScanLeftover);
    actionMenu->addAction(m_actCopy);

    QMenu *viewMenu = menuBar()->addMenu(QStringLiteral("视图"));
    viewMenu->addAction(actRefresh);
    viewMenu->addSeparator();
    viewMenu->addAction(actShowSystem);
    viewMenu->addAction(actShowUpdates);

    QMenu *helpMenu = menuBar()->addMenu(QStringLiteral("帮助"));
    helpMenu->addAction(actSettings);
    helpMenu->addAction(actAbout);

    // ----- 工具栏 -----
    QToolBar *toolbar = addToolBar(QStringLiteral("主工具栏"));
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolbar->addAction(actRefresh);
    toolbar->addSeparator();
    toolbar->addAction(m_actUninstall);
    toolbar->addAction(m_actSilent);
    toolbar->addAction(m_actForce);
    toolbar->addSeparator();
    toolbar->addAction(m_actBatch);
    toolbar->addAction(m_actMonitor);

    // 子动作默认禁用，选中某一行后才可用
    m_actUninstall->setEnabled(false);
    m_actSilent->setEnabled(false);
    m_actForce->setEnabled(false);
    m_actOpenFolder->setEnabled(false);
    m_actCopy->setEnabled(false);
}

void MainWindow::buildUi()
{
    QSplitter *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);

    // ---------------- 左侧：筛选栏 + 列表 ----------------
    QWidget *leftPane = new QWidget(splitter);
    QVBoxLayout *leftLayout = new QVBoxLayout(leftPane);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(8);

    QHBoxLayout *filterRow = new QHBoxLayout;
    filterRow->setContentsMargins(10, 10, 10, 0);
    filterRow->setSpacing(8);

    m_search = new QLineEdit(leftPane);
    m_search->setPlaceholderText(QStringLiteral("搜索程序名 / 发布者 / 安装路径…"));
    m_search->setClearButtonEnabled(true);
    connect(m_search, &QLineEdit::textChanged, this, &MainWindow::onSearchChanged);
    filterRow->addWidget(m_search, 1);

    m_publisherFilter = new QComboBox(leftPane);
    m_publisherFilter->addItems({QStringLiteral("全部发布者"), QStringLiteral("仅微软"),
                                 QStringLiteral("仅第三方")});
    connect(m_publisherFilter, &QComboBox::currentIndexChanged,
            this, &MainWindow::onPublisherFilterChanged);
    filterRow->addWidget(m_publisherFilter);

    leftLayout->addLayout(filterRow);

    m_table = new QTableView(leftPane);
    m_table->setModel(m_proxy);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setSortingEnabled(true);
    m_table->sortByColumn(AppTableModel::Name, Qt::AscendingOrder);
    m_table->setAlternatingRowColors(true);
    m_table->setShowGrid(false);
    m_table->setWordWrap(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(28);

    QHeaderView *header = m_table->horizontalHeader();
    header->setSectionResizeMode(AppTableModel::Name, QHeaderView::Stretch);
    header->setSectionResizeMode(AppTableModel::Publisher, QHeaderView::Interactive);
    header->setSectionResizeMode(AppTableModel::Version, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(AppTableModel::InstallDate, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(AppTableModel::Size, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(AppTableModel::Type, QHeaderView::ResizeToContents);
    header->resizeSection(AppTableModel::Publisher, 180);

    leftLayout->addWidget(m_table, 1);

    connect(m_table->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &MainWindow::onSelectionChanged);
    connect(m_table, &QTableView::customContextMenuRequested,
            this, &MainWindow::showContextMenu);
    connect(m_table, &QTableView::doubleClicked, this, [this](const QModelIndex &index) {
        Q_UNUSED(index)
        if (m_admin || m_actUninstall->isEnabled()) uninstallSelected();
    });

    // ---------------- 右侧：详情 ----------------
    QScrollArea *scroll = new QScrollArea(splitter);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMinimumWidth(330);
    scroll->setMaximumWidth(470);
    // 详情内容一律换行显示，禁止出现横向滚动条（否则内容会被推到右侧看不见）
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    QWidget *detail = new QWidget(scroll);
    QVBoxLayout *detailLayout = new QVBoxLayout(detail);
    detailLayout->setContentsMargins(16, 14, 16, 14);
    detailLayout->setSpacing(10);

    QHBoxLayout *topRow = new QHBoxLayout;
    m_detailIcon = new QLabel(detail);
    m_detailIcon->setFixedSize(54, 54);
    m_detailIcon->setAlignment(Qt::AlignCenter);

    QVBoxLayout *nameBox = new QVBoxLayout;
    nameBox->setSpacing(2);
    m_detailName = new QLabel(QStringLiteral("未选择程序"), detail);
    QFont nameFont = m_detailName->font();
    nameFont.setPointSize(nameFont.pointSize() + 3);
    nameFont.setBold(true);
    m_detailName->setFont(nameFont);
    m_detailName->setWordWrap(true);

    m_detailPublisher = new QLabel(QString(), detail);
    m_detailPublisher->setStyleSheet(QStringLiteral("color:#6b7280;"));
    m_detailPublisher->setWordWrap(true);

    nameBox->addWidget(m_detailName);
    nameBox->addWidget(m_detailPublisher);
    topRow->addWidget(m_detailIcon);
    topRow->addSpacing(10);
    topRow->addLayout(nameBox, 1);
    detailLayout->addLayout(topRow);

    QFrame *line = new QFrame(detail);
    line->setFrameShape(QFrame::HLine);
    line->setStyleSheet(QStringLiteral("color:#e5e7eb;"));
    detailLayout->addWidget(line);

    m_valueVersion  = makeValueLabel();
    m_valueDate     = makeValueLabel();
    m_valueSize     = makeValueLabel();
    m_valueType     = makeValueLabel();
    m_valueLocation = makeValueLabel();
    m_valueRegistry = makeValueLabel();

    // 用 QGridLayout 而不是 QFormLayout：标签列固定宽度，
    // 高 DPI 下 QFormLayout 的列宽会被内容撑开，导致值跑到很右边。
    QGridLayout *form = new QGridLayout;
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(8);
    form->setColumnStretch(1, 1);

    auto addField = [&](int row, const QString &labelText, QWidget *value) {
        QLabel *label = new QLabel(labelText, detail);
        label->setStyleSheet(QStringLiteral("color:#6b7280;"));
        label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        form->addWidget(label, row, 0, Qt::AlignLeft | Qt::AlignTop);
        form->addWidget(value, row, 1);
    };

    addField(0, QStringLiteral("版本"),     m_valueVersion);
    addField(1, QStringLiteral("安装日期"), m_valueDate);
    addField(2, QStringLiteral("占用空间"), m_valueSize);
    addField(3, QStringLiteral("安装类型"), m_valueType);
    addField(4, QStringLiteral("安装位置"), m_valueLocation);
    addField(5, QStringLiteral("注册表项"), m_valueRegistry);
    detailLayout->addLayout(form);

    QLabel *cmdTitle = new QLabel(QStringLiteral("卸载命令"), detail);
    cmdTitle->setStyleSheet(QStringLiteral("color:#6b7280;margin-top:6px;"));
    detailLayout->addWidget(cmdTitle);

    m_valueCommand = new QPlainTextEdit(detail);
    m_valueCommand->setReadOnly(true);
    m_valueCommand->setMaximumHeight(86);
    m_valueCommand->setStyleSheet(QStringLiteral(
        "QPlainTextEdit{background:#f6f7fb;border:1px solid #e5e7eb;border-radius:6px;"
        "font-family:Consolas,monospace;font-size:12px;}"));
    detailLayout->addWidget(m_valueCommand);

    detailLayout->addStretch(1);

    m_btnUninstall = new QPushButton(QStringLiteral("卸载"), detail);
    m_btnSilent    = new QPushButton(QStringLiteral("静默卸载"), detail);
    m_btnForce     = new QPushButton(QStringLiteral("强制卸载"), detail);
    m_btnUninstall->setObjectName(QStringLiteral("primaryButton"));

    QHBoxLayout *btnRow = new QHBoxLayout;
    btnRow->setSpacing(8);
    btnRow->addWidget(m_btnUninstall, 1);
    btnRow->addWidget(m_btnSilent, 1);
    detailLayout->addLayout(btnRow);

    // 详情面板本身只有 ~400 逻辑像素宽，三个按钮挤一行会把第三个顶出可视区，
    // 所以把"强制卸载"单独放一行并拉满宽度。
    detailLayout->addWidget(m_btnForce);

    connect(m_btnUninstall, &QPushButton::clicked, this, &MainWindow::uninstallSelected);
    connect(m_btnSilent,    &QPushButton::clicked, this, &MainWindow::silentUninstallSelected);
    connect(m_btnForce,     &QPushButton::clicked, this, &MainWindow::forceUninstallSelected);

    m_btnUninstall->setEnabled(false);
    m_btnSilent->setEnabled(false);
    m_btnForce->setEnabled(false);

    scroll->setWidget(detail);

    splitter->addWidget(leftPane);
    splitter->addWidget(scroll);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    splitter->setSizes({720, 400});

    setCentralWidget(splitter);

    // ---------------- 状态栏 ----------------
    m_statusCount = new QLabel(this);
    statusBar()->addWidget(m_statusCount);

    m_busyBar = new QProgressBar(this);
    m_busyBar->setRange(0, 0);          // 不确定进度：来回滚动
    m_busyBar->setFixedWidth(140);
    m_busyBar->setVisible(false);
    statusBar()->addPermanentWidget(m_busyBar);
}

// ===========================================================================
// 数据加载与刷新
// ===========================================================================

void MainWindow::refresh()
{
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    m_busyBar->setVisible(true);

    // 多源枚举：ARP 逻辑仍由 RegistryScanner 提供，经 InventoryService 合并其它来源。
    InventoryScanOptions opt;
    opt.arp = true;
    opt.msi = m_settings.scanExtraSources;
    opt.appx = m_settings.scanExtraSources;
    opt.driverPackages = m_settings.scanExtraSources;
    opt.services = m_settings.scanExtraSources;
    opt.installerFiles = m_settings.scanExtraSources;
    opt.shortcuts = m_settings.scanShortcuts;
    opt.exeVersion = m_settings.scanExeVersion;
    opt.snapshotDiff = m_settings.scanSnapshotDiff;
    opt.includeHeuristicInSoftware = m_settings.showHeuristic;

    const InventoryScanResult bundle = InventoryService::scan(opt);
    QVector<InstalledApp> apps = InventoryService::projectApps(bundle, m_settings.showSystemComponents);

    IconProvider::instance().clear();
    m_model->setApps(std::move(apps));

    m_busyBar->setVisible(false);
    QGuiApplication::restoreOverrideCursor();

    if (!bundle.errors.isEmpty()) {
        QStringList msgs;
        for (int i = 0; i < bundle.errors.size() && i < 3; ++i)
            msgs << bundle.errors.at(i).message;
        statusBar()->showMessage(QStringLiteral("部分数据源异常：%1").arg(msgs.join(QStringLiteral("；"))),
                                 8000);
    }

    updateStatusBar();
    updateDetailPanel();
}

void MainWindow::updateStatusBar()
{
    const int visible = m_proxy->rowCount();
    const int total = m_model->rowCount();

    qint64 totalBytes = 0;
    for (const InstalledApp &app : m_model->apps()) {
        if (app.sizeBytes > 0) totalBytes += app.sizeBytes;
    }

    QString text = QStringLiteral("显示 %1 / 共 %2 个程序　已知占用 %3")
                       .arg(visible)
                       .arg(total)
                       .arg(formatBytes(totalBytes));
    if (!m_admin) text += QStringLiteral("　（当前为非管理员权限）");

    m_statusCount->setText(text);
}

InstalledApp MainWindow::appAtProxyRow(int proxyRow) const
{
    const QModelIndex proxyIndex = m_proxy->index(proxyRow, 0);
    const QModelIndex sourceIndex = m_proxy->mapToSource(proxyIndex);
    return m_model->appAt(sourceIndex.row());
}

QVector<InstalledApp> MainWindow::selectedApps() const
{
    QVector<InstalledApp> result;
    if (!m_table->selectionModel()) return result;

    const QModelIndexList rows = m_table->selectionModel()->selectedRows();
    for (const QModelIndex &index : rows) {
        const InstalledApp app = m_model->appAt(m_proxy->mapToSource(index).row());
        if (app.name.isEmpty()) continue;
        result << app;
    }
    return result;
}

// ===========================================================================
// 详情面板
// ===========================================================================

void MainWindow::onSelectionChanged()
{
    updateDetailPanel();
}

void MainWindow::updateDetailPanel()
{
    const QModelIndexList rows = m_table->selectionModel()->selectedRows();

    if (rows.isEmpty()) {
        m_detailIcon->clear();
        m_detailName->setText(QStringLiteral("未选择程序"));
        m_detailPublisher->setText(QString());
        m_valueVersion->setText(QStringLiteral("—"));
        m_valueDate->setText(QStringLiteral("—"));
        m_valueSize->setText(QStringLiteral("—"));
        m_valueType->setText(QStringLiteral("—"));
        m_valueLocation->setText(QStringLiteral("—"));
        m_valueRegistry->setText(QStringLiteral("—"));
        m_valueCommand->clear();

        m_btnUninstall->setEnabled(false);
        m_btnSilent->setEnabled(false);
        m_btnForce->setEnabled(false);
        m_actUninstall->setEnabled(false);
        m_actSilent->setEnabled(false);
        m_actForce->setEnabled(false);
        m_actOpenFolder->setEnabled(false);
        m_actScanLeftover->setEnabled(false);
        m_actCopy->setEnabled(false);
        return;
    }

    const InstalledApp app = appAtProxyRow(rows.first().row());

    m_detailIcon->setPixmap(IconProvider::instance().icon(app, 54).pixmap(54, 54));
    m_detailName->setText(app.name);
    m_detailPublisher->setText(app.publisher.isEmpty() ? QStringLiteral("未知发布者")
                                                       : app.publisher);
    m_valueVersion->setText(app.version.isEmpty() ? QStringLiteral("—") : app.version);
    m_valueDate->setText(app.installDate.isValid()
                             ? app.installDate.toString(QStringLiteral("yyyy-MM-dd"))
                             : QStringLiteral("未知"));
    m_valueSize->setText(app.sizeBytes >= 0 ? formatBytes(app.sizeBytes)
                                            : QStringLiteral("注册表未记录"));
    m_valueType->setText(packageTypeText(app.type));
    m_valueLocation->setText(app.installLocation.isEmpty()
                                 ? QStringLiteral("—")
                                 : breakablePath(app.installLocation));
    m_valueLocation->setToolTip(app.installLocation);
    m_valueRegistry->setText(breakablePath(app.registryPath));
    m_valueRegistry->setToolTip(app.registryPath);

    if (app.type == PackageType::Driver) {
        m_valueCommand->setPlainText(
            app.canUninstall()
                ? QStringLiteral("卸载时由程序调用 pnputil 删除该 OEM 驱动包，执行前会再次核对文件名和设备类。这里不提供可复制的删除命令。")
                : (app.inventoryNote.isEmpty()
                       ? QStringLiteral("该驱动包不允许从这里卸载。")
                       : app.inventoryNote));
    } else if (app.canUninstall()) {
        QString cmd = app.uninstallString;
        if (cmd.isEmpty()) cmd = app.quietUninstallString;
        m_valueCommand->setPlainText(cmd);
    } else {
        m_valueCommand->setPlainText(QStringLiteral("（该程序没有登记卸载命令，只能强制卸载）"));
    }

    const bool canUninstall = app.canUninstall();
    m_btnUninstall->setEnabled(canUninstall);
    m_btnSilent->setEnabled(canUninstall);
    m_btnForce->setEnabled(true);
    m_actUninstall->setEnabled(canUninstall);
    m_actSilent->setEnabled(canUninstall);
    m_actForce->setEnabled(true);
    m_actOpenFolder->setEnabled(!app.installLocation.isEmpty());
    m_actScanLeftover->setEnabled(true);
    m_actCopy->setEnabled(true);

    // 多选时禁用"强制卸载"（一次只处理一个）
    if (rows.size() > 1) m_btnForce->setEnabled(false);
}

// ===========================================================================
// 搜索与筛选
// ===========================================================================

void MainWindow::onSearchChanged(const QString &text)
{
    m_proxy->setQuery(text);
    updateStatusBar();
}

void MainWindow::onPublisherFilterChanged()
{
    switch (m_publisherFilter->currentIndex()) {
    case 1: m_proxy->setPublisherFilter(QStringLiteral("微软")); break;
    case 2: m_proxy->setPublisherFilter(QStringLiteral("第三方")); break;
    default: m_proxy->setPublisherFilter(QStringLiteral("全部")); break;
    }
    updateStatusBar();
}

// ===========================================================================
// 右键菜单
// ===========================================================================

void MainWindow::showContextMenu(const QPoint &pos)
{
    QMenu menu(this);
    menu.addAction(m_actUninstall);
    menu.addAction(m_actSilent);
    menu.addAction(m_actForce);
    menu.addSeparator();
    menu.addAction(m_actBatch);
    menu.addAction(m_actMonitor);
    menu.addSeparator();
    menu.addAction(m_actOpenFolder);
    menu.addAction(m_actScanLeftover);
    menu.addAction(m_actCopy);

    menu.exec(m_table->viewport()->mapToGlobal(pos));
}

// ===========================================================================
// 卸载流程
// ===========================================================================

bool MainWindow::confirmUninstall(const QVector<InstalledApp> &apps, bool silent)
{
    QStringList names;
    for (int i = 0; i < apps.size() && i < 8; ++i) {
        names << QStringLiteral("· %1 %2").arg(apps.at(i).name, apps.at(i).version);
    }
    if (apps.size() > 8) names << QStringLiteral("· …… 共 %1 个程序").arg(apps.size());

    QString text = silent
        ? QStringLiteral("即将以静默方式卸载以下程序：\n\n%1\n\n"
                         "静默卸载不会弹出安装器的界面，部分程序可能不支持。")
              .arg(names.join(QLatin1Char('\n')))
        : QStringLiteral("即将卸载以下程序：\n\n%1\n\n"
                         "卸载程序可能会弹出自己的窗口，请按提示完成，"
                         "期间请不要关闭本工具。")
              .arg(names.join(QLatin1Char('\n')));

    if (!m_admin) {
        text += QStringLiteral("\n\n当前不是管理员权限，部分程序可能卸载失败"
                               "（可执行\"文件 → 以管理员身份重新启动\"）。");
    }

    return QMessageBox::question(this, QStringLiteral("确认卸载"), text,
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes)
           == QMessageBox::Yes;
}

void MainWindow::uninstallSelected()
{
    runUninstall(selectedApps(), false);
}

void MainWindow::silentUninstallSelected()
{
    runUninstall(selectedApps(), true);
}

void MainWindow::runUninstall(const QVector<InstalledApp> &apps, bool silent)
{
    if (apps.isEmpty()) return;

    if (!confirmUninstall(apps, silent)) return;

    QVector<UninstallTask> tasks;
    QStringList skipped;

    for (const InstalledApp &app : apps) {
        UninstallPlan plan = Uninstaller::buildPlan(app, silent);
        if (!plan.valid) {
            skipped << (plan.reason.isEmpty()
                            ? app.name
                            : QStringLiteral("%1（%2）").arg(app.name, plan.reason));
            continue;
        }

        UninstallTask task;
        task.app     = app;
        task.plan    = plan;
        // 普通卸载让安装器自己弹 UAC。驱动包的 pnputil 不会弹，需要管理员时这里提权。
        task.elevate = plan.needsAdmin
                       && (app.type == PackageType::Driver
                           || (silent && m_settings.elevateSilent));
        tasks << task;
    }

    if (tasks.isEmpty()) {
        QMessageBox::information(
            this, QStringLiteral("无法卸载"),
            QStringLiteral("所选项目无法卸载：\n\n%1\n\n"
                           "请改用\"强制卸载\"清理。")
                .arg(skipped.join(QLatin1Char('\n'))));
        return;
    }

    if (!skipped.isEmpty()) {
        QMessageBox::information(
            this, QStringLiteral("部分程序被跳过"),
            QStringLiteral("以下项目无法卸载，已跳过：\n\n%1\n\n"
                           "可稍后用\"强制卸载\"处理。")
                .arg(skipped.join(QLatin1Char('\n'))));
    }

    UninstallDialog dialog(tasks, this);
    dialog.exec();

    const QStringList succeeded = dialog.succeededApps();
    refresh();

    if (succeeded.isEmpty() || !m_settings.autoScanLeftovers) return;

    const QString question =
        QStringLiteral("以下程序已卸载：\n\n· %1\n\n是否立即扫描它们的残留项（注册表、"
                       "安装目录、用户数据、快捷方式、服务、计划任务）？")
            .arg(succeeded.join(QLatin1Char('\n')));

    if (QMessageBox::question(this, QStringLiteral("清理残留"), question,
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes)
        != QMessageBox::Yes)
        return;

    // 逐个程序打开残留向导（数量一般不多）
    for (const QString &name : succeeded) {
        for (const InstalledApp &app : apps) {
            if (app.name == name) {
                startLeftoverScan(app);
                break;
            }
        }
    }
}

void MainWindow::startLeftoverScan(const InstalledApp &app)
{
    LeftoverDialog dialog(app, this);
    dialog.exec();
    refresh();
}

// 只扫描、不删除：给用户在"真正卸载之前"看清单的机会。
// 对话框里的"删除选中项"按钮仍然可用，但默认不勾选任何弱匹配项。
void MainWindow::scanLeftoverOfCurrent()
{
    const QVector<InstalledApp> apps = selectedApps();
    if (apps.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("扫描残留"),
                                 QStringLiteral("请先在列表里选中一个程序。"));
        return;
    }
    startLeftoverScan(apps.first());
}

// ===========================================================================
// 强制卸载
// ===========================================================================

void MainWindow::forceUninstallSelected()
{
    const QVector<InstalledApp> apps = selectedApps();
    if (apps.isEmpty()) return;

    if (apps.size() > 1) {
        QMessageBox::information(this, QStringLiteral("强制卸载"),
                                 QStringLiteral("强制卸载一次只能处理一个程序，"
                                                "请只选中一项。"));
        return;
    }

    const InstalledApp app = apps.first();

    const QString warning =
        QStringLiteral("对「%1」执行强制卸载，会进行以下操作：\n\n"
                       "1. 结束安装目录下正在运行的进程\n"
                       "2. 删除它在\"应用列表\"中的登记项（删除前自动导出 .reg 备份）\n"
                       "3. 扫描残留项，由你逐项勾选后清理（文件进回收站）\n\n"
                       "适用场景：程序自带的卸载器损坏、卸载到一半失败、安装包丢失。\n\n"
                       "确定继续吗？")
            .arg(app.name);

    if (QMessageBox::warning(this, QStringLiteral("强制卸载"),
                             warning, QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes)
        return;

    // ---- 第一步：结束相关进程 ----
    const QStringList processes = LeftoverRemover::relatedProcessNames(app);
    if (!processes.isEmpty()) {
        const auto ret = QMessageBox::question(
            this, QStringLiteral("结束进程"),
            QStringLiteral("以下进程正在运行，继续操作前需要结束它们：\n\n%1\n\n是否结束？")
                .arg(processes.join(QLatin1Char('\n'))),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret == QMessageBox::Yes) {
            const int killed = ForceUninstaller::terminateProcesses(app);
            Q_UNUSED(killed)
        }
    }

    // ---- 第二步：删除登记项 ----
    const QString backupDir = m_settings.backupRegistry ? ForceUninstaller::makeBackupDir()
                                                        : QString();
    QString backupFile;
    QString error;

    const bool removed = ForceUninstaller::removeRegistration(app, backupDir, &backupFile, &error);
    refresh();

    if (!removed && !error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("删除登记项失败"), error);
    }

    // ---- 第三步：残留扫描 ----
    startLeftoverScan(app);

    if (!backupFile.isEmpty()) {
        QMessageBox::information(
            this, QStringLiteral("注册表已备份"),
            QStringLiteral("登记项已备份到：\n%1\n\n如果误删，双击该 .reg 文件即可还原。")
                .arg(backupFile));
    }
}

// ===========================================================================
// 其它动作
// ===========================================================================

void MainWindow::openInstallFolder()
{
    const QVector<InstalledApp> apps = selectedApps();
    if (apps.isEmpty()) return;

    const QString dir = expandEnv(apps.first().installLocation.trimmed());
    if (dir.isEmpty() || !QDir(dir).exists()) {
        QMessageBox::information(this, QStringLiteral("打开安装目录"),
                                 QStringLiteral("注册表里没有记录有效的安装目录。"));
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void MainWindow::openRegistryEditor()
{
    const QVector<InstalledApp> apps = selectedApps();
    if (apps.isEmpty()) return;

    const QString path = apps.first().registryPath;
    QApplication::clipboard()->setText(path);

    QProcess::startDetached(QStringLiteral("regedit.exe"), {});

    QMessageBox::information(
        this, QStringLiteral("定位到注册表项"),
        QStringLiteral("路径已复制到剪贴板：\n%1\n\n"
                       "注册表编辑器不支持直接跳转，请在左侧逐级定位，"
                       "或把路径粘贴到地址栏（部分系统支持）。").arg(path));
}

void MainWindow::copyDetails()
{
    const QVector<InstalledApp> apps = selectedApps();
    if (apps.isEmpty()) return;

    QStringList lines;
    for (const InstalledApp &app : apps) {
        lines << QStringLiteral("名称：%1").arg(app.name)
              << QStringLiteral("版本：%1").arg(app.version)
              << QStringLiteral("发布者：%1").arg(app.publisher)
              << QStringLiteral("安装日期：%1").arg(app.installDate.isValid()
                      ? app.installDate.toString(QStringLiteral("yyyy-MM-dd"))
                      : QStringLiteral("未知"))
              << QStringLiteral("占用：%1").arg(app.sizeBytes >= 0
                      ? formatBytes(app.sizeBytes) : QStringLiteral("未知"))
              << QStringLiteral("类型：%1").arg(packageTypeText(app.type))
              << QStringLiteral("安装位置：%1").arg(app.installLocation)
              << QStringLiteral("卸载命令：%1").arg(app.uninstallString)
              << QStringLiteral("注册表项：%1").arg(app.registryPath)
              << QString();
    }

    QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
    statusBar()->showMessage(QStringLiteral("已复制 %1 个程序的信息").arg(apps.size()), 3000);
}

void MainWindow::exportCsv()
{
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmm"));
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    const QString file = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出为 CSV"),
        QDir(dir).filePath(QStringLiteral("installed-apps-%1.csv").arg(stamp)),
        QStringLiteral("CSV 文件 (*.csv)"));

    if (file.isEmpty()) return;

    // 导出"当前看到的列表"，而不是全部（含被筛掉的系统组件）
    QVector<InstalledApp> visible;
    for (int row = 0; row < m_proxy->rowCount(); ++row) {
        const QModelIndex source = m_proxy->mapToSource(m_proxy->index(row, 0));
        visible << m_model->appAt(source.row());
    }

    QString error;
    if (!Exporter::exportCsv(visible, file, &error)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
        return;
    }
    QMessageBox::information(this, QStringLiteral("导出完成"),
                             QStringLiteral("已导出 %1 个程序到：\n%2")
                                 .arg(visible.size()).arg(file));
}

void MainWindow::exportHtml()
{
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmm"));
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    const QString file = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出为 HTML 报告"),
        QDir(dir).filePath(QStringLiteral("installed-apps-%1.html").arg(stamp)),
        QStringLiteral("HTML 文件 (*.html)"));

    if (file.isEmpty()) return;

    QVector<InstalledApp> visible;
    for (int row = 0; row < m_proxy->rowCount(); ++row) {
        const QModelIndex source = m_proxy->mapToSource(m_proxy->index(row, 0));
        visible << m_model->appAt(source.row());
    }

    QString error;
    if (!Exporter::exportHtml(visible, file, &error)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
        return;
    }
    QMessageBox::information(this, QStringLiteral("导出完成"),
                             QStringLiteral("报告已生成：\n%1").arg(file));
}

void MainWindow::editSettings()
{
    SettingsDialog dialog(m_settings, this);
    if (dialog.exec() != QDialog::Accepted) return;

    m_settings = dialog.settings();
    m_settings.save();

    m_proxy->setShowSystemComponents(m_settings.showSystemComponents);
    m_proxy->setShowUpdates(m_settings.showUpdates);
    m_proxy->setShowHeuristic(m_settings.showHeuristic);
    refresh();
}

void MainWindow::relaunchAsAdmin()
{
    if (m_admin) {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("当前已经以管理员身份运行。"));
        return;
    }
    if (!relaunchSelfAsAdmin({})) {
        QMessageBox::warning(this, QStringLiteral("提权失败"),
                             QStringLiteral("未能以管理员身份重新启动（可能取消了 UAC 授权）。"));
        return;
    }
    close();
}

void MainWindow::showAbout()
{
    // 说明：关于框只讲"这个程序能做什么"，不再显示本机签名状态与 exe 路径，
    // 签名是否有效由 scripts\sign.ps1 -Verify 或资源管理器属性页自查。
    QMessageBox::about(
        this, QStringLiteral("关于 Cleveil"),
        QStringLiteral(
            "<h3>Cleveil 1.2.2</h3>"
            "<p>Windows 程序卸载与残留清理工具。</p>"
            "<p><b>功能</b><br>"
            "· 多源枚举：ARP 注册表 + MSI API + AppX + OEM 驱动包 + 服务 + 安装器卸载文件<br>"
            "· 启发式辅助：快捷方式 / EXE 版本信息 / 目录快照差分（默认关闭，单独标识）<br>"
            "· 正常卸载 / 静默卸载（MSI 走 msiexec /qn，EXE 用 QuietUninstallString）<br>"
            "· <b>批量卸载向导</b>：多选一键排队，逐项执行、进度日志、结果汇总，可随时中止<br>"
            "· <b>实时安装监控</b>：注册表变更通知 + 安装器进程侦测，秒级捕获新装程序<br>"
            "· 强制卸载：结束进程 + 删除登记项 + 残留深度清理<br>"
            "· 残留覆盖：注册表项、安装目录、用户数据、快捷方式、服务、启动项、计划任务<br>"
            "· 删除前备份：注册表导出 .reg、文件进回收站<br>"
            "· <b>数字签名</b>：内置 Authenticode 验签，签/验工具见 scripts/sign.ps1<br>"
            "· 导出 CSV / HTML 清单</p>"
            "<p>技术栈：C++20 + Qt 6 Widgets（Win32 API），无运行时依赖。</p>"));
}

// ===========================================================================
// 批量卸载向导 / 安装监控
// ===========================================================================

void MainWindow::openBatchWizard()
{
    // 只把"当前列表里可见的程序"交给向导：被搜索/过滤隐藏的项不该出现在批量清单里
    QVector<InstalledApp> visible;
    visible.reserve(m_proxy->rowCount());
    for (int row = 0; row < m_proxy->rowCount(); ++row) {
        visible.append(appAtProxyRow(row));
    }

    BatchWizard wizard(visible, selectedApps(), this);
    const int result = wizard.exec();

    // 只要跑过队列就刷新（哪怕最后点了取消，也可能已经卸掉了几个）
    if (result == QDialog::Accepted || !wizard.succeededNames().isEmpty()) {
        refresh();
    }
}

void MainWindow::openMonitor()
{
    if (!m_monitorDialog) {
        m_monitorDialog = new MonitorDialog(this);
        connect(m_monitorDialog, &MonitorDialog::uninstallRequested, this,
                [this](const InstalledApp &app) {
                    QVector<InstalledApp> one;
                    one.append(app);
                    runUninstall(one, false);
                });
        connect(m_monitorDialog, &MonitorDialog::scanLeftoverRequested, this,
                [this](const InstalledApp &app) { startLeftoverScan(app); });
    }

    m_monitorDialog->show();
    m_monitorDialog->raise();
    m_monitorDialog->activateWindow();

    if (!m_monitorDialog->isMonitoring()) m_monitorDialog->startMonitoring();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    QSettings s;
    s.setValue(QStringLiteral("ui/geometry"), saveGeometry());
    QMainWindow::closeEvent(event);
}

} // namespace un
