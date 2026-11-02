#include "api/types.hpp"

namespace bp::api {

Event toEvent(const DeadlineInput& input) {
    Event event;
    event.kind = Kind::Deadline;
    event.start = input.when;
    event.end = input.when;
    event.title = input.title;
    event.note = input.note;
    return event;
}

Event toEvent(const ScheduleInput& input) {
    if (!input.end.has_value() && !input.durationSeconds.has_value()) {
        throw std::invalid_argument(
            "ScheduleInput requires either end or durationSeconds");
    }
    Event event;
    event.kind = Kind::Schedule;
    event.start = input.start;
    event.end = input.end.value_or(input.start + input.durationSeconds.value());
    event.title = input.title;
    event.note = input.note;
    return event;
}

}  // namespace bp::api
