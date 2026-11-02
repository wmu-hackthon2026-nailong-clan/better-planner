# Better Planner 开发计划（黑客松冲刺版）

> 依据 2026-11-01 19:52–19:56 群聊讨论（丁子健、张励志）整理，起草于 2026-11-02。
> 仓库：`wmu-hackthon2026-nailong-clan/better-planner`，按群里约定直接 push 到 main，提交保持小步。

## 1. 目标与定位

把现有 Python + 网页版「课期 Keqi」升级为 **Windows 桌面端日历应用 Better Planner**：

- C++ 开发，Windows 10 UI 架构（WinUI 3），支持 Windows 10 及以上；
- 液态玻璃（Liquid Glass，参考 iOS 26）视觉风格，日视图中的课程/日程以半透明磨砂玻璃块呈现；
- 日 / 周 / 月 三级视图，左上角折叠按钮循环切换，周视图可点击某天下钻到日视图；
- 对外接口允许提交三种信息（deadline、日程、第三种待定）；
- 加分项：应用内自动滚动截长图（补上 Win11 才自带、Win10 没有的能力）。

**演示优先于功能全量**：黑客松交付一个能跑、能看、能输入的精致桌面端 + 一套演示脚本。

## 2. 需求清单（提炼自群聊）

### P0 日视图：磨砂玻璃时间轴（丁子健的原始设计）

持续项目/课程以**半透明磨砂玻璃方框**占据空间展示，重叠规则：

| 情况 | 处理 |
| --- | --- |
| 时间完全相同的多个事件 | 合并为**同一个玻璃框**，内部用分隔符标记多个事件 |
| 一个事件的时间被另一个包含，或时间交叉 | **被包含/相交的项目右移**（错位排布，不硬叠） |
| 无重叠 | 玻璃框内**显示全部信息** |
| 有重叠 | 玻璃框内只显示**标题 3–5 个字符**，点开后查看完整内容 |

玻璃框细节：上下两侧用**高光、低透明度的横线**做隔离（亮边描出玻璃质感）。

### P0 三级视图切换（主界面）

- 左上角**折叠按钮**，每按一次循环：日进程 → 周进程 → 月进程；
- **日进程**：上述磨砂玻璃视图，信息完整；
- **周进程**：也显示时间，但信息更密集（压缩排版）；
- **月进程**：仅显示摘要；
- 点击**周进程中的某一天** → 进入该日的日视图。

### P0 外部接口：三种提交形式

| 类型 | 字段 | 备注 |
| --- | --- | --- |
| ① deadline | 时间点、标题、注释 | 单一时间点 |
| ② 日程 | 起始 + 终末、标题、注释 | 支持两种注册：`起始+持续时间` / `起始+终末时间` |
| ③ 待定 | —— | 丁子健原话"突然想不起来了，先空着"。**预留枚举位**，建议候选：周期性/重复事件、全天日程，确认后补 |

### P1 自动滚动截长图

- 群里明确要求：支持"缓慢滚动页面，或点出自动滚动开始截长图"；
- Win11 系统自带，**Win10 没有** → 我们自己做，就是差异化卖点；
- 技术路线：Windows.Graphics.Capture（Win10 1903+）连续截帧 + 程序化滚动（UI Automation / WM_VSCROLL）+ 重叠区域比对拼接。

### P2 平台同步（黑客松后移植）

学习通 / 智慧树只读同步已在 Python 版 Keqi 跑通（`app/connectors.py`，子进程读 JSON 的架构）。**黑客松期间 C++ 版不做平台联网**，用 JSON 导入 + 手动录入演示；赛后把连接器作为独立进程通过接口层接入。

### 工程约定（张励志拍板）

1. **C++ 开发**，UI 层 / 数据层 / 接口层**分离在不同文件夹**；
2. 写明 hpp / cpp 接口，**分别注册 class**，层与层只通过 **public 接口**互联；
3. **hpp 写清楚注释，cpp 不写注释**（注释即契约）；
4. 液态玻璃效果：参考 B 站视频（iOS 26 液态玻璃 React 实现）中的库，**只参考实现途径，不直接调用，允许重写实现**，作为界面风格落地。

## 3. 技术选型

| 决策 | 推荐 | 理由 / 备选 |
| --- | --- | --- |
| UI 框架 | **WinUI 3 + C++/WinRT + XAML** | 群里"Windows 10 UI 架构"的直译；Win10 1809+ 可跑。备选：Win32 + Direct2D/Composition（更硬核、控玻璃效果更自由）、Qt（最省事但风格违和） |
| 玻璃效果 | 系统 Acrylic 背景 + **D2D/Win2D 自绘**（高光横线、边缘 specular、折射) | React 库的 CSS `backdrop-filter` + 分层 SVG 思路 → 用 Composition Brush + 高斯模糊 Effect 重写 |
| 数据存储 | 先 JSON 文件，第二优先 SQLite (sqlite3 amalgamation) | 黑客松先保进度，SQLite 编译即用 |
| 构建 | CMake + vcpkg（sqlite3、nlohmann/json） | 团队都能编 |

## 4. 目录结构（三层隔离）

