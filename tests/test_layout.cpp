#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "api/json_io.hpp"
#include "api/types.hpp"
#include "core/layout.hpp"
#include "core/queries.hpp"
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
    int mainSummary = 0;
    int shiftedSummary = 0;
    for (const auto& block : blocks) {
        if (block.lane == 0) {
            mainSummary += block.summary ? 1 : 0;
        } else {
            shiftedSummary += block.summary ? 1 : 0;
        }
    }
    check(mainSummary == 0, "contained: main lane full info");
    check(shiftedSummary == 1, "contained: shifted lane summary only");
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

void testStoreIncludesDeadlineInRange() {
    EventStore store;
    store.add(makeEvent(3600, 3600, "deadline"));
    check(store.range(0, 7200).size() == 1, "store: deadline in range");
    check(store.range(0, 3600).empty(), "store: deadline range half-open");
    check(store.all().size() == 1, "store: all returns deadline");
}

void testJsonSubmissionAndRoundTrip() {
    const std::string json =
        "[{\"type\":\"schedule\",\"start\":100,\"end\":200,"
        "\"title\":\"\\u9ad8\\u6570\",\"note\":\"line\\n2\"},"
        "{\"type\":\"schedule\",\"start\":300,\"durationSeconds\":60,"
        "\"title\":\"duration\"},"
        "{\"type\":\"deadline\",\"when\":400,\"title\":\"\\ud83d\\ude00\"}]";
    const auto events = bp::api::parseEventsJson(json);
    check(events.size() == 3, "json: three events");
    check(events[0].title == "\xe9\xab\x98\xe6\x95\xb0", "json: unicode title");
    check(events[0].note == "line\n2", "json: escaped newline");
    check(events[1].end == 360, "json: duration normalized");
    check(events[2].kind == Kind::Deadline, "json: deadline kind");
    check(events[2].title == "\xf0\x9f\x98\x80", "json: surrogate pair");
    const auto roundTrip = bp::api::parseEventsJson(bp::api::dumpEventsJson(events));
    check(roundTrip.size() == 3, "json: round trip size");
    check(roundTrip[0].start == 100 && roundTrip[0].end == 200,
          "json: round trip span");
    check(roundTrip[2].title == events[2].title, "json: round trip unicode");
}

void testJsonRejectsBadInput() {
    const std::vector<std::string> inputs = {
        "{",
        "[1]",
        "{\"type\":\"other\"}",
        "{\"type\":\"schedule\",\"start\":100}",
        "{\"type\":\"schedule\",\"start\":100,\"end\":\"200\"}",
        "{\"type\":\"deadline\",\"when\":1,\"title\":2}",
        "{\"type\":\"schedule\",\"start\":100,\"end\":50}",
        "{\"type\":\"deadline\",\"when\":01}",
        "{\"type\":\"deadline\",\"when\":1.5}",
        "{\"type\":\"deadline\",\"when\":999999999999999999999}",
        "{\"type\":\"deadline\",\"when\":1,\"when\":2}",
        "{\"type\":\"deadline\",\"when\":1,\"title\":\"\\ud800\"}",
    };
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        bool threw = false;
        try {
            bp::api::parseEventsJson(inputs[i]);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        check(threw, ("json rejects bad input " + std::to_string(i)).c_str());
    }
}

void testGroupByDayMultiDayEvent() {
    std::vector<Event> events = {
        makeEvent(0, 86400 * 3, "trip"),
    };
    const auto grouped = groupByDay(events, 0, 86400);
    check(grouped.size() == 3, "group: three days");
    check(grouped.count(0) == 1, "group: day0");
    check(grouped.count(86400) == 1, "group: day1");
    check(grouped.count(86400 * 2) == 1, "group: day2");
}

