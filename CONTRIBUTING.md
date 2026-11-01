# 开发与验证

核心应用使用 Python 标准库和原生 HTML/CSS/JavaScript。离线测试不需要账号、网络或平台连接器。

```powershell
python -m unittest discover -s tests -v
node --check web/app.js
```

连接器回归测试在当前 Python 环境未安装对应依赖时会跳过。CI 检查离线逻辑，不能代替真实账号联调或 Windows 10 实机验收。

## 提交范围

只提交应用源码、脚本、测试、示例和文档。不要提交 `.codex/`、`.deps/`、`data/`、`.zhs/`、`config.local.json`、密码、Cookie 或包含个人课表的导出文件。推送前检查 `git diff --cached`。

`dist/` 和 `build/` 不纳入源码管理；便携 ZIP 应在验证后通过 Release 等发布渠道分发。不要分发使用后的运行目录。

## 当前待办

- 完成学习通和智慧树真实账号的只读同步联调，验证登录失效、部分失败和考试域 SSO。
- 验证连接器安装流程，并使用完整连接器环境执行被跳过的回归测试。
- 用最新源码重新生成便携包，在干净的 Windows 10 / 11 环境验收。
- 核对手机日历客户端的订阅刷新、完成任务移除和 HTTPS 兼容性。
- 确定 Windows 桌面界面方案；目前是本地网页应用，尚未迁移到 WPF 或 WinUI。

系统通知、托盘、开机启动、固定公网日历地址以及智慧树 AI 课 / 翻转课属于后续可选扩展。
