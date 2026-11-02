// api/types.hpp —— 外部接口的三种提交形式（接口层契约）
//
// 需求原文（丁子健，2026-11-01 群聊）："日历软件需要做好外接接口，
// 允许提交三种信息"：
//   ① deadline 形式：仅一个时间点、标题、注释；
//   ② 日程形式：起始、终末时间点、标题、注释；允许两种注册方式——
//      起始时间 + 持续时间，或 起始时间 + 终末时间；
//   ③ 第三种：群聊中暂未确定，此处预留，确定后追加。
//
// 所有输入最终归一化为 core::Event（UI 层只认识 Event）。
// 时间字段均为 epoch 秒（UTC），时区换算由提交方或 UI 层完成。
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include "core/event.hpp"

namespace bp::api {

// 形式①：deadline。
struct DeadlineInput {
    std::int64_t when = 0;  // 截止时间点（epoch 秒）
    std::string title;
    std::string note;
};

// 形式②：日程。end 与 durationSeconds 二选一；
// 两者都填时以 end 为准；两者都缺视为非法输入。
struct ScheduleInput {
    std::int64_t start = 0;                      // 起始时间（epoch 秒）
    std::optional<std::int64_t> end;             // 方式 A：终末时间
    std::optional<std::int64_t> durationSeconds; // 方式 B：持续时间
    std::string title;
    std::string note;
};

// 归一化：形式① -> Event（start == end == when，kind = Deadline）。
Event toEvent(const DeadlineInput& input);

// 归一化：形式② -> Event。end 与 durationSeconds 均缺失时抛出
// std::invalid_argument；归一化后终末早于起始（含负持续时间）同样抛出
// std::invalid_argument，由调用方决定如何向用户呈现。
Event toEvent(const ScheduleInput& input);

}  // namespace bp::api
