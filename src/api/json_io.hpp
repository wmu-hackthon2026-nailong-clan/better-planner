// api/json_io.hpp —— 外部接口的 JSON 提交与导出（接口层契约）
//
// 对应群聊"日历软件需要做好外接接口，允许提交三种信息"：
//   * 本模块负责把外部 JSON 文本解析为统一 Event（经 types.hpp 归一化）；
//   * 第三种提交形式待丁子健确认后在此扩展，当前 type 仅接受
//     "deadline" 与 "schedule"。
//
// 提交格式（单个对象或对象数组均可）：
//   {"type":"deadline","when":1760000000,"title":"...","note":"..."}
//   {"type":"schedule","start":...,"end":...,"title":"...","note":"..."}
//   {"type":"schedule","start":...,"durationSeconds":5400,"title":"..."}
// 时间均为 epoch 秒（UTC），时区换算由提交方负责。
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "core/event.hpp"

namespace bp::api {

// 解析外部提交 JSON。字段缺失、类型错误、时间倒挂、未知 type 均抛出
// std::invalid_argument；语法错误同样以 std::invalid_argument 抛出，
// 由 HTTP 端点统一映射为 400。
std::vector<Event> parseEventsJson(std::string_view jsonText);

// 导出事件数组为 JSON 文本（GET /events 与调试用）。
// deadline 输出 when 字段；schedule 输出 start/end 字段。
std::string dumpEventsJson(const std::vector<Event>& events);

}  // namespace bp::api
