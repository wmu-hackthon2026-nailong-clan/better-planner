# Better Planner

面向学生的 **Windows 桌面日历应用**（原生 exe，无需浏览器）：把学习通的作业、智慧树的考试、教务系统的课表汇集到同一条时间轴上，用液态玻璃质感的日程块与 deadline 亮线呈现一天的节奏，支持日 / 周 / 月三种视图。

## 为什么做它

学生的 deadlines 散落各处：学习通的作业、智慧树的考试、教务的课表、群里随口通知的截止日期。Better Planner 做两件事：

1. **汇集** —— 通过只读连接器子进程从各平台拉取任务与课表，统一映射为事件入库，重复同步自动去重，本地完成状态不被上游覆盖；
2. **呈现** —— 原生 Win32 + Direct2D 窗口，日程按实际时长占位为液态玻璃块，deadline 以亮线钉在时刻上，重叠自动右移嵌套，同刻合并，一眼看清"什么时候忙、什么时候死线"。

## 核心特性

- **原生桌面应用**：C++ 编译为单个 `better_planner.exe`，冷启动快、离线可用；数据保存在本地 SQLite。
- **三种视图**：日视图（完整时间轴）→ 周视图（七列压缩）→ 月视图（每日摘要），侧栏一键切换，另有「今天」快速回位。
- **液态玻璃视觉**：半透明磨砂 + 背景模糊 + 边缘高光；deadline 亮线在撞线时同侧堆叠。
- **平台同步**：学习通（作业 / 考试 / 课表）、智慧树（作业 / 考试）、智慧树 AI 课程（考试 / 自测）。侧栏「连接与同步」打开本机账号页：学习通账密登录、智慧树扫码登录，之后一键同步。
- **课表导入**：教务系统课表导出为 JSON 后一键导入（配套浏览器书签工具见 `tools/`）；自动解析周次区间与单双周标记（如 `4-6周(双)`）。
- **学期周次对齐**：`config.local.json` 的 `semester_start` 指定学期第一周周一的日期，课表按真实周次展开，不错位。
- **导出长图**：把当前时间轴导出为长图，方便分享与汇报。
- **凭据安全**：登录凭据只通过环境变量注入连接器子进程，不写命令行、不落盘、不转发 Cookie。

## 架构

C++ 三层，每层职责单一、可独立编译测试；同步服务复用少量只读 Python 脚本：

```
src/
├── core/    数据层：事件模型、SQLite 存储、时间线布局引擎（纯计算，零 I/O 零图形依赖）
├── api/     接口层：自带 JSON 解析器（零第三方依赖）、本机 HTTP 服务、
│            平台同步桥接（连接器子进程 + 管道捕获）、课表导入（周次 / 单双周解析）
└── ui/      表现层：Win32 窗口 + Direct2D 液态玻璃渲染、日 / 周 / 月视图

app/         Python 本机登录服务与连接器调度（凭据只进环境变量）
scripts/     构建打包脚本 + 学习通 / 智慧树 / 智慧树AI 只读连接器
web/         「连接与同步」本机账号页（桌面应用内嵌 WebView 打开）
tools/       课表导入书签工具
```

关键约束：

- 时间统一为 Unix epoch 秒（UTC），区间左闭右开 `[start, end)`；
- 布局引擎输出**归一化比例**而非像素，窗口缩放不必重算结构；
- 全部视觉参数集中在样式结构中，渲染器只读不改。

## 构建

要求：MSVC（Build Tools 或 Visual Studio）+ Windows SDK 10.0.26100 + CMake。

```powershell
# 若 vcvars64.bat 不在默认位置，先设置环境变量 VCVARS64 指向它
.\scripts\build.ps1        # vcvars64 + CMake(NMake) + ctest 一条龙
```

或直接使用 CMake：

```cmd
cmake -B build -S .
cmake --build build
```

产物为 `build\better_planner.exe`（Win32 GUI 程序）。

## 测试

```cmd
ctest --test-dir build --output-on-failure    # C++ 布局与接口层测试
python -m unittest discover -s tests -v        # 同步服务与连接器测试
```

CI（`.github/workflows/checks.yml`）在每次 push 时对 Python 3.11 / 3.14 运行离线测试（不携带任何平台凭据）。

## 配置

首次使用时复制 `config.example.json` 为 `config.local.json`：

| 字段 | 说明 |
| --- | --- |
| `xxt_command` / `xxt_cwd` | 学习通连接器命令与工作目录 |
| `zhs_python` | 智慧树连接器所用 Python |
| `desktop_python` | 桌面同步服务所用 Python |
| `sync_interval_minutes` | 自动同步间隔（默认 30 分钟） |
| `semester_start` | **学期第一周周一的日期**（如 `2026-09-07`），决定课表周次与单双周对齐 |

便携版已内置运行时与连接器，开箱即用，无需手工配置。

## 打包分发

```powershell
python scripts/setup_desktop.py            # 准备嵌入式 Python 运行时与连接器依赖（一次性）
python scripts/build_desktop_portable.py   # 产出 dist\Better-Planner-Windows-x64.zip
```

便携包内含：`better_planner.exe`、嵌入式 Python 运行时、只读连接器、`Start-BetterPlanner.bat` 启动脚本、使用说明、连接器源码归档与许可证（`sources/`、`THIRD_PARTY_NOTICES.txt`）。解压即用，无需安装；附带 `.sha256` 校验文件。

## 第三方依赖与许可

- `zhs`（智慧树连接器）：**GPL-3.0-only**，pjm314159
- `xuexitong-cli`（学习通连接器）：MIT
- Python 3.14.0：Python Software Foundation License
- Microsoft Visual C++ Runtime：Microsoft Software License Terms

## 目录结构

```
better-planner/
├── CMakeLists.txt          CMake 构建
├── config.example.json     配置模板（复制为 config.local.json）
├── src/                    C++ 核心（core / api / ui 三层）
├── app/                    Python 同步服务与连接器调度
├── scripts/                构建打包脚本、平台只读连接器
├── web/                    「连接与同步」本机账号页
├── tools/                  课表导入书签工具
└── tests/                  C++（test_layout / test_api_http）与 Python 测试
```
