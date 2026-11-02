// api/timetable.hpp —— 教务系统课表导入（接口层契约）
//
// 目标：把正方教务系统（zftal，如温州医科大学教学综合信息服务平台）导出的
// 课表数据归一化为统一 Event。课表是「周几 + 第几节」循环排课，不是单天
// 一次性事件，故导入时按学期周次展开为每周重复的 Schedule 事件。
//
// 支持两种输入形态（自动识别，无需调用方指定）：
//   ① 教务系统接口返回的课表 JSON，含 kbList 数组；每条课程含
//      kcmc(课程名) / xqj(星期,1-7) / zcd(周次描述) / cdmc(场地) /
//      xm 或 jsxm(教师) / jcor(起始节次) / length(课长节数)。
//      周次描述 zcd 支持："1-16周"、"1,3,5周"、"2-16周(双)" 等写法。
//   ② 保存网页后的静态 HTML：若含 <table> 课表，或内嵌 kbList JSON，
//      同样解析。纯前端外壳（课程由 JS 动态加载、无数据）会返回空并标记。
//
// 时间换算依赖「学期起始日 + 节次时间表」，二者优先从 JSON 里读取，
// 缺省时用内置默认节次表（上午 08:00 起每 45 分钟一节、课间 5 分钟，
// 下午 13:30 起，晚上 18:20 起，与常见高校作息一致）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/event.hpp"

namespace bp::api {

// 一条待展开的课表行（未展开周次前的中间表示）。
struct TimetableRow {
    std::string course;    // 课程名
    int weekday = 1;       // 星期几 1=周一 .. 7=周日
    int startSection = 1;  // 起始节次（1-based）
    int length = 1;        // 课长（节数）
    std::string teacher;   // 教师
    std::string location;  // 场地
    std::vector<int> weeks;  // 生效周次列表（1-based，已解析）
};

// 解析结果。
struct TimetableResult {
    std::vector<Event> events;   // 展开后的 Event（source 置 "教务系统"）
    std::vector<std::string> warnings;  // 非致命提示（如某行缺字段被跳过）
    bool empty = true;           // 未解析到任何数据（可能是纯外壳 HTML）
};

// 解析课表文本（JSON 或 HTML），自动识别。semesterStart 为学期第一周
// 周一的本地 0 点 epoch 秒；传 0 表示缺省，用内置默认（当前时间附近
// 最近的周一）。返回展开后的 Event 列表。
TimetableResult parseTimetable(const std::string& text,
                               std::int64_t semesterStart);

}  // namespace bp::api
