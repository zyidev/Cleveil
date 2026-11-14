// ============================================================================
//  signature.cpp —— Authenticode 验签实现
// ----------------------------------------------------------------------------
//  关键 API：
//    WinVerifyTrust  —— 一次调用完成"摘要比对 + 证书链校验"，是微软推荐入口
//    CryptQueryObject / CryptMsgGetParam / CertFindCertificateInStore
//                    —— 从文件里把 PKCS#7 签名结构取出来，定位签名者证书
// ============================================================================

#include "core/signature.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QString>

namespace un {

namespace {

// 从签名结构里取出"签名者证书"，填充签名者/签发机构名称
void fillSignerInfo(const QString &filePath, SignatureInfo *info)
{
    const std::wstring wide = filePath.toStdWString();

    DWORD encoding = 0, contentType = 0, formatType = 0;
    HCERTSTORE store = nullptr;
    HCRYPTMSG  msg   = nullptr;

    // 只取"内嵌在文件里的 PKCS#7 签名"，不联网、不猜
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, wide.c_str(),
                          CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                          CERT_QUERY_FORMAT_FLAG_BINARY, 0,
                          &encoding, &contentType, &formatType,
                          &store, &msg, nullptr)) {
        info->detail = QStringLiteral("无法从文件中解析签名结构（CryptQueryObject 失败）");
        return;
    }

    // ---- 取签名者信息（Issuer + 序列号就够唯一定位证书）----
    DWORD signerSize = 0;
    if (!CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &signerSize) || signerSize == 0) {
        if (msg) CryptMsgClose(msg);
        if (store) CertCloseStore(store, 0);
        return;
    }

    CMSG_SIGNER_INFO *signerInfo =
        reinterpret_cast<CMSG_SIGNER_INFO *>(LocalAlloc(LPTR, signerSize));
    if (signerInfo) {
        if (CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, signerInfo, &signerSize)) {
            CERT_INFO certInfo = {};
            certInfo.Issuer       = signerInfo->Issuer;
            certInfo.SerialNumber = signerInfo->SerialNumber;

            PCCERT_CONTEXT cert = CertFindCertificateInStore(
                store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                CERT_FIND_SUBJECT_CERT, &certInfo, nullptr);

            if (cert) {
                wchar_t nameBuf[512] = {};
                CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, nameBuf, 512);
                info->signer = QString::fromWCharArray(nameBuf);

                wchar_t issuerBuf[512] = {};
                CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE,
                                   1 /*CERT_NAME_ISSUER_FLAG*/, nullptr, issuerBuf, 512);
                info->issuer = QString::fromWCharArray(issuerBuf);

                CertFreeCertificateContext(cert);
            }
        }
        LocalFree(signerInfo);
    }

    if (msg)   CryptMsgClose(msg);
    if (store) CertCloseStore(store, 0);
}

} // namespace

// ----------------------------------------------------------------------------

QString signatureErrorText(long hresult)
{
    const DWORD code = static_cast<DWORD>(hresult);
    switch (code) {
    case 0:
        return QStringLiteral("签名有效，证书受信任");
    case TRUST_E_NOSIGNATURE:            // 0x800B0100
        return QStringLiteral("文件未签名（或签名格式 Windows 不认识）");
    case TRUST_E_SUBJECT_FORM_UNKNOWN:   // 0x800B0003
        return QStringLiteral("签名主体类型不受支持");
    case TRUST_E_PROVIDER_UNKNOWN:       // 0x800B0001
        return QStringLiteral("系统找不到能处理该签名的验证器");
    case TRUST_E_EXPLICIT_DISTRUST:      // 0x800B0111
        return QStringLiteral("签名已被系统标记为“明确不信任”");
    case TRUST_E_SUBJECT_NOT_TRUSTED:    // 0x800B0004
        return QStringLiteral("签名主体未被信任");
    case CERT_E_UNTRUSTEDROOT:           // 0x800B0109
        return QStringLiteral("证书链的根不受信任（自签名或私有 CA 签发的证书）");
    case CERT_E_EXPIRED:                 // 0x800B0101
        return QStringLiteral("签名证书已过期，且没有可用时间戳证明签名时证书有效");
    case CERT_E_REVOKED:                 // 0x800B010C
        return QStringLiteral("签名证书已被吊销");
    case CERT_E_CHAINING:                // 0x800B010A
        return QStringLiteral("证书链构建失败");
    case TRUST_E_BAD_DIGEST:             // 0x80096010
        return QStringLiteral("文件内容与签名摘要不一致（文件被修改过）");
    default:
        return QStringLiteral("验证失败，错误码 0x%1").arg(code, 8, 16, QLatin1Char('0'));
    }
}

