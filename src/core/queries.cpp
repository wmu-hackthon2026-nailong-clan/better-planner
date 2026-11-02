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
        for (std::int64_t day = (first / daySeconds) * daySeconds;
             day < spanEnd; day += daySeconds) {
            if (day >= dayStart) {
                grouped[day].push_back(event);
            }
        }
    }
    return grouped;
}

}  // namespace bp
