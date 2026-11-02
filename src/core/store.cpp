#include "core/store.hpp"

#include <mutex>
#include <stdexcept>

namespace bp {

void EventStore::mergeImported(std::vector<Event> events) {
    for (const auto& event : events) {
        if (event.source.empty() || event.externalId.empty()) {
            throw std::invalid_argument("source and externalId required");
        }
    }
    std::unique_lock lock(mutex_);
    for (auto& event : events) {
        bool found = false;
        for (auto& record : records_) {
            if (record.event.source == event.source && record.event.externalId == event.externalId) {
                record.event = std::move(event);
                found = true;
                break;
            }
        }
        if (!found) records_.push_back(Record{nextId_++, std::move(event)});
    }
}

std::uint64_t EventStore::add(Event event) {
    std::unique_lock lock(mutex_);
    const std::uint64_t id = nextId_++;
    records_.push_back(Record{id, std::move(event)});
    return id;
}

bool EventStore::remove(std::uint64_t id) {
    std::unique_lock lock(mutex_);
    for (auto it = records_.begin(); it != records_.end(); ++it) {
        if (it->id == id) {
            records_.erase(it);
            return true;
        }
    }
    return false;
}

std::optional<Event> EventStore::get(std::uint64_t id) const {
    std::shared_lock lock(mutex_);
    for (const auto& record : records_) {
        if (record.id == id) {
            return record.event;
        }
    }
    return std::nullopt;
}

std::vector<Event> EventStore::range(std::int64_t from, std::int64_t to) const {
    std::shared_lock lock(mutex_);
    std::vector<Event> result;
    for (const auto& record : records_) {
        const Event& event = record.event;
        const bool pointInRange = event.start == event.end &&
                                  from <= event.start && event.start < to;
        const bool spanOverlaps = event.start < to && from < event.end;
        if (pointInRange || spanOverlaps) {
            result.push_back(event);
        }
    }
    return result;
}

std::vector<Event> EventStore::all() const {
    std::shared_lock lock(mutex_);
    std::vector<Event> result;
    result.reserve(records_.size());
    for (const auto& record : records_) {
        result.push_back(record.event);
    }
    return result;
}

std::size_t EventStore::size() const {
    std::shared_lock lock(mutex_);
    return records_.size();
}

}  // namespace bp
