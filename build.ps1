# ============================================================================
#  Cleveil 一键构建脚本（Windows + MSVC + Qt 6）
#
#  用法：
#    .\build.ps1                      # Release 构建，并自动部署 Qt 运行库
#    .\build.ps1 -Config Debug        # Debug 构建
#    .\build.ps1 -Clean               # 先清空 build 目录，再从头构建
#    .\build.ps1 -NoDeploy            # 只编译，不跑 windeployqt（开发时更快）
#    .\build.ps1 -QtDir "D:\Qt\6.8.2\msvc2022_64"   # 指定其它 Qt 安装路径
#
#  说明：
#   1. 脚本本身不依赖"开发者命令提示符"，会自己调 VsDevCmd.bat 注入 MSVC 环境；
#   2. 必须用 MSVC 套件（msvc2022_64 + Ninja），MinGW 套件无法链接本项目用到的
#      advapi32/shell32 等 Win32 库组合；
#   3. 构建完成后 exe 与 Qt 依赖都在 $BuildDir 目录里，可直接拷到别的机器运行。
# ============================================================================
[CmdletBinding()]
param(
    [string] $QtDir    = "C:\Qt\6.11.1\msvc2022_64",
    [string] $BuildDir = "build",
    [ValidateSet("Release", "Debug")] [string] $Config = "Release",
    [switch] $Clean,
    [switch] $NoDeploy
)

$ErrorActionPreference = "Stop"

# 项目根目录 = 本脚本所在目录
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Build = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $Root $BuildDir }

function Write-Step($msg) { Write-Host "[build] $msg" -ForegroundColor Cyan }
function Fail($msg) { Write-Host "[build] 失败：$msg" -ForegroundColor Red; exit 1 }

# ---------------------------------------------------------------------------
# 1. 找到 Visual Studio，拿到 VsDevCmd.bat
# ---------------------------------------------------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = $null
if (Test-Path -LiteralPath $vswhere) {
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $vsPath) { Fail "没找到带 C++ 工具集的 Visual Studio，请先安装 VS 2022/2026 的“使用 C++ 的桌面开发”工作负载" }

$VsDevCmd = Join-Path $vsPath "Common7\Tools\VsDevCmd.bat"
if (-not (Test-Path -LiteralPath $VsDevCmd)) { Fail "找不到 $VsDevCmd" }
Write-Step "Visual Studio: $vsPath"

# ---------------------------------------------------------------------------
# 2. 找到 CMake / Ninja（优先用 Qt 自带的，避免 PATH 上没有）
# ---------------------------------------------------------------------------
$cmake = Join-Path (Split-Path -Parent $QtDir) "Tools\CMake_64\bin\cmake.exe"
if (-not (Test-Path -LiteralPath $cmake)) { $cmake = "cmake" }   # 退回 PATH 里的 cmake
$ninja = Join-Path (Split-Path -Parent $QtDir) "Tools\Ninja\ninja.exe"
if (-not (Test-Path -LiteralPath $ninja)) { $ninja = "ninja" }

$QtConfig = Join-Path $QtDir "lib\cmake\Qt6"
if (-not (Test-Path -LiteralPath $QtConfig)) { Fail "QtDir 下找不到 Qt6 的 CMake 配置：$QtConfig（请确认 $QtDir 指向 msvc2022_64 这类套件目录）" }
Write-Step "Qt: $QtDir"
Write-Step "CMake: $cmake"

# ---------------------------------------------------------------------------
# 3. 可选：清理 build 目录
# ---------------------------------------------------------------------------
if ($Clean -and (Test-Path -LiteralPath $Build)) {
    Write-Step "清理 $Build"
    Remove-Item -LiteralPath $Build -Recurse -Force
}

# ---------------------------------------------------------------------------
# 4. 在 VsDevCmd 环境里配置 + 编译
#    这里把 cmake 命令串成一行交给 cmd.exe，保证编译器/cl/rc.exe/mt.exe 都在 PATH 上。
# ---------------------------------------------------------------------------
$configureCmd = '"{0}" -S "{1}" -B "{2}" -G Ninja -DCMAKE_BUILD_TYPE={3} -DCMAKE_PREFIX_PATH="{4}"' -f `
    $cmake, $Root, $Build, $Config, ($QtDir -replace '\\', '/')
$buildCmd     = '"{0}" --build "{1}"' -f $cmake, $Build

Write-Step "配置：$Config"
& cmd.exe /c ('call "{0}" -arch=x64 -no_logo >nul && {1}' -f $VsDevCmd, $configureCmd)
if ($LASTEXITCODE -ne 0) { Fail "CMake 配置失败" }

Write-Step "编译"
& cmd.exe /c ('call "{0}" -arch=x64 -no_logo >nul && {1}' -f $VsDevCmd, $buildCmd)
if ($LASTEXITCODE -ne 0) { Fail "编译失败" }

$Exe = Join-Path $Build "Cleveil.exe"
if (-not (Test-Path -LiteralPath $Exe)) { Fail "没有生成 $Exe" }

# ---------------------------------------------------------------------------
# 5. 部署 Qt 运行库（windeployqt）
# ---------------------------------------------------------------------------
if (-not $NoDeploy) {
    $windeployqt = Join-Path $QtDir "bin\windeployqt.exe"
    if (-not (Test-Path -LiteralPath $windeployqt)) { Fail "找不到 windeployqt：$windeployqt" }
    Write-Step "部署 Qt 运行库"
    & $windeployqt --release --no-translations --no-system-d3d-compiler --no-opengl-sw `
        --compiler-runtime $Exe
    if ($LASTEXITCODE -ne 0) { Write-Host "[build] windeployqt 返回 $LASTEXITCODE（通常不影响运行）" -ForegroundColor Yellow }
}

Write-Step "完成：$Exe"
Write-Host ""
Write-Host "运行：& `"$Exe`"" -ForegroundColor Green
