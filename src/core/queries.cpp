#include "core/queries.hpp"

#include <algorithm>

namespace bp {

std::map<std::int64_t, std::vector<Event>> groupByDay(
    const std::vector<Event>& events,
    std::int64_t dayStart,
    std::int64_t daySeconds) {
    std::map<std::int64_t, std::vector<Event>> grouped;
    for (const Event& event : events) {
        const std::int64_t spanEnd =
            std::max(event.end, event.start + 1);
        const std::int64_t first = std::max(event.start, dayStart);
        // 以 dayStart 为对齐基准计算第一天，避免时区偏移导致
        // (first / daySeconds) 把本地 0 点错误取整到 UTC 0 点。
        const std::int64_t firstDay =
            dayStart + ((first - dayStart) / daySeconds) * daySeconds;
        for (std::int64_t day = firstDay; day < spanEnd;
             day += daySeconds) {
            grouped[day].push_back(event);
        }
    }
    return grouped;
}

}  // namespace bp
