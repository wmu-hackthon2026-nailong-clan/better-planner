# 课期 Keqi

> 桌面主线：`build/better_planner.exe`。工具栏“连接与同步”打开本机账号页，支持学习通账号密码登录与智慧树扫码登录；同步后的日程自动回填桌面。首次安装连接环境运行 `python scripts/setup_desktop.py`。详见 [桌面登录同步](docs/DESKTOP_SYNC.md)、[液态玻璃实现](docs/LIQUID_GLASS.md)、[代码签名](docs/SIGNING.md)。下文保留原 Python 网页版说明。

本地运行的课表与截止日期汇总工具：将**学习通课表、学习通作业 / 考试、智慧树知到共享课作业 / 考试**汇总到同一个看板。

当前是 Python + 浏览器的本地应用，尚未迁移到 WPF / WinUI。开发检查、提交范围和后续任务见 [CONTRIBUTING.md](CONTRIBUTING.md)。离线测试通过不代表平台在线接口已验证。

## 给普通使用者：便携版，无需 Git / Python

将 `dist/Keqi-Windows-x64.zip` 发给使用者，**解压整个文件夹后双击 `Start-Keqi.bat`**。程序自带官方 Python 运行环境和两个连接器，自动打开浏览器；运行期间保留命令窗口。

点击页面右上角「连接账号」：学习通输入手机号和密码；智慧树点击「获取登录二维码」，使用知到 App 扫码确认。登录后点击「同步平台」。自动同步默认 30 分钟一次。

学习通默认只保留本次会话中的密码；勾选「记住账号密码」后使用 Windows DPAPI 加密，绑定当前 Windows 用户。平台 Cookie 与数据库保存在 `data/`。请分享原始 ZIP，**不要把使用后的文件夹直接发给别人**。

这是 Windows x64 便携包，不是安装向导，也没有代码签名。连接和同步依赖平台接口及账号状态，仍需实际账号验证；遇到平台验证码请在官方客户端完成验证后重试。

以下为源码运行与开发者安装说明。

核心应用使用 Python 标准库 + 原生 HTML/CSS/JS，无需 Node、前端构建或数据库安装。界面和数据服务均留在本机，不部署到公共网站。

## 启动

需要 Python 3.11+（智慧树连接器需要 Python 3.13 / 3.14）。在项目目录执行：

```powershell
python -m app.server
```

打开 <http://127.0.0.1:8765>。初次启动为空数据，可直接添加课程 / 任务，或在「数据连接」中导入 `examples/tasks.json` 查看流程。示例不会自动混入真实数据。

启用定时同步（进程运行期间生效）：

```powershell
python -m app.server --auto-sync
# 或
.\run.ps1
```

默认每 30 分钟同步一次，最短 15 分钟。也可在页面点击「同步平台」。后台查询可能耗时数分钟，不阻塞看板；两次同步不会重叠。端口被占用可用 `--port 8766`。

## 连接两个平台

连接器复用独立的开源项目，核心应用通过子进程读取 JSON，不复制上游代码。需要安装 Git，网络能访问 GitHub / PyPI。

```powershell
.\scripts\install-connectors.ps1
```

脚本会将仓库和各自 Python 虚拟环境放在 `.deps/`，生成 `config.local.json`；已有配置不会覆盖。安装使用执行时的上游版本，上游变更可能影响兼容性，安装后建议记录 `git -C .deps/xuexitong rev-parse HEAD` 与 `git -C .deps/zhs rev-parse HEAD` 以便复现。安装完成后仍需登录并验证实际同步结果。

### 学习通：课表、作业与考试

