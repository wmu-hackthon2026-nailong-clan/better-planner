#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "api/types.hpp"
#include "core/layout.hpp"
#include "core/store.hpp"

using namespace bp;

namespace {

int g_failures = 0;

void check(bool ok, const char* name) {
    if (!ok) {
        ++g_failures;
        std::cout << "FAIL: " << name << std::endl;
    }
}

Event makeEvent(std::int64_t start, std::int64_t end, std::string title) {
    Event event;
    event.kind = Kind::Schedule;
    event.start = start;
    event.end = end;
    event.title = std::move(title);
    return event;
}

void testIdenticalSpansMergeIntoOneBlock() {
    std::vector<Event> events = {
        makeEvent(0, 10, "math"),
        makeEvent(0, 10, "physics"),
    };
    const std::vector<LayoutEngine::Block> blocks = LayoutEngine().layout(events);
    check(blocks.size() == 1, "identical: one block");
    check(blocks[0].eventIndices.size() == 2, "identical: two events");
    check(blocks[0].lane == 0, "identical: main lane");
    check(!blocks[0].summary, "identical: full info");
}

void testDisjointEventsStayOnMainLane() {
    std::vector<Event> events = {
        makeEvent(0, 10, "a"),
        makeEvent(20, 30, "b"),
    };
    const std::vector<LayoutEngine::Block> blocks = LayoutEngine().layout(events);
    check(blocks.size() == 2, "disjoint: two blocks");
    bool ok = true;
    for (const auto& block : blocks) {
        ok = ok && (block.lane == 0) && !block.summary;
    }
    check(ok, "disjoint: lane 0, full info");
}

void testContainedEventShiftsRight() {
    std::vector<Event> events = {
        makeEvent(0, 100, "long"),
        makeEvent(20, 30, "inner"),
    };
    const std::vector<LayoutEngine::Block> blocks = LayoutEngine().layout(events);
    check(blocks.size() == 2, "contained: two blocks");
    check(blocks[0].lane != blocks[1].lane, "contained: different lanes");
    bool allSummary = true;
    for (const auto& block : blocks) {
        allSummary = allSummary && block.summary;
    }
    check(allSummary, "contained: summary only");
}

void testPartialOverlapShiftsRight() {
    std::vector<Event> events = {
        makeEvent(0, 20, "a"),
        makeEvent(10, 30, "b"),
    };
    const std::vector<LayoutEngine::Block> blocks = LayoutEngine().layout(events);
    check(blocks.size() == 2, "overlap: two blocks");
    check(blocks[0].lane != blocks[1].lane, "overlap: different lanes");
}

void testChainUsesLowestFreeLane() {
    std::vector<Event> events = {
        makeEvent(0, 10, "a"),
        makeEvent(5, 15, "b"),
        makeEvent(10, 20, "c"),
    };
    const std::vector<LayoutEngine::Block> blocks = LayoutEngine().layout(events);
    check(blocks.size() == 3, "chain: three blocks");
    int lanes[3] = {0, 0, 0};
    for (const auto& block : blocks) {
        lanes[block.eventIndices[0]] = block.lane;
    }
    check(lanes[0] == 0, "chain: a lane 0");
    check(lanes[1] == 1, "chain: b lane 1");
    check(lanes[2] == 0, "chain: c lane 0");
}

void testDeadlineGrouping() {
    std::vector<Event> events = {
        makeEvent(50, 50, "d1"),
        makeEvent(50, 50, "d2"),
    };
    const std::vector<LayoutEngine::Block> blocks = LayoutEngine().layout(events);
    check(blocks.size() == 1, "deadline: one block");
    check(blocks[0].eventIndices.size() == 2, "deadline: two events");
}

void testStoreRoundTrip() {
    EventStore store;
    const std::uint64_t id = store.add(makeEvent(0, 10, "a"));
    check(store.size() == 1, "store: size 1");
    check(store.get(id).has_value(), "store: get exists");
    check(store.get(id)->title == "a", "store: title matches");
    check(store.range(0, 5).size() == 1, "store: range hit");
    check(store.range(10, 20).empty(), "store: range half-open");
    check(store.remove(id), "store: remove");
    check(store.size() == 0, "store: empty after remove");
}

void testApiNormalization() {
    bp::api::DeadlineInput deadline;
    deadline.when = 42;
    deadline.title = "t";
    const Event d = bp::api::toEvent(deadline);
    check(d.kind == Kind::Deadline, "api: deadline kind");
    check(d.start == 42 && d.end == 42, "api: deadline point");

    bp::api::ScheduleInput byDuration;
    byDuration.start = 100;
    byDuration.durationSeconds = 50;
    byDuration.title = "s";
    const Event s1 = bp::api::toEvent(byDuration);
    check(s1.start == 100 && s1.end == 150, "api: duration mode");

    bp::api::ScheduleInput byEnd;
    byEnd.start = 100;
    byEnd.end = 200;
    byEnd.durationSeconds = 999;
    byEnd.title = "s";
    const Event s2 = bp::api::toEvent(byEnd);
    check(s2.end == 200, "api: end wins");

    bp::api::ScheduleInput invalid;
    invalid.start = 100;
    bool threw = false;
    try {
        bp::api::toEvent(invalid);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "api: missing end throws");
}

}  // namespace

int main() {
    testIdenticalSpansMergeIntoOneBlock();
    testDisjointEventsStayOnMainLane();
    testContainedEventShiftsRight();
    testPartialOverlapShiftsRight();
    testChainUsesLowestFreeLane();
    testDeadlineGrouping();
    testStoreRoundTrip();
    testApiNormalization();
    if (g_failures != 0) {
        std::cout << g_failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "all core/api checks passed" << std::endl;
    return 0;
}
