# Cleveil

[中文](README.md) ｜ **English**

![Platform](https://img.shields.io/badge/platform-Windows%2010%20%2F%2011-0078D6?logo=windows&logoColor=white)
![C++](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)
![Qt](https://img.shields.io/badge/Qt-6%20Widgets-41CD52?logo=qt&logoColor=white)
![License](https://img.shields.io/badge/license-MIT-blue)

A Windows uninstaller and leftover cleaner: scan installed programs → browse/search/sort → inspect details →
normal uninstall / silent uninstall / batch uninstall wizard → force uninstall with deep leftover cleanup → live
install monitor → export lists.

It reads no third-party database — every fact comes from Windows' own registry and file system; uninstalling is
handed back to the program's own uninstaller (MSI via `msiexec`, EXE via `UninstallString`).



## Table of contents

- [1. Features](#1-features)
- [2. Screenshots](#2-screenshots)
- [3. Build](#3-build)
- [4. Repository layout](#4-repository-layout)
- [5. License and disclaimer](#5-license-and-disclaimer)

---

## 1. Features

- **Program scan**: reads all four `Uninstall` branches under `HKLM` / `HKCU` (including `WOW6432Node`); deduplicated by name + version + publisher, machine-wide preferred over per-user, 64-bit view over 32-bit.
- **List and filtering**: six columns with real icons (three-level fallback), header sorting (date and size sorted numerically), live search over name / publisher / path, publisher filter, optional system components and updates, multi-select.
- **Details pane**: icon, version, install date, size, package type (MSI / EXE), install path, registry key path and the complete uninstall command (selectable).
- **Three uninstall modes**: normal (run the registered command, can be queued) / silent (`QuietUninstallString`, MSI gets `/qn /norestart`, programs without a silent switch are reported as unsupported instead of being forced) / force (kill related processes, back up and delete the registration key, then hand over to leftover cleanup).
- **Batch uninstall wizard**: pick programs → choose strategy (prefer silent / confirm each item / auto-clean leftovers) → execute (progress, timestamped per-item log, summary, stoppable mid-run).
- **Live install monitor**: `RegNotifyChangeKeyValue` on the four `Uninstall` keys plus installer-process detection every 2 seconds; new programs appear within seconds and are highlighted, uninstall events are captured as well.
- **Digital signature**: built-in Authenticode verification (`WinVerifyTrust` + `CryptQueryObject`), with `scripts/sign.ps1` supporting self-signed certificates, PFX files, thumbprints and timestamps — see [docs/SIGNING.md](docs/SIGNING.md).
- **Leftover cleanup**: scans registry, install directory, `AppData` / `ProgramData`, Start-menu and desktop shortcuts, services and scheduled tasks; strong/weak keyword matching (weak hits unchecked by default), registry keys exported to `.reg`, folders go to the Recycle Bin.

---

## 2. Screenshots

| Main window | Batch wizard (pick programs) |
| --- | --- |
| ![](docs/screenshot-main.png) | ![](docs/screenshot-batch.png) |

---

## 3. Build

Requirements:

| Component | Version (as tested) |
| --- | --- |
| Visual Studio | 2022 / 2026 with the "Desktop development with C++" workload (MSVC 19.5x) |
| Qt | 6.5 or newer, MSVC kit (tested 6.11.1 `msvc2022_64`) |
| CMake | 3.21+ (tested 3.30.5) |
| Ninja | 1.10+ (tested 1.12.1) |

One command (the script injects the MSVC environment itself, no developer prompt needed):

```powershell
cd Cleveil-1.0.0            # the project root after clone/extract
.\build.ps1                 # Release build + windeployqt deployment
.\build.ps1 -Config Debug   # Debug build
.\build.ps1 -Clean          # wipe build/ and start over
```



## 4. Repository layout

```
src/
├─ main.cpp                     entry: high-DPI policy, Fusion style, QSS theme, app icon, OleInitialize
├─ core/
│  ├─ appinfo.h                 InstalledApp struct + PackageType enum
│  ├─ winutil.h/.cpp            Win32 helpers: env expansion, icon→QPixmap, elevated exec, recycle-bin
│  │                            delete, folder size, registry read/write/backup, process & service ops
│  ├─ iconprovider.h/.cpp       three-level icon sources + cache + fallback placeholder drawing
│  ├─ registryscanner.h/.cpp    four-way Uninstall scan, field parsing, dedupe
│  ├─ uninstaller.h/.cpp        uninstall plan (MSI/EXE/silent), command splitting, QProcess & elevated runner
│  ├─ leftovers.h/.cpp          leftover scan (registry/folders/shortcuts/services/tasks) + delete & backup
│  ├─ remover.h/.cpp            force uninstall: kill processes, back up and delete the key, backup folder
│  ├─ exporter.h/.cpp           CSV / HTML export
│  ├─ installmonitor.h/.cpp     monitor: change-notification thread + installer detection + driver ABI
│  └─ signature.h/.cpp          Authenticode verification (WinVerifyTrust + CryptQueryObject)
└─ ui/
   ├─ appmodel.h/.cpp           table model (lazy icons, numeric sort) + filter proxy model
   ├─ mainwindow.h/.cpp         menus, toolbar, list, details, uninstall entries, export, monitor/wizard
   ├─ uninstalldialog.h/.cpp    uninstall queue dialog (sequential run + log + registry verification)
   ├─ batchwizard.h/.cpp        batch wizard (QWizard, three pages + sequential run + auto cleanup)
   ├─ monitordialog.h/.cpp      install monitor window (event stream + one-click uninstall + driver note)
   ├─ leftoverdialog.h/.cpp     leftover dialog (QtConcurrent background scan + progress + batch delete)
   └─ settingsdialog.h/.cpp     settings dialog
resources/
├─ icon-1024.png                icon artwork
├─ icon-256.png / app.ico       runtime PNG / multi-size icon
├─ app.rc                       ICON + VERSIONINFO (product name Cleveil)
└─ resources.qrc                Qt resources
scripts/
├─ make_icon.py                 icon tooling (rounded corners + PNG + ICO)
└─ sign.ps1                     Authenticode sign/verify script
docs/
└─ SIGNING.md                   signing options, certificate choice, driver-signing differences
```

---

## 5. License and disclaimer

Released under the **MIT License** — see [LICENSE](LICENSE).

> **Disclaimer**: uninstalling and cleaning leftovers are irreversible operations; the author is not liable for
> data loss, system damage or business interruption caused by using this tool.

If documentation and implementation disagree, the source code wins — issues are welcome.
