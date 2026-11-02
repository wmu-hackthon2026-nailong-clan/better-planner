// core/event.hpp —— 数据层统一事件模型（hpp 为契约，注释必须写清楚）
//
// 设计约定（张励志，2026-11-01 群聊）：
//   * 三层只通过本头文件等 public 接口互联，禁止跨层 include 实现文件；
//   * 本文件写清楚注释，对应实现文件不写注释。
//
// 时间的统一口径：
//   * 全部使用 Unix epoch 秒（UTC），与现有 Python 版 Keqi 保留毫秒的做法区分；
//   * 时区换算（北京时间 Asia/Shanghai）只发生在 UI 显示层与导入导出层，
//     core 层不做任何本地时间解析。
#pragma once

#include <cstdint>
#include <string>

namespace bp {

// 事件种类：对应外部接口的三种提交形式中已确定的前两种。
// 第三种（群聊中待定）预留枚举位，确定后在 Kind 末尾追加，禁止插队改值。
enum class Kind : std::int8_t {
    Deadline = 0,   // 形式①：单一时间点（start == end）
    Schedule = 1,   // 形式②：起始 + 终末时间点
    Reserved3 = 2,  // 形式③：占位，待丁子健确认语义后替换
};

// 事件统一模型：外部接口的三种提交形式都归一化到这里，UI 只认识 Event。
struct Event {
    Kind kind = Kind::Schedule;
    std::int64_t start = 0;  // 起始时间（epoch 秒）；Deadline 事件 start == end
    std::int64_t end = 0;    // 终末时间（epoch 秒，不含）；Deadline 事件 start == end
    std::string title;       // 标题；日视图重叠时只展示前 3-5 个字符
    std::string note;        // 注释；点开详情时展示

    // 平台同步扩展字段（默认空，不影响本地手动/HTTP 提交的事件）。
    std::string source;      // 来源："学习通" / "智慧树" / 空（本地）
    std::string course;      // 所属课程名
    std::string location;    // 地点
    std::string teacher;     // 教师
    std::string url;         // 平台详情链接
    bool done = false;       // 平台侧是否已完成
    std::string externalId;  // 平台稳定标识；与 source 联合作为同步更新键。
};

// 判定两个时间区间是否重叠（半开区间 [start, end) 口径）。
// 完全相同的时间点也算重叠（同一玻璃框合并的依据，见 layout.hpp）。
inline bool overlaps(const Event& a, const Event& b) {
    return a.start < b.end && b.start < a.end;
}

// 判定两个事件时间完全相同（合并进同一磨砂玻璃框的唯一条件）。
inline bool sameSpan(const Event& a, const Event& b) {
    return a.start == b.start && a.end == b.end;
}

}  // namespace bp