void testGroupByDayDeadlineLandsInOwnDay() {
    std::vector<Event> events = {
        makeEvent(86400 + 36000, 86400 + 36000, "hw"),
    };
    const auto grouped = groupByDay(events, 0, 86400);
    check(grouped.size() == 1, "group deadline: one day");
    check(grouped.count(86400) == 1, "group deadline: next day key");
    check(grouped.at(86400).size() == 1, "group deadline: one event");
}

void testGroupByDayIgnoresEventsBeforeWindow() {
    std::vector<Event> events = {
        makeEvent(-86400, -3600, "past"),
        makeEvent(-3600, 3600, "straddle"),
        makeEvent(4000, 5000, "in"),
    };
    const auto grouped = groupByDay(events, 0, 86400);
    check(grouped.size() == 1, "group window: one day only");
    check(grouped.at(0).size() == 2, "group window: straddle+in");
}

void testApiRejectsInvertedSchedule() {
    bp::api::ScheduleInput inverted;
    inverted.start = 100;
    inverted.end = 50;
    bool threw = false;
    try {
        bp::api::toEvent(inverted);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "api: end<start throws");

    bp::api::ScheduleInput negative;
    negative.start = 100;
    negative.durationSeconds = -10;
    threw = false;
    try {
        bp::api::toEvent(negative);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "api: negative duration throws");
}

}  // namespace

int main() {
    {
        EventStore store;
        auto local = makeEvent(10, 20, "local");
        const auto localId = store.add(local);
        auto imported = makeEvent(30, 40, "original");
        imported.source = "platform-a";
        imported.externalId = "stable-id";
        store.mergeImported({imported});
        imported.title = "updated";
        imported.end = 60;
        imported.done = true;
        store.mergeImported({imported, imported});
        check(store.size() == 2, "sync: repeated snapshots do not duplicate");
        check(store.get(localId)->title == "local", "sync: local events preserved");
        auto rows = store.range(30, 61);
        check(rows.size() == 1 && rows[0].title == "updated" && rows[0].done,
              "sync: deadline and completion update");
        imported.source = "platform-b";
        store.mergeImported({imported});
        check(store.size() == 3, "sync: platform identities do not collide");
        auto invalid = imported;
        invalid.externalId.clear();
        imported.externalId = "new-id";
        bool rejected = false;
        try { store.mergeImported({imported, invalid}); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected && store.size() == 3, "sync: invalid batch is atomic");
    }
    std::cout << std::unitbuf;
    std::cout << "start tests" << std::endl;
    testIdenticalSpansMergeIntoOneBlock();
    std::cout << "ok identical" << std::endl;
    testDisjointEventsStayOnMainLane();
    std::cout << "ok disjoint" << std::endl;
    testContainedEventShiftsRight();
    std::cout << "ok contained" << std::endl;
    testPartialOverlapShiftsRight();
    std::cout << "ok partial" << std::endl;
    testChainUsesLowestFreeLane();
    std::cout << "ok chain" << std::endl;
    testDeadlineGrouping();
    std::cout << "ok deadline" << std::endl;
    testStoreRoundTrip();
    std::cout << "ok store" << std::endl;
    testStoreIncludesDeadlineInRange();
    std::cout << "ok store deadline" << std::endl;
    testJsonSubmissionAndRoundTrip();
    std::cout << "ok json round trip" << std::endl;
    testJsonRejectsBadInput();
    std::cout << "ok json rejects" << std::endl;
    testApiNormalization();
    std::cout << "ok api" << std::endl;
    testGroupByDayMultiDayEvent();
    std::cout << "ok group multi" << std::endl;
    testGroupByDayDeadlineLandsInOwnDay();
    std::cout << "ok group deadline" << std::endl;
    testGroupByDayIgnoresEventsBeforeWindow();
    std::cout << "ok group window" << std::endl;
    testApiRejectsInvertedSchedule();
    std::cout << "ok inverted" << std::endl;
    if (g_failures != 0) {
        std::cout << g_failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "all core/api checks passed" << std::endl;
    return 0;
}
