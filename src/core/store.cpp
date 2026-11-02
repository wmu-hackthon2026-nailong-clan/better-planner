#include "core/store.hpp"

namespace bp {

std::uint64_t EventStore::add(Event event) {
    const std::uint64_t id = nextId_++;
    records_.push_back(Record{id, std::move(event)});
    return id;
}

bool EventStore::remove(std::uint64_t id) {
    for (auto it = records_.begin(); it != records_.end(); ++it) {
        if (it->id == id) {
            records_.erase(it);
            return true;
        }
    }
    return false;
}

std::optional<Event> EventStore::get(std::uint64_t id) const {
    for (const auto& record : records_) {
        if (record.id == id) {
            return record.event;
        }
    }
    return std::nullopt;
}

std::vector<Event> EventStore::range(std::int64_t from, std::int64_t to) const {
    std::vector<Event> result;
    for (const auto& record : records_) {
        const Event& event = record.event;
        if (event.start < to && from < event.end) {
            result.push_back(event);
        }
    }
    return result;
}

std::size_t EventStore::size() const {
    return records_.size();
}

}  // namespace bp
