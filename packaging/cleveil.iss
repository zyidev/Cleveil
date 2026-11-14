; ===========================================================================
;  cleveil.iss —— Cleveil 1.0.0 的 Inno Setup 安装包脚本
; ===========================================================================
;  用法（Inno Setup 6 已安装）：
;      & "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" packaging\cleveil.iss
;
;  前置条件：先由 build.ps1 构建出 build\，再执行下面的"整理分发目录"步骤，
;  把 build\ 里的 exe + Qt DLL + 插件目录拷到 dist\Cleveil-1.0.0-win64\。
;  打包时把 README、LICENSE 一并带进入口目录，安装后用户可直接查看。
;
;  设计取舍：
;   · PrivilegesRequired=lowest —— 装到 %LOCALAPPDATA%\Programs\Cleveil，
;     普通用户无需 UAC 即可安装；Cleveil 自身在需要写 HKLM 时会自行提权。
;   · 默认英文界面：Inno Setup 6 官方语言包不含简体中文，避免混入非官方 isl。
;   · SetupIconFile 用与应用一致的图标，安装包在资源管理器里显示同样的图标。
; ===========================================================================

#define MyAppName "Cleveil"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "zyidev"
#define MyAppURL "https://github.com/zyidev/Cleveil"
#define MyAppExeName "Cleveil.exe"

[Setup]
; AppId 一经发布不要修改，否则升级安装会被当成另一个程序
AppId={{B7F0E3C1-9A24-4F62-8D71-6C5A2E4B9D30}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases
VersionInfoVersion={#MyAppVersion}.0
VersionInfoProductName={#MyAppName}
VersionInfoDescription={#MyAppName} Setup
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
OutputDir=..\dist
OutputBaseFilename=Cleveil-1.0.0-win64-setup
SetupIconFile=..\resources\app.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional icons:"; Flags: unchecked

[Files]
; 整个分发目录，含 Qt6*.dll 与 generic/iconengines/imageformats/networkinformation/platforms/styles/tls
Source: "..\dist\Cleveil-1.0.0-win64\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent
