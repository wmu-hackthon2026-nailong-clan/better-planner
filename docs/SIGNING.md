# Windows 代码签名

2026-11-02 核对。每次构建产生的新 EXE/DLL 都需要签名；签名后再改文件会使签名失效。证书在有效期内可用于多个版本，不需要每次购买。

## 如何取得证书

- 个人开发者可向支持个人身份验证的公开 CA 申请代码签名证书，例如 [SSL.com IV Code Signing](https://www.ssl.com/products/software-integrity/code-signing/)。先确认申请地区、实名资料、密钥托管方式与完整费用。
- 组织可申请 OV/EV 代码签名；例如 [DigiCert 申请流程](https://docs.digicert.com/en/certcentral/order-and-manage-certificates/request-certificates/request-a-code-signing-or-ev-code-signing-certificate/request-code-signing-certificate.html)。完成身份审核后，按服务商要求使用 USB Token、HSM 或云签名。
- [Azure Artifact Signing](https://learn.microsoft.com/en-us/azure/artifact-signing/quickstart) 的公开信任证书有地区限制：个人目前限美国/加拿大，中国大陆个人不在支持范围。组织范围按该官方页面为准。
- 经 Microsoft Store 分发 MSIX，商店审核后代为签名；普通 EXE/MSI 商店分发仍需要发布者自行签名。见 [Microsoft 官方选项对比](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/code-signing-options)。

不要把“自签名测试证书”当作公开受信任签名；它不能直接满足 Smart App Control 的公开信任要求。数字签名也不保证任意企业自定义允许列表都会放行。没有为了本项目关闭或修改系统安全策略。

## 签署已编译的程序

先按 CA 说明安装证书及其私钥访问组件。对于 Windows 个人证书存储区中的证书：

```powershell
.\scripts\sign-release.ps1 -CertificateThumbprint '<证书的40位SHA1指纹>' -TimestampUrl '<CA提供的RFC3161时间戳地址>'
```

脚本调用 Windows SDK 的 SignTool，使用 SHA-256 文件摘要、RFC 3161 时间戳，并执行 `verify /pa /all /v`。它不会创建自签名证书、修改信任存储区或购买证书。云签名服务请使用供应商插件/命令，不能假定所有云证书都能通过 `/sha1` 访问。

构建流水线顺序：编译 → 对最终 EXE/DLL/安装包签名并加时间戳 → 验证 → 打包发布。购买/实名审核由证书持有人本人完成，密码和 Token PIN 不写入仓库或聊天。

参考：[Smart App Control 签名要求](https://learn.microsoft.com/en-us/windows/apps/develop/smart-app-control/code-signing-for-smart-app-control)、[SignTool 参数](https://learn.microsoft.com/en-us/windows/win32/seccrypto/signtool)。
