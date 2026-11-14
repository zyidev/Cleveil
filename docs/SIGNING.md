# Cleveil 数字签名说明

> 本文覆盖三件事：**exe 的 Authenticode 签名怎么做**、**证书有哪些选择**、**内核驱动为什么需要另一套签名流程**。

---

## 1. 为什么必须签名

| 场景 | 未签名的后果 |
| --- | --- |
| 用户双击安装/运行 | UAC 弹窗显示"未知发布者"，大量用户直接取消 |
| 从浏览器/网盘下载 | SmartScreen 提示"Windows 已保护你的电脑"，需要点"仍要运行" |
| 企业终端管控 | 很多策略（AppLocker、WDAC）默认只允许签名程序运行 |
| 驱动加载 | 未签名驱动在 64 位 Windows 上**根本加载不了**（KMCS 强制要求） |

签名同时解决两件事：**完整性**（文件被改一个字节，签名就失效）和**身份**（证书链能追到受信任的 CA）。

---

## 2. 签名方案对照

| 方案 | 适用 | 成本 | SmartScreen | 说明 |
| --- | --- | --- | --- | --- |
| 自签名证书 | 本机测试、内网分发 | 免费 | **不放行**，仍会拦截 | 只证明"文件没被改过"，没人给它背书 |
| OV（组织验证）代码签名证书 | 面向公众分发 | 需向 CA 购买，按年续费 | 需要累积下载声誉后才不拦 | 私钥可以是文件（PFX），也可以是 USB Key |
| EV（扩展验证）代码签名证书 | 正式商业分发 | 高于 OV，需企业资质审核 | **签发即可获得较好声誉**，云端签名服务可即时生效 | 私钥必须在硬件令牌/HSM 中，无法导出成 PFX |
| 微软商店/MSIX 发布 | 走商店渠道 | 开发者账号 | 由商店负责 | 与本项目 exe 分发方式不同 |

> 具体价格与审核材料以证书颁发机构（DigiCert / Sectigo / GlobalSign / Certum 等）当期要求为准，本文不给出报价。

---

## 3. 给 Cleveil.exe 签名

脚本：`scripts/sign.ps1`（自动在 Windows SDK 里找 `signtool.exe`，取版本号最高的那份）。

### 3.1 自签名（本地测试）

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\sign.ps1 -SelfSigned
```

脚本会：

1. 用 `New-SelfSignedCertificate -Type CodeSigningCert` 在当前用户证书库建一张代码签名证书
   （扩展密钥用法必须含 `1.3.6.1.5.5.7.3.3` Code Signing，否则 `signtool` 直接拒绝）；
2. 调用 `signtool sign /sha1 <指纹> /fd sha256 /tr <时间戳> /td sha256`；
3. 自动 `signtool verify /pa` 复核。

预期结果：签名成功，但验签会报 `CERT_E_UNTRUSTEDROOT`（根证书不受信任）——**这是正常的**，
它证明哈希层没问题，只是缺一张公开 CA 的背书。Cleveil 的"关于"窗口会把这种情况显示为
"已签名但证书不受信任（自签名/测试证书）"。

### 3.2 正式证书（PFX）

```powershell
.\scripts\sign.ps1 -Pfx D:\certs\cleveil.pfx -Password "你的密码"
```

### 3.3 正式证书（证书库/硬件令牌）

```powershell
# 先确认证书已导入当前用户证书库
Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert | Format-List Subject, Thumbprint
# 用指纹签名（EV 证书用硬件令牌时，签名过程会要求输入令牌 PIN）
.\scripts\sign.ps1 -Thumbprint <证书指纹>
```

### 3.4 只验签

```powershell
.\scripts\sign.ps1 -Verify
Get-AuthenticodeSignature -LiteralPath .\build\Cleveil.exe | Format-List
```

---

## 4. 关键参数为什么这么选

| 参数 | 取值 | 原因 |
| --- | --- | --- |
| `/fd sha256` | 文件摘要算法 | SHA-1 已被浏览器与 Windows 逐步淘汰 |
| `/td sha256` | 时间戳摘要算法 | 必须与 `/tr` 配合，否则时间戳不生效 |
| `/tr http://timestamp.digicert.com` | RFC3161 时间戳 | 有可信时间戳，证书过期后签名依然有效 |
| `/pa` | 验签策略 | 使用"默认 Authenticode 策略"验，与资源管理器一致 |

---

## 5. 签名后的自检

1. `signtool verify /pa /v build\Cleveil.exe` → 输出 `Successfully verified`；
2. 资源管理器 → 右键 exe → 属性 → **数字签名**页签能看到签名者；
3. 运行 Cleveil → 帮助 → 关于 → "本程序签名状态"应显示"签名有效，签名者：xxx"
   （这一栏由 `src/core/signature.cpp` 里的 `WinVerifyTrust` 实时算出来，不是写死的文案）。

---

## 6. 内核驱动的签名（与 exe 完全不同的一套）

本项目"安装监控"默认走**用户态注册表通知通道**，不需要驱动、不需要签名。
如果你要把它升级成真正的内核驱动（注册表回调 / 进程回调），需要注意：

| 事项 | 要求 |
| --- | --- |
| 编译环境 | Visual Studio + **WDK**（Windows Driver Kit），目标 `Windows Driver, Universal` |
| 测试阶段 | 打开测试签名：`bcdedit /set testsigning on` 后**重启**；用 `signtool sign /fd sha256 /a /ac <MSCV-VI-CrossCert> driver.sys` 签测试签名 |
| 正式分发 | 必须过 **微软硬件开发中心（Partner Center）的 attestation signing**，或 WHQL 认证；本地 CA 签的驱动在普通用户机器上加载不了 |
| 风险 | 驱动崩溃 = 蓝屏。必须先在虚拟机/测试机上长期跑，且用 `Driver Verifier` 压测 |
| 代码签名证书与驱动签名 | **不是同一回事**：exe 用 Authenticode（本文 1~4 节），`.sys` 用内核签名 + 微软提交 |

因此本项目采取的路线是：**先把"能不能第一时间知道安装发生"这件事用零成本、零风险的方式做对**
（`RegNotifyChangeKeyValue` 由内核在键被写入时主动唤醒监控线程，延迟在数百毫秒级），
同时把驱动接口留好：一旦系统里出现设备 `\\.\CleveilMon`，监控窗口状态栏会自动显示"内核驱动通道"。
驱动侧只需要按 `src/core/installmonitor.h` 里的 `CleveilDriverEvent` 结构往设备写事件即可，C++ 侧不用改代码。

---

## 7. 常见问题

| 现象 | 原因 / 处理 |
| --- | --- |
| `SignTool Error: No certificates were found that met all the given criteria` | 指纹不对，或证书不在 `CurrentUser\My`；用 `Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert` 确认 |
| 验签报 `CERT_E_UNTRUSTEDROOT` | 自签名证书的正常现象；正式分发要换公开 CA 证书 |
| 验签报 `TRUST_E_BAD_DIGEST` | 文件签名后被改动过（例如签名后又重新链接了 exe）→ 重新签一次 |
| 时间戳失败 `The timestamp signature and/or certificate could not be verified` | 换一个时间戳服务，或检查网络/代理；没时间戳时签名在证书过期后会失效 |
| SmartScreen 仍然拦截 OV 签名的程序 | OV 无即时声誉，需要随下载量累积；急用请换 EV |
| 签名后再 `cmake --build` | 链接会覆盖 exe，**签名必须放在构建之后**，CI 里要按"构建 → 签名 → 打包"的顺序 |
