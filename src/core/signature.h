#pragma once

// ============================================================================
//  signature.h —— 数字签名（Authenticode）校验
// ----------------------------------------------------------------------------
//  校验分两层：
//    1) 哈希层：文件的 PE 镜像摘要是否被改动过（WinVerifyTrust 内部完成）；
//    2) 证书层：签名证书能否链到受信任根、是否过期/被吊销。
//  两者都通过才算"受信任的正式签名"。自签名证书（测试用）会走到
//  CERT_E_UNTRUSTEDROOT，此时哈希是好的、只是没人能给这张证书担保。
// ============================================================================

#include <QString>

namespace un {

struct SignatureInfo {
    bool    hasSignature   = false;   // 文件里是否真的有 Authenticode 签名
    bool    trusted        = false;   // 证书链验证通过（正式分发要求这一项）
    bool    untrustedCert  = false;   // 有签名但证书不受信任（典型：自签名测试证书）
    QString signer;                   // 签名者显示名（证书 Subject 的 CN）
    QString issuer;                   // 签发机构
    QString statusText;               // 一句话结论，直接给界面显示
    QString detail;                   // 失败/异常时的技术细节
};

// 校验任意 exe/dll 的签名状态
SignatureInfo verifySignature(const QString &filePath);

// 当前进程自己的 exe 路径（"关于"窗口用它显示自身签名状态）
QString currentExePath();

// 把 HRESULT 翻译成中文说明（供界面与日志复用）
QString signatureErrorText(long hresult);

} // namespace un
