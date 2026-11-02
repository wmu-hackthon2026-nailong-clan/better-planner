// core/queries.hpp —— 日/周/月视图的查询聚合（纯函数）
//
// 视图与时间粒度（丁子健，2026-11-01 群聊）：
//   * 日进程：一天内的全部事件 -> LayoutEngine 排布；
//   * 周进程：7 天，信息密集；
//   * 月进程：仅摘要。
// 本头文件只提供"把区间结果按天分组"这一基础能力，周/月的聚合在 UI 层组合。
#pragma once

#include <map>
#include <vector>

#include "core/event.hpp"

namespace bp {

// 把一组事件按其覆盖到的"天"分组。
// dayStart：第一天的 0 点（epoch 秒，由 UI 层按时区算好传入）；
// daySeconds：一天长度（86400，保留参数便于测试伪造短天）。
// 返回值 key 为该天的 0 点时间戳，value 为与该天 [0点, 0点+daySeconds)
// 有交集的事件。跨天事件会出现在它覆盖到的每一天里。
std::map<std::int64_t, std::vector<Event>> groupByDay(
    const std::vector<Event>& events,
    std::int64_t dayStart,
    std::int64_t daySeconds);

}  // namespace bp
