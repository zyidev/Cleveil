<#
============================================================================
 sign.ps1 —— Cleveil 的 Authenticode 数字签名脚本
============================================================================
 为什么需要签名：
   · 未签名的 exe 在 Windows 上会触发 "未知发布者" UAC 提示；
   · 从网络下载的 exe 会被 SmartScreen 拦截（"Windows 已保护你的电脑"）；
   · 部分企业策略直接禁止运行未签名程序。

 三种用法（按场景选一种）：

   1) 正式证书（PFX 文件，OV/EV 证书导出后就是这种）
        .\scripts\sign.ps1 -Pfx D:\certs\cleveil.pfx -Password "你的密码"

   2) 证书已装在证书存储里（例如 EV 证书配的 USB Key 已导入）
        .\scripts\sign.ps1 -Thumbprint 3A1B2C...（证书指纹）

   3) 本地测试：自动生成一张自签名代码签名证书并签上去
        .\scripts\sign.ps1 -SelfSigned
      注意：自签名只能证明"文件未被篡改"，无法让 SmartScreen 放行，
            正式分发必须用 1) 或 2)。

 只验签不签名：
        .\scripts\sign.ps1 -Verify
============================================================================
#>
param(
    [string]$Exe        = "",
    [string]$Pfx        = "",
    [string]$Password   = "",
    [string]$Thumbprint = "",
    [switch]$SelfSigned,
    [switch]$Verify,
    [string]$Timestamp  = "http://timestamp.digicert.com"
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if ([string]::IsNullOrWhiteSpace($Exe)) {
    $Exe = Join-Path $Root "build\Cleveil.exe"
}

# ---------------------------------------------------------------------------
# 1. 找到 signtool.exe（随 Windows SDK 安装，路径里带版本号，取最新的一份）
# ---------------------------------------------------------------------------
function FindSignTool {
    $roots = @(
        "C:\Program Files (x86)\Windows Kits\10\bin",
        "C:\Program Files\Windows Kits\10\bin"
    )
    $found = @()
    foreach ($root in $roots) {
        if (Test-Path -LiteralPath $root) {
            $found += Get-ChildItem -LiteralPath $root -Recurse -Filter "signtool.exe" -ErrorAction SilentlyContinue |
                      Where-Object { $_.FullName -match "\\x64\\signtool\.exe$" }
        }
    }
    if ($found.Count -eq 0) { throw "没有找到 signtool.exe，请安装 Windows SDK（签名工具随 SDK 一起装）" }
    # 目录名是 10.0.xxxxx.0，按版本号排序取最大
    return ($found | Sort-Object FullName -Descending | Select-Object -First 1).FullName
}

if (-not (Test-Path -LiteralPath $Exe)) { throw "找不到要签名的文件：$Exe" }
$signTool = FindSignTool
Write-Host "[1/4] signtool = $signTool"

# ---------------------------------------------------------------------------
# 2. 只验签模式
# ---------------------------------------------------------------------------
if ($Verify) {
    Write-Host "[2/4] 验签：$Exe"
    & $signTool "verify" "/pa" "/v" $Exe
    exit $LASTEXITCODE
}

# ---------------------------------------------------------------------------
# 3. 决定用哪张证书
# ---------------------------------------------------------------------------
if ($SelfSigned) {
    Write-Host "[2/4] 生成自签名代码签名证书（仅本机测试用）…"
    # 证书用途必须包含 Code Signing（1.3.6.1.5.5.7.3.3），否则 signtool 会拒绝
    $cert = New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject "CN=Cleveil Test (Self-Signed)" `
        -CertStoreLocation "Cert:\CurrentUser\My" `
        -KeyUsage DigitalSignature `
        -KeyExportPolicy Exportable `
        -NotAfter (Get-Date).AddYears(2) `
        -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3")
    $Thumbprint = $cert.Thumbprint
    Write-Host "      已创建证书：$($cert.Subject)，指纹 $Thumbprint"
}

if ($Thumbprint -ne "") {
    Write-Host "[3/4] 使用证书存储里的证书（指纹 $Thumbprint）"
    & $signTool "sign" "/sha1" $Thumbprint "/fd" "sha256" "/tr" $Timestamp "/td" "sha256" $Exe
}
elseif ($Pfx -ne "") {
    if (-not (Test-Path -LiteralPath $Pfx)) { throw "找不到 PFX 文件：$Pfx" }
    Write-Host "[3/4] 使用 PFX 证书：$Pfx"
    & $signTool "sign" "/f" $Pfx "/p" $Password "/fd" "sha256" "/tr" $Timestamp "/td" "sha256" $Exe
}
else {
    throw "请指定 -Pfx / -Thumbprint / -SelfSigned 之一，或用 -Verify 只验签"
}

if ($LASTEXITCODE -ne 0) { throw "签名失败，signtool 退出码 $LASTEXITCODE" }

# ---------------------------------------------------------------------------
# 4. 立刻验签，确认签名真的落到了文件上
# ---------------------------------------------------------------------------
Write-Host "[4/4] 验签：$Exe"
# 把验签输出抓下来，方便判断失败原因（不是所有失败都算脚本失败）
$verifyOut  = & $signTool "verify" "/pa" "/v" $Exe 2>&1
$verifyCode = $LASTEXITCODE
$verifyText = (($verifyOut | Out-String) -replace "\s+", " ")   # 折叠换行，便于匹配整句错误
$verifyOut | Select-Object -Last 10 | Write-Host

if ($verifyCode -ne 0) {
    # 自签名证书的链必然"终止于不受信任的根"，这属于预期结果，只警告不报错
    if ($verifyText -match "root certificate" -or $verifyText -match "根证书" -or
        $verifyText -match "CERT_E_UNTRUSTEDROOT") {
        Write-Warning "验签未通过：证书链终止于不受信任的根（自签名/测试证书的预期现象）"
    } else {
        throw "验签失败：签名虽然写入了文件，但校验没有通过（$($verifyText.Trim() -split "`n" | Select-Object -Last 1)）"
    }
}

Write-Host ""
Write-Host "签名流程结束。可用以下命令查看签名状态："
Write-Host "  Get-AuthenticodeSignature -LiteralPath `"$Exe`" | Format-List"
exit 0
