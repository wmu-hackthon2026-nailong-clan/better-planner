// core/store.hpp —— 事件存储的 public 接口（接口层唯一写入口）
//
// 层间规则：接口层（api）负责把外部提交归一化为 Event 后调用 add()；
// UI 层只读取。实现首版用内存 vector，接口不变即可无痛替换为 SQLite/JSON。
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "core/event.hpp"

namespace bp {

class EventStore {
public:
    EventStore() = default;

    // 新增事件，返回分配的稳定 id。
    std::uint64_t add(Event event);

    // 按 id 删除；id 不存在返回 false。
    bool remove(std::uint64_t id);

    // 按 id 取回事件副本；不存在返回 std::nullopt。
    std::optional<Event> get(std::uint64_t id) const;

    // 取回与 [from, to) 时间区间有交集的全部事件（半开区间口径）。
    std::vector<Event> range(std::int64_t from, std::int64_t to) const;

    // 当前事件总数（演示与测试用）。
    std::size_t size() const;

private:
    struct Record {
        std::uint64_t id;
        Event event;
    };
    std::vector<Record> records_;
    std::uint64_t nextId_ = 1;
};

}  // namespace bp
