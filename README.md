# Cleveil

**中文** ｜ [English](README.en.md)

![Platform](https://img.shields.io/badge/platform-Windows%2010%20%2F%2011-0078D6?logo=windows&logoColor=white)
![C++](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)
![Qt](https://img.shields.io/badge/Qt-6%20Widgets-41CD52?logo=qt&logoColor=white)
![License](https://img.shields.io/badge/license-MIT-blue)

Windows 程序卸载与残留清理工具。已安装项来自 Windows 自身：注册表、MSI、应用商店包、OEM 驱动包、服务和安装器卸载文件。卸载仍交给程序自带的卸载器（MSI 走 `msiexec`）。

当前版本 **1.2.1**（修复 1.2.0 安装后无法启动）。安装包见 [Releases](https://github.com/zyidev/Cleveil/releases) 的 `Cleveil-1.2.1-win64-setup.exe`，装到当前用户目录，不需要管理员权限。



## 目录

- [一、功能一览](#一功能一览)
- [二、界面](#二界面)
- [三、构建](#三构建)
- [四、目录结构](#四目录结构)
- [五、许可证与免责声明](#五许可证与免责声明)

---

## 一、功能一览

- **扫描**：合并注册表、MSI、应用商店包、OEM 驱动包、服务和安装器卸载文件，同一程序只占一行并标出来源。快捷方式、EXE 版本信息和目录快照用于识别绿色软件，默认不显示，也不能直接卸载。
- **卸载**：正常卸载、静默卸载、强制卸载，以及可中途停止的批量向导。
- **残留清理**：扫描注册表、目录、快捷方式、服务和计划任务。文件进回收站，注册表先导出备份。运行库等系统共享组件默认不可卸载。
- **其它**：安装监控（含应用商店包）、CSV / HTML 导出。签名工具见 [docs/SIGNING.md](docs/SIGNING.md)。

---

## 二、界面

| 主窗口 | 批量卸载向导（选择程序） |
| --- | --- |
| ![](docs/screenshot-main.png) | ![](docs/screenshot-batch.png) |

---

## 三、构建

环境要求：

| 组件 | 版本（本项目实测） |
| --- | --- |
| Visual Studio | 2022 / 2026，需"使用 C++ 的桌面开发"工作负载（MSVC 19.5x） |
| Qt | 6.10 及以上，MSVC 套件（实测 6.11.1 `msvc2022_64`） |
| CMake | 3.21+（实测 3.30.5） |
| Ninja | 1.10+（实测 1.12.1） |

一键构建（脚本会自己调 `VsDevCmd.bat` 注入 MSVC 环境，不需要开"开发者命令提示符"）：

```powershell
cd Cleveil-1.2.0            # 进入项目目录（克隆/解压后的实际路径）
.\build.ps1                 # Release 构建 + windeployqt 部署 Qt 运行库
.\build.ps1 -Config Debug   # Debug 构建
.\build.ps1 -Clean          # 清空 build 后重来
```



## 四、目录结构

```
src/
├─ main.cpp                     入口：高 DPI 策略、Fusion 风格、QSS 主题、应用图标、OleInitialize
├─ core/
│  ├─ appinfo.h                 InstalledApp 数据结构 + PackageType 枚举
│  ├─ winutil.h/.cpp            Win32 封装：环境变量展开、图标转 QPixmap、提权执行、回收站删除、
│  │                            目录体积、注册表读写/备份/存在性、进程枚举与结束、服务枚举与删除
│  ├─ iconprovider.h/.cpp       三级图标来源 + 缓存 + 默认占位图绘制
│  ├─ registryscanner.h/.cpp    四路 Uninstall 键扫描、字段解析、去重合并
│  ├─ inventory/                多源枚举与合并（MSI、AppX、驱动包、服务、安装器文件、启发式）
│  ├─ uninstaller.h/.cpp        卸载计划构建（MSI/EXE/静默）、命令行拆分、QProcess 与提权执行器
│  ├─ leftovers.h/.cpp          残留扫描（注册表/目录/快捷方式/服务/计划任务）+ 删除与备份
│  ├─ remover.h/.cpp            强制卸载：结束进程、备份并删除登记项、备份目录管理
│  ├─ exporter.h/.cpp           CSV / HTML 导出
│  ├─ installmonitor.h/.cpp     安装监控：注册表变更通知线程 + 安装器进程侦测 + 驱动通道 ABI
│  └─ signature.h/.cpp          Authenticode 验签（WinVerifyTrust + CryptQueryObject）
└─ ui/
   ├─ appmodel.h/.cpp           表格模型（懒加载图标、数值排序）+ 过滤代理模型
   ├─ mainwindow.h/.cpp         主窗口：菜单、工具栏、列表、详情、卸载入口、导出、提权重启、监控/批量入口
   ├─ uninstalldialog.h/.cpp    卸载队列对话框（顺序执行 + 日志 + 注册表核验）
   ├─ batchwizard.h/.cpp        批量卸载向导（QWizard 三页 + 顺序执行 + 自动清残留）
   ├─ monitordialog.h/.cpp      安装监控窗口（事件流 + 一键卸载 + 驱动模式说明）
   ├─ leftoverdialog.h/.cpp     残留清理对话框（QtConcurrent 后台扫描 + 进度回传 + 批量删除）
   └─ settingsdialog.h/.cpp     设置（含多源枚举与启发式开关）
resources/
├─ icon-1024.png                图标原稿
├─ icon-256.png / app.ico       运行时 PNG / 多尺寸图标
├─ app.rc                       ICON + VERSIONINFO（产品名 Cleveil）
└─ resources.qrc                Qt 资源
scripts/
├─ make_icon.py                 图标加工脚本（圆角 + PNG + ICO）
└─ sign.ps1                     Authenticode 签名/验签脚本
docs/
└─ SIGNING.md                   签名方案、证书选择、驱动签名差异
```

---

## 五、许可证与免责声明

本项目以 **MIT License** 发布，见 [LICENSE](LICENSE)。

> **免责声明**：卸载与残留清理是不可逆操作，作者不对因使用本工具造成的
> 数据丢失、系统损坏或业务中断负责。

若发现文档与实现不一致，以源码为准；欢迎提 Issue 指出。