使用 [DanMo661/xuexitong-cli](https://github.com/DanMo661/xuexitong-cli)。在启动服务的 PowerShell 中设置环境变量（不要把真实账号填进仓库文件）：

```powershell
$env:CHAOXING_PHONE = Read-Host '学习通手机号'
$securePassword = Read-Host '学习通密码' -AsSecureString
$env:CHAOXING_PASSWORD = [System.Net.NetworkCredential]::new('', $securePassword).Password
.\.deps\xxt-env\Scripts\python.exe -m xuexitong_mcp.cli whoami --json
python -m app.server --auto-sync
```

账号会被子进程继承；也可以按照上游说明在 `.deps/xuexitong/config.json` 配置账号。登录态由上游保存在本地。

实际调用以下只读命令：

```text
xxt week --json
xxt hw --all-courses --json
xxt exam --json
```

作业使用 `--all-courses`，避免只看课表内课程而漏掉网课任务；上游该命令输出待交作业。课表当前同步的是**当前教学周和下一教学周**，历史周数据累计保留，未来周可用上游 `xxt week 12 --json` 导出后在页面按「课表」类型导入。页面切周本身不触发查询。考试的学期范围沿用上游默认行为。

手动安装时复制 `config.example.json` 为 `config.local.json`，`xxt_command` 填字符串数组，例如 `["C:/.../python.exe", "-m", "xuexitong_mcp.cli"]`，`xxt_cwd` 填上游仓库绝对路径。不要把整个命令写成一个字符串。

### 智慧树：知到共享课作业与考试

使用 [pjm314159/zhs](https://github.com/pjm314159/zhs) 的会话与查询接口。先运行：

```powershell
.\.deps\zhs-env\Scripts\zhs.exe init
.\.deps\zhs-env\Scripts\zhs.exe login
```

界面扫码后的 Cookie 保存在 `data/zhs/.zhs/`。上游当前版本的 `.zhs/` 相对于启动工作目录，并非固定用户主目录。命令行登录时请先进入 `data/zhs/` 再执行。`config.local.json` 中的 `zhs_python` 指向安装了该项目的 Python；省略则使用当前 Python。

本项目的 `scripts/zhs_readonly.py` 只调用课程列表、`getStudentHomework` 和 `exam_list`，不调用答题、开考、提交或刷课功能。作业读取未提交和已提交两组，按原始 `endDate` 汇总截止时间；考试读取未完成和已完成两组。

**当前覆盖范围：知到共享课。AI 智慧课、翻转课 / Hike 尚未自动接入**，请通过手动日程或规范 JSON 导入补充。上游知到考试代码对不同考试域 SSO 有待验证的假设，可能出现作业能读、考试失败的情况；看板会显示部分失败，不能将它当成“没有考试”。

## 已实现

- 学习总览：今日课程、待办数量、72 小时内截止、逾期任务。
- 周课表：按日期展示课程和考试，支持前后翻周。
- 截止清单：按截止时间排序，支持平台 / 状态筛选与课程关键词搜索；没有截止时间的任务单独显示“未知”，不猜日期。
- 手动补录课程、作业、考试，支持删除手动记录。
- 本地完成标记不会提交到平台；平台已完成任务无法通过本地勾选改成未交。
- 同步增量更新，重复导入去重，任务延期保留本地完成标记。
- 分别展示各查询最近尝试、最近完整成功、失败原因；失败保留已有数据。
- JSON 文件导入、ICS 日历导出（UTF-8 折行、UTC 时间，适用于 Outlook / Apple / Google 日历导入）。

## 数据规则与限制

所有无时区时间均按北京时间（Asia/Shanghai，UTC+8）解释。未知 / 无截止日期保留为空。带稳定 `external_id` 的任务改名、延期都会更新同一记录；没有 ID 的学习通列表使用课程、标题与开始时间生成标识，重名任务或考试改期开考时间可能需要人工确认。

同步采用增量合并，**不会因为任务暂时没有返回就自动删除或推断已完成**。学习通待交作业在提交后可能从上游列表消失，需要在本机标记完成；停课 / 调课后旧课程也可能需要人工核对。第一版不是平台全量镜像。

应用只有本机 HTTP 服务：监听 `127.0.0.1`，校验 Host，修改请求需要页面会话令牌；没有云端账号或公网访问。界面登录密码通过本机请求进入后台，再通过子进程 stdin 传入，不写入命令参数、日志或接口状态。可选的密码持久化使用 Windows DPAPI。数据库存于 `data/planner.sqlite3`。`.deps/`、`data/`、`config.local.json` 均被 Git 忽略。登录态仅供当前使用者，勿上传这些文件。

「导出日历」下载的 ICS 是文件快照；「手机日历订阅」提供持续更新的局域网 ICS 链接。作业与考试在实际截止日期的前一天显示为单日全天日程，详情保留真实截止时间；课程仍按上课起止时间显示。未实现操作系统推送通知，提醒目前体现在看板数字、排序和颜色中。

## 验证与开发

```powershell
python -m unittest discover -s tests -v
node --check web/app.js
```

测试覆盖统一时间、跨平台去重、延期更新、本地完成标记、失败缓存、ICS 以及 HTTP 增删改导出流程。**本次没有真实账号，未实测平台登录与在线数据同步；连接器须在你完成登录后联调。**

项目结构：

```text
app/core.py                  数据模型、SQLite、日历导出
app/connectors.py            子进程协议、平台同步与失败隔离
app/server.py                本机 HTTP API 与定时同步
scripts/zhs_readonly.py      智慧树只读查询桥接
scripts/install-connectors.ps1  安装独立连接器环境
web/                         看板界面，无构建依赖
tests/                       自动测试，不访问真实平台
```


### 手机日历订阅（一加 / ColorOS）

启动后，点击「手机日历订阅」，复制地址到手机日历的「URL 导入」。手机和电脑连接同一 Wi-Fi；电脑保持运行课期。默认 TCP 8768 只提供带随机密钥的只读日历，管理页面 8765 仍仅允许本机访问。密钥保存在 data/calendar-subscription.key，重启后保持不变；可在界面重置以撤销旧链接。

手机先用浏览器打开链接验证能下载 ICS。若无法访问，在便携包中双击 Enable-Calendar-LAN.bat 并确认管理员提示；规则只允许当前便携 Python 的 TCP 8768、来源为本地子网，不更改系统网络类别。学校 Wi-Fi 设备隔离、VPN 或地址变化也可能影响访问。局域网链接不是公网链接，外出时不会更新；HTTP 链接只应在可信局域网使用。此版本未提供公网托管、HTTPS 或 CalDAV。

订阅与导出包含已同步、未完成且有时间的课程、作业与考试。修改截止日期后使用同一 UID 返回最新日程；手机是否及时更新及如何处理被移除的已完成任务，取决于日历客户端。没有配置日历闹钟。更远周次的课程需要先同步后才会出现在日历中。


HTTPS 兼容性测试：如果手机浏览器能下载 ICS，但一加日历的「URL 导入」仍报网络不稳定，可在订阅窗口启用 HTTPS 临时链接。此功能使用官方 cloudflared，将 TCP 8768 的只读日历转发到 trycloudflare.com；不转发登录和管理页面。无需 Cloudflare 账号。临时地址在通道重建后变化，手机需重新添加，因此只作为兼容性测试与临时使用；长期自动更新需要固定域名的正式通道。电脑仍需运行课期，转发服务会处理日历请求。窗口可关闭通道或重置密钥使旧链接失效。

日历任务展示：按北京时间计算截止日期，作业、考试及其他任务仅在该日期的前一天显示为一个全天日程（DTEND 为不包含的次日），标题为「明日截止」。网页截止清单保留实际截止时间。
