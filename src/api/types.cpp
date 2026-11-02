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
    if (input.end.has_value()) {
        event.end = *input.end;
    } else {
        event.end = input.start + *input.durationSeconds;
    }
    if (event.end < event.start) {
        throw std::invalid_argument(
            "ScheduleInput end must not be earlier than start");
    }
    event.title = input.title;
    event.note = input.note;
    return event;
}

}  // namespace bp::api
