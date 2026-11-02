// core/layout.hpp —— 磨砂玻璃日视图的重叠排布引擎（产品灵魂，纯函数）
//
// 需求原文（丁子健，2026-11-01 群聊）：
//   * 同一时间内事件完全一样 -> 同一个玻璃框，内部用分隔符标记多个事件；
//   * 一个包含于另一个或时间有重叠 -> 被包含的项目右移；
//   * 无重叠 -> 玻璃框内显示所有信息；
//   * 有重叠 -> 只显示标题 3-5 个字符，点开后查看内容。
//
// 本类不依赖任何 UI / 存储设施，输入一天的事件列表，输出玻璃框排布结果，
// 便于单元测试与日后替换渲染实现。
#pragma once

#include <cstddef>
#include <vector>

#include "core/event.hpp"

namespace bp {

class LayoutEngine {
public:
    // 一个磨砂玻璃框的排布结果。
    struct Block {
        // 落进同一玻璃框的事件下标（对应输入 vector 的下标）。
        // size() > 1 表示时间完全相同、需要在框内用分隔符并列展示。
        std::vector<std::size_t> eventIndices;

        // 泳道序号：0 = 主列；>0 表示因重叠被右移的第几列。
        // UI 层按 lane * 固定缩进宽度右移绘制。
        int lane = 0;

        // true 表示该玻璃框与其他框存在重叠：
        // UI 只展示标题前 3-5 个字符，点击后弹出完整内容；
        // false 表示无重叠，可展示全部信息。
        bool summary = false;
    };

    LayoutEngine() = default;

    // 对一天内的事件计算排布。输入顺序任意，不修改输入。
    // 返回的 Block 数量等于"时间完全相同的事件聚簇数"。
    std::vector<Block> layout(const std::vector<Event>& dayEvents) const;
};

}  // namespace bp