```text
src/
├── core/                  数据层
│   ├── event.hpp/.cpp         Event/Deadline 统一模型（三种接口形式归一）
│   ├── store.hpp/.cpp         存储：JSON 起步，SQLite 备选
│   ├── layout.hpp/.cpp        ★ 重叠判定 + 泳道排布（需求灵魂，纯函数，重点测试）
│   └── queries.hpp/.cpp       日/周/月查询聚合
├── api/                   接口层
│   ├── types.hpp              三种提交形式的结构体 + 归一化转换
│   ├── registry.hpp/.cpp      class 注册表：各模块注册，public 接口互联的枢纽
│   ├── bridge.hpp/.cpp        JSON 导入导出（兼容现有 examples/tasks.json）
│   └── endpoint.hpp/.cpp      本机 HTTP 端点（复用 Python 版的 127.0.0.1 + Host 校验思路）
├── ui/                    UI 层
│   ├── app.cpp                 窗口骨架 + 折叠按钮循环切换
│   ├── day_view.cpp            ★ 磨砂玻璃日视图（吃 layout 结果）
│   ├── week_view.cpp           周视图（信息密集）
│   ├── month_view.cpp          月视图（仅摘要）
│   ├── glass_effect.cpp        ★ 液态玻璃渲染（重写 React 库思路）
│   └── capture.cpp             截长图（P1，最后做）
└── tests/                 核心层与接口层单测（重叠/合并/归一化/导入导出）
```

## 5. 接口契约草案（hpp 示意，开工前拉齐）

```cpp
// core/event.hpp —— 数据层只暴露这些 public 接口
namespace bp {
struct Event { std::int64_t start{}, end{}; std::string title, note; Kind kind; };  // deadline: end==start
class EventStore {
public:
    void add(Event);                                    // 接口层唯一写入口
    std::vector<Event> day(Date) const;                 // 供日/周视图
    std::vector<Event> range(Date, Date) const;         // 供周/月视图
};
class LayoutEngine {                                    // 纯函数，可单测
public:
    struct Block { int lane; bool merged; std::vector<std::size_t> eventIdx; };
    std::vector<Block> layout(std::vector<Event> const&) const;  // 落实群里的重叠规则
};
}
// api/registry.hpp —— 各层 class 在此注册，只拿指针/引用，public 互联
```

## 6. 里程碑（时间盒，按演示时间 = 11-03 晚估，待确认后可缩放）

| 里程碑 | 内容 | 验收标准 | 预估 |
| --- | --- | --- | --- |
| **M0 骨架** | CMake + 三层空目录 + 全部 hpp 契约 + WinUI 空窗口 | `cmake --build` 通过，窗口能打开 | 3h |
| **M1 数据层** | Event 模型 + EventStore + **LayoutEngine 重叠算法** + 单测 | 重叠三种情况单测全绿 | 4h |
| **M2 日视图** | 磨砂玻璃块渲染 + 重叠排布 + 点开详情 | 用 examples 数据肉眼验收群里的全部重叠规则 | 5h |
| **M3 周/月视图** | 折叠按钮循环切换 + 周→日下钻 + 密集排版 | 三视图循环流畅、下钻正确 | 3h |
| **M4 接口层** | 三种形式 JSON 导入 + 本机 HTTP 端点 + 演示数据 | `curl` 提交 deadline/日程，界面即刷 | 3h |
| **M5 截长图** | 自动滚动 + 截帧拼接（P1，可砍） | 导出一张正确拼接的长图 | 3h |
| **M6 打磨** | 动画/高光细节、演示脚本、README 更新、录屏 | 5 分钟演示不翻车 | 2h |

**核心链路 = M0–M4 ≈ 18 小时**，两人并行（数据层 ∥ UI）可压到 1 天内；M5 只在核心链路提前完成时启动。

## 7. 分工建议（按群里分工倾向，可调）

- **张励志**：架构 + 数据层（M0 契约、M1 全部）——群里他定的三层和注释规范，由他把 hpp 契约守好；
- **丁子健**：UI/视觉（M2 玻璃渲染、M3 视图）——设计是他写的，重叠规则他验收最权威；
- **steve**：接口层 + 测试 + 演示（M4、M6、单测补位）——兼容 tasks.json、HTTP 端点、演示脚本。

## 8. 风险与开放问题

1. **C++/WinRT 上手成本**：若没人写过，WinUI3 的 XAML 绑定会吃掉半天。缓解：M0 就把空窗口跑通，不行立刻降级到 Win32+D2D 方案（视图都是自绘，迁移成本低）；
2. **玻璃效果性能**：逐帧 backdrop 模糊贵，缓解：静止时缓存位图，仅动画/滚动时实时模糊；
3. **演示数据**：不依赖真实账号，提前准备一份像样的大学生课表 JSON（含考试、网课 deadline、重叠课程）；
4. **待确认**：黑客松具体截止时间；第三种接口形式（催丁子健）；是否要我按本计划直接开工 M0。

## 9. 下一步（今天下午就做）

1. 群里同步本计划，确认截止时间与分工；
2. 张励志出 `core/*.hpp` 契约初稿（半小时），其余人读契约后并行开工；
3. 把 B 站视频里那套 React 液态玻璃库源码找出来喂给 AI，产出"实现途径笔记"（模糊/高光/分层的参数与层次），供 `glass_effect.cpp` 重写参考；
4. 准备演示数据 `examples/demo_university.json`。