QString currentExePath()
{
    return QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
}

SignatureInfo verifySignature(const QString &filePath)
{
    SignatureInfo info;

    if (filePath.isEmpty() || !QFile::exists(filePath)) {
        info.statusText = QStringLiteral("文件不存在，无法校验签名");
        return info;
    }

    const std::wstring wide = filePath.toStdWString();

    WINTRUST_FILE_INFO fileInfo = {};
    fileInfo.cbStruct       = sizeof(WINTRUST_FILE_INFO);
    fileInfo.pcwszFilePath  = wide.c_str();

    WINTRUST_DATA trustData = {};
    trustData.cbStruct            = sizeof(WINTRUST_DATA);
    trustData.dwUIChoice          = WTD_UI_NONE;              // 不弹 UI
    trustData.fdwRevocationChecks = WTD_REVOKE_NONE;          // 离线环境不查 CRL，避免卡住
    trustData.dwUnionChoice       = WTD_CHOICE_FILE;
    trustData.pFile               = &fileInfo;
    trustData.dwStateAction       = WTD_STATEACTION_VERIFY;
    trustData.dwProvFlags         = WTD_CACHE_ONLY_URL_RETRIEVAL;

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const long result = static_cast<long>(WinVerifyTrust(nullptr, &action, &trustData));

    // 无论结果如何都要释放状态，否则 Wintrust 会一直持有文件句柄
    trustData.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &trustData);

    info.statusText = signatureErrorText(result);

    if (result == 0) {
        info.hasSignature = true;
        info.trusted      = true;
        fillSignerInfo(filePath, &info);
        if (!info.signer.isEmpty()) {
            info.statusText = QStringLiteral("签名有效，签名者：%1").arg(info.signer);
        }
        return info;
    }

    // 走到这里说明要么没签名，要么签名有问题。再问一次"文件里到底有没有签名结构"，
    // 用来区分"完全未签名"和"签了但证书不受信任"。
    DWORD encoding = 0, contentType = 0, formatType = 0;
    HCERTSTORE store = nullptr;
    HCRYPTMSG  msg   = nullptr;
    const bool hasPkcs7 =
        CryptQueryObject(CERT_QUERY_OBJECT_FILE, wide.c_str(),
                         CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                         CERT_QUERY_FORMAT_FLAG_BINARY, 0,
                         &encoding, &contentType, &formatType, &store, &msg, nullptr) != FALSE;
    if (msg)   CryptMsgClose(msg);
    if (store) CertCloseStore(store, 0);

    info.hasSignature = hasPkcs7;
    if (hasPkcs7) {
        fillSignerInfo(filePath, &info);
        info.untrustedCert = (static_cast<DWORD>(result) == CERT_E_UNTRUSTEDROOT);
        if (info.untrustedCert) {
            info.statusText = QStringLiteral("已签名但证书不受信任（自签名/测试证书）%1")
                                  .arg(info.signer.isEmpty() ? QString()
                                                             : QStringLiteral("，签名者：%1").arg(info.signer));
        }
    } else {
        info.statusText = QStringLiteral("未签名（Windows 无法验证发布者）");
    }

    return info;
}

} // namespace un
