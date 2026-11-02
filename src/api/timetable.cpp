#include "api/timetable.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <sstream>

namespace bp::api {

namespace {

// 去除首尾空白。
std::string trim(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return std::string();
    }
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

// 轻量 JSON 值提取：给定文本与键名，返回紧跟其后的字符串值（去转义）。
std::string getString(const std::string& text, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    std::size_t pos = text.find(needle);
    if (pos == std::string::npos) {
        return std::string();
    }
    const std::size_t colon = text.find(':', pos + needle.size());
    if (colon == std::string::npos) {
        return std::string();
    }
    std::size_t cursor = colon + 1;
    while (cursor < text.size() &&
           std::isspace(static_cast<unsigned char>(text[cursor]))) {
        ++cursor;
    }
    if (cursor >= text.size() || text[cursor] != '"') {
        return std::string();
    }
    const std::size_t end = text.find('"', cursor + 1);
    if (end == std::string::npos) {
        return std::string();
    }
    std::string out = text.substr(cursor + 1, end - cursor - 1);
    // 处理常见转义 \" 与 \\。
    std::string unescaped;
    unescaped.reserve(out.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '\\' && i + 1 < out.size()) {
            ++i;
        }
        unescaped.push_back(out[i]);
    }
    return unescaped;
}

// 提取整数（键值对中的数值，正负均可）。
int getInt(const std::string& text, const std::string& key, int fallback) {
    const std::string needle = "\"" + key + "\"";
    const std::size_t pos = text.find(needle);
    if (pos == std::string::npos) {
        return fallback;
    }
    const std::size_t colon = text.find(':', pos + needle.size());
    if (colon == std::string::npos) {
        return fallback;
    }
    std::size_t cursor = colon + 1;
    while (cursor < text.size() &&
           std::isspace(static_cast<unsigned char>(text[cursor]))) {
        ++cursor;
    }
    if (cursor >= text.size()) {
        return fallback;
    }
    const char* begin = text.c_str() + cursor;
    char* endPtr = nullptr;
    const long value = std::strtol(begin, &endPtr, 10);
    if (endPtr == begin) {
        return fallback;
    }
    return static_cast<int>(value);
}

// 从文本中提取某个键的数组元素片段（每个元素是 {...} 的原始文本）。
std::vector<std::string> getArray(const std::string& text,
                                  const std::string& key) {
    std::vector<std::string> items;
    const std::string needle = "\"" + key + "\"";
    const std::size_t pos = text.find(needle);
    if (pos == std::string::npos) {
        return items;
    }
    const std::size_t colon = text.find(':', pos + needle.size());
    if (colon == std::string::npos) {
        return items;
    }
    const std::size_t open = text.find('[', colon + 1);
    if (open == std::string::npos) {
        return items;
    }
    std::size_t cursor = open + 1;
    while (cursor < text.size()) {
        while (cursor < text.size() && text[cursor] != '{' &&
               text[cursor] != ']') {
            ++cursor;
        }
        if (cursor >= text.size() || text[cursor] == ']') {
            break;
        }
        // 逐字符找配对的 }，处理嵌套对象。
        std::size_t depth = 0;
        std::size_t close = cursor;
        for (; close < text.size(); ++close) {
            if (text[close] == '{') {
                ++depth;
            } else if (text[close] == '}') {
                --depth;
                if (depth == 0) {
                    break;
                }
            }
        }
        if (close >= text.size()) {
            break;
        }
        items.push_back(text.substr(cursor, close - cursor + 1));
        cursor = close + 1;
    }
    return items;
}

// 解析周次描述："1-16周"、"1,3,5周"、"2-16周(双)"、"1-16周(单)" 等。
// 返回生效周次列表（1-based）。空或无法解析返回空。
std::vector<int> parseWeeks(const std::string& raw) {
    std::vector<int> weeks;
    std::string s = trim(raw);
    if (s.empty()) {
        return weeks;
    }
    // 去掉"周"字与括号内容说明，但记住单双周标记。
    bool oddOnly = false;
    bool evenOnly = false;
    if (s.find("(单)") != std::string::npos || s.find("（单）") != std::string::npos) {
        oddOnly = true;
    }
    if (s.find("(双)") != std::string::npos || s.find("（双）") != std::string::npos) {
        evenOnly = true;
    }
    // 去掉括号及其中内容。
    const std::size_t lp = s.find('(');
    if (lp != std::string::npos) {
        s = s.substr(0, lp);
    }
    const std::size_t lc = s.find("\xEF\xBC\x88");  // 全角（
    if (lc != std::string::npos) {
        s = s.substr(0, lc);
    }
    // 去掉"周"字。
    std::string digits;
    digits.reserve(s.size());
    for (char ch : s) {
        if (std::isdigit(static_cast<unsigned char>(ch)) || ch == ',' ||
            ch == '-') {
            digits.push_back(ch);
        }
    }
    if (digits.empty()) {
        return weeks;
    }
    // 按逗号切段。
    std::size_t start = 0;
    while (start <= digits.size()) {
        const std::size_t comma = digits.find(',', start);
        const std::string seg =
            digits.substr(start, comma == std::string::npos
                                     ? std::string::npos
                                     : comma - start);
        if (!seg.empty()) {
            const std::size_t dash = seg.find('-');
            if (dash == std::string::npos) {
                const int w = std::atoi(seg.c_str());
                if (w > 0) {
                    weeks.push_back(w);
                }
            } else {
                const int from = std::atoi(seg.substr(0, dash).c_str());
                const int to = std::atoi(seg.substr(dash + 1).c_str());
                for (int w = from; w <= to && w <= 30; ++w) {
                    weeks.push_back(w);
                }
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    // 应用单双周过滤。
    if (oddOnly || evenOnly) {
        std::vector<int> filtered;
        for (int w : weeks) {
            const bool odd = (w % 2) == 1;
            if ((oddOnly && odd) || (evenOnly && !odd)) {
                filtered.push_back(w);
            }
        }
        weeks.swap(filtered);
    }
    return weeks;
}

// 内置默认节次时间表：第 N 节的起始分钟（从 0 点起）。
// 与常见高校作息一致：上午 08:00 起每 45 分钟一节、课间 5 分钟，
// 中午 11:55 结束第 5 节；下午 13:30 起；晚上 18:20 起。
int defaultSectionStart(int section) {
    // 上午第 1-5 节（08:00 / 08:45 / 09:40 / 10:25 / 11:10）。
    if (section == 1) return 8 * 60;
    if (section == 2) return 8 * 60 + 45;
    if (section == 3) return 9 * 60 + 40;
    if (section == 4) return 10 * 60 + 25;
    if (section == 5) return 11 * 60 + 10;
    // 下午第 6-8 节（13:30 / 14:15 / 15:00 起，每 45 分钟）。
    if (section == 6) return 13 * 60 + 30;
    if (section == 7) return 14 * 60 + 15;
    if (section == 8) return 15 * 60;
    // 晚上第 9-11 节（18:20 起，每 45 分钟）。
    if (section == 9) return 18 * 60 + 20;
    if (section == 10) return 19 * 60 + 5;
    if (section == 11) return 19 * 60 + 50;
    // 兜底：线性外推。
    return 8 * 60 + (section - 1) * 50;
}

// 每节课时长（分钟）。
constexpr int kSectionMinutes = 45;

// 把 "HH:MM" 解析为当日分钟数；失败返回 -1。
int parseClockMinutes(const std::string& text) {
    const int h = std::atoi(text.substr(0, 2).c_str());
    const int m = std::atoi(text.substr(3, 2).c_str());
    if (h < 0 || h > 23 || m < 0 || m > 59) {
        return -1;
    }
    return h * 60 + m;
}

// 当前时间最近的周一 0 点（epoch 秒，本地时区）。
std::int64_t nearestMonday() {
    const std::time_t now = std::time(nullptr);
    std::tm lt{};
    localtime_s(&lt, &now);
    lt.tm_hour = 0;
    lt.tm_min = 0;
    lt.tm_sec = 0;
    const std::int64_t today = static_cast<std::int64_t>(_mktime64(&lt));
    const int back = (lt.tm_wday + 6) % 7;  // 周一=0
    return today - back * 86400;
}

}  // namespace

TimetableResult parseTimetable(const std::string& text,
                               std::int64_t semesterStart) {
    TimetableResult result;
    result.empty = true;

    // 优先找 kbList 数组；若整体本身是数组，也支持直接传入课表行。
    std::vector<std::string> rows = getArray(text, "kbList");
    if (rows.empty()) {
        // 尝试：整个文本就是一个 JSON 数组（kbList 内容）。
        rows = getArray(text, "");
    }
    if (rows.empty()) {
        // 尝试从 HTML 里提取内嵌 JSON：查找 kbList 关键字附近的数组。
        const std::size_t kb = text.find("kbList");
        if (kb != std::string::npos) {
            const std::size_t open = text.find('[', kb);
            if (open != std::string::npos) {
                // 用 getArray 语义：手动从 open 扫描顶层对象。
                std::size_t cursor = open + 1;
                while (cursor < text.size()) {
                    while (cursor < text.size() && text[cursor] != '{' &&
                           text[cursor] != ']') {
                        ++cursor;
                    }
                    if (cursor >= text.size() || text[cursor] == ']') break;
                    std::size_t depth = 0;
                    std::size_t close = cursor;
                    for (; close < text.size(); ++close) {
                        if (text[close] == '{') ++depth;
                        else if (text[close] == '}') { if (--depth == 0) break; }
                    }
                    if (close >= text.size()) break;
                    rows.push_back(text.substr(cursor, close - cursor + 1));
                    cursor = close + 1;
                }
            }
        }
    }

    if (rows.empty()) {
        result.warnings.push_back("未找到课表数据（kbList 为空或 HTML 无内嵌数据）");
        return result;
    }

    if (semesterStart == 0) {
        semesterStart = nearestMonday();
    }

    for (const std::string& row : rows) {
        // 每条课表行可能是 {..., "name":{...}} 或字段直接平铺。
        std::string flat = row;
        const std::string nameObj = getString(row, "name");
        // name 是对象而非字符串时，字段直接读 row（对象内容也在 row 里）。
        (void)nameObj;

        const std::string course = getString(row, "kcmc");
        const std::string teacher =
            getString(row, "xm").empty() ? getString(row, "jsxm")
                                         : getString(row, "xm");
        const std::string location = getString(row, "cdmc");
        const int weekday = getInt(row, "xqj", 0);
        const std::string zcd = getString(row, "zcd");
        // 节次：优先 jcor/length，其次 jcs("1-2")。
        int startSection = getInt(row, "jcor", 0);
        int length = getInt(row, "length", 1);
        if (startSection == 0) {
            const std::string jcs = getString(row, "jcs");
            if (!jcs.empty()) {
                const std::size_t dash = jcs.find('-');
                const int a = std::atoi(jcs.substr(0, dash).c_str());
                const int b = dash == std::string::npos
                                  ? a
                                  : std::atoi(jcs.substr(dash + 1).c_str());
                startSection = a;
                length = b - a + 1;
            }
        }

        if (course.empty() || weekday < 1 || weekday > 7 || startSection < 1) {
            result.warnings.push_back("跳过缺字段的行：" + course);
            continue;
        }

        const std::vector<int> weeks = parseWeeks(zcd);
        if (weeks.empty()) {
            result.warnings.push_back("周次无法解析，跳过：" + course);
            continue;
        }

        const int startMin = defaultSectionStart(startSection);
        if (startMin < 0) {
            continue;
        }

        result.empty = false;
        for (int week : weeks) {
            // 学期第 week 周、星期 weekday（1=周一）。周内偏移：
            // (week-1)*7 + (weekday-1) 天。
            const std::int64_t dayBase =
                semesterStart +
                (static_cast<std::int64_t>(week) - 1) * 7 * 86400 +
                (static_cast<std::int64_t>(weekday) - 1) * 86400;
            Event e;
            e.kind = Kind::Schedule;
            e.start = dayBase + startMin * 60;
            e.end = e.start + length * kSectionMinutes * 60;
            e.title = course;
            e.course = course;
            e.location = location;
            e.teacher = teacher;
            e.source = "教务系统";
            e.externalId = course + "@" + std::to_string(weekday) + "-" +
                           std::to_string(startSection);
            result.events.push_back(std::move(e));
        }
    }

    return result;
}

}  // namespace bp::api
