#include "api/sync.hpp"

#include <windows.h>

#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace bp::api {

namespace {

std::string rootDir() {
    char buffer[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    std::string path(buffer);
    const std::size_t slash = path.find_last_of("\\/");
    if (slash != std::string::npos) {
        path = path.substr(0, slash);
    }
    const std::size_t build = path.rfind("\\build");
    if (build != std::string::npos && build + 6 == path.size()) {
        path = path.substr(0, build);
    }
    return path;
}

std::string readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::string();
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string configString(const std::string& json, const std::string& key,
                         const std::string& fallback) {
    const std::string needle = "\"" + key + "\"";
    const std::size_t pos = json.find(needle);
    if (pos == std::string::npos) {
        return fallback;
    }
    const std::size_t colon = json.find(':', pos + needle.size());
    if (colon == std::string::npos) {
        return fallback;
    }
    const std::size_t quote = json.find('"', colon + 1);
    if (quote == std::string::npos) {
        return fallback;
    }
    const std::size_t end = json.find('"', quote + 1);
    if (end == std::string::npos) {
        return fallback;
    }
    return json.substr(quote + 1, end - quote - 1);
}

std::string runProcess(const std::string& program, const std::string& args,
                       const std::string& cwd, int timeoutSeconds,
                       int& exitCode) {
    std::string commandLine = "\"" + program + "\" " + args;
    std::vector<char> commandBuffer(commandLine.begin(), commandLine.end());
    commandBuffer.push_back('\0');

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        exitCode = -1;
        return std::string();
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    const BOOL created =
        CreateProcessA(nullptr, commandBuffer.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr,
                       cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    CloseHandle(writePipe);
    if (!created) {
        CloseHandle(readPipe);
        exitCode = -1;
        return std::string();
    }

    std::string output;
    char buffer[4096];
    DWORD bytesRead = 0;
    const DWORD deadline = GetTickCount() +
                           static_cast<DWORD>(timeoutSeconds) * 1000;
    bool finished = false;
    while (!finished) {
        if (GetTickCount() > deadline) {
            TerminateProcess(pi.hProcess, 1);
            break;
        }
        if (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &bytesRead, nullptr) &&
            bytesRead > 0) {
            const BOOL ok = ReadFile(readPipe, buffer, sizeof(buffer),
                                     &bytesRead, nullptr);
            if (!ok || bytesRead == 0) {
                finished = true;
                continue;
            }
            output.append(buffer, bytesRead);
        } else {
            DWORD processExit = 0;
            if (GetExitCodeProcess(pi.hProcess, &processExit) &&
                processExit != STILL_ACTIVE) {
                while (ReadFile(readPipe, buffer, sizeof(buffer), &bytesRead,
                                nullptr) &&
                       bytesRead > 0) {
                    output.append(buffer, bytesRead);
                }
                exitCode = static_cast<int>(processExit);
                finished = true;
            } else {
                Sleep(20);
            }
        }
    }

    CloseHandle(readPipe);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return output;
}

std::string trim(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return std::string();
    }
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

// 轻量 JSON 值解析：仅取字段值（字符串/布尔/数字），足够平台返回的结构。
class FlatJson {
public:
    explicit FlatJson(const std::string& text) : text_(text) {}

    // 取顶层某键的字符串值（空则返回 ""）。
    std::string getString(const std::string& key) const {
        const std::string needle = "\"" + key + "\"";
        const std::size_t pos = text_.find(needle);
        if (pos == std::string::npos) {
            return std::string();
        }
        const std::size_t colon = text_.find(':', pos + needle.size());
        if (colon == std::string::npos) {
            return std::string();
        }
        std::size_t cursor = colon + 1;
        while (cursor < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[cursor]))) {
            ++cursor;
        }
        if (cursor >= text_.size() || text_[cursor] != '"') {
            return std::string();
        }
        const std::size_t end = text_.find('"', cursor + 1);
        if (end == std::string::npos) {
            return std::string();
        }
        return text_.substr(cursor + 1, end - cursor - 1);
    }

    // 取顶层某键的布尔值（true 才返回 true）。
    bool getBool(const std::string& key) const {
        const std::string needle = "\"" + key + "\"";
        const std::size_t pos = text_.find(needle);
        if (pos == std::string::npos) {
            return false;
        }
        const std::size_t colon = text_.find(':', pos + needle.size());
        if (colon == std::string::npos) {
            return false;
        }
        return text_.compare(colon + 1, 4, "true") == 0;
    }

    // 提取顶层某键为数组的每个元素片段（不含花括号）。
    std::vector<std::string> getArray(const std::string& key) const {
        std::vector<std::string> items;
        const std::string needle = "\"" + key + "\"";
        const std::size_t pos = text_.find(needle);
        if (pos == std::string::npos) {
            return items;
        }
        const std::size_t colon = text_.find(':', pos + needle.size());
        if (colon == std::string::npos) {
            return items;
        }
        const std::size_t open = text_.find('[', colon + 1);
        if (open == std::string::npos) {
            return items;
        }
        std::size_t cursor = open + 1;
        while (cursor < text_.size()) {
            while (cursor < text_.size() &&
                   std::isspace(static_cast<unsigned char>(text_[cursor])) &&
                   text_[cursor] != '{') {
                ++cursor;
            }
            if (cursor >= text_.size() || text_[cursor] == ']') {
                break;
            }
            if (text_[cursor] == '{') {
                const std::size_t close = text_.find('}', cursor + 1);
                if (close == std::string::npos) {
                    break;
                }
                items.push_back(text_.substr(cursor, close - cursor + 1));
                cursor = close + 1;
            } else {
                ++cursor;
            }
        }
        return items;
    }

private:
    std::string text_;
};

// 把 "YYYY-MM-DD HH:MM"（北京时间）解析为 epoch 秒。
std::int64_t parseBeijing(const std::string& text) {
    std::tm tm = {};
    std::istringstream stream(text);
    stream >> std::get_time(&tm, "%Y-%m-%d %H:%M");
    if (stream.fail()) {
        stream.clear();
        stream.str(text);
        stream >> std::get_time(&tm, "%Y-%m-%dT%H:%M");
    }
    if (stream.fail()) {
        return 0;
    }
    const time_t local = _mkgmtime(&tm);
    return static_cast<std::int64_t>(local) - 8 * 3600;
}

// 解析平台时间：纯数字按 epoch（毫秒或秒），否则按北京时间字符串。
std::int64_t parseTime(const std::string& text) {
    const std::string value = trim(text);
    if (value.empty()) {
        return 0;
    }
    if (value.find_first_not_of("0123456789") == std::string::npos) {
        const std::int64_t number = std::stoll(value);
        return number > 100000000000LL ? number / 1000 : number;
    }
    return parseBeijing(value);
}

}  // namespace

SyncConfig loadSyncConfig() {
    SyncConfig config;
    const std::string root = rootDir();
    const std::string json = readFile(root + "\\config.local.json");
    config.python = configString(json, "zhs_python", "python");
    config.xxtScript = root + "\\scripts\\xxt_readonly.py";
    config.zhsScript = root + "\\scripts\\zhs_readonly.py";
    config.timeoutSeconds = 300;
    return config;
}

SyncResult syncChaoxing(const SyncConfig& config) {
    SyncResult result;
    result.platform = "学习通";
    int exitCode = 0;
    const std::string output = runProcess(
        config.python,
        "\"" + config.xxtScript + "\" week --json", rootDir(),
        config.timeoutSeconds, exitCode);
    if (exitCode != 0) {
        result.error = exitCode == 2 ? "尚未登录或登录已失效"
                                     : "查询失败（退出码 " +
                                           std::to_string(exitCode) + "）";
        return result;
    }
    const std::string json = trim(output);
    if (json.empty()) {
        result.error = "连接器无输出";
        return result;
    }
    // 学习通脚本输出是数组（课表/作业/考试行）。逐条解析。
    const FlatJson root(json);
    const std::vector<std::string> items = root.getArray("");
    if (items.empty() && json.front() == '[') {
        // 数组在顶层，手动切分。
        std::size_t cursor = json.find('[') + 1;
        while (cursor < json.size()) {
            while (cursor < json.size() && json[cursor] != '{' &&
                   json[cursor] != ']') {
                ++cursor;
            }
            if (cursor >= json.size() || json[cursor] == ']') {
                break;
            }
            const std::size_t close = json.find('}', cursor + 1);
            if (close == std::string::npos) {
                break;
            }
            const std::string item = json.substr(cursor, close - cursor + 1);
            const FlatJson row(item);
            Event event;
            event.source = "学习通";
            const std::string kind = row.getString("kind");
            const std::string name = row.getString("name");
            event.title = name.empty() ? row.getString("title") : name;
            if (event.title.empty()) {
                cursor = close + 1;
                continue;
            }
            const std::string date = row.getString("date");
            const std::string startText = row.getString("start");
            const std::string endText = row.getString("end");
            const std::string deadlineText = row.getString("deadline");
            if (!date.empty() && !startText.empty()) {
                event.start = parseBeijing(date + " " + startText);
            } else if (!startText.empty()) {
                event.start = parseTime(startText);
            }
            if (!date.empty() && !endText.empty()) {
                event.end = parseBeijing(date + " " + endText);
            } else if (!endText.empty()) {
                event.end = parseTime(endText);
            } else if (!deadlineText.empty()) {
                event.end = parseTime(deadlineText);
            }
            if (event.start == 0 && event.end != 0) {
                event.start = event.end;
            }
            event.kind = (kind == "class" ||
                          (event.start != 0 && event.start != event.end))
                             ? Kind::Schedule
                             : Kind::Deadline;
            event.course = row.getString("course");
            if (event.course.empty() && !name.empty()) {
                event.course = name;  // 学习通 week 输出的 name 即课程名
            }
            event.location = row.getString("location");
            event.teacher = row.getString("teacher");
            event.url = row.getString("url");
            event.done = row.getBool("completed");
            if (event.start != 0 || event.end != 0) {
                result.events.push_back(event);
            }
            cursor = close + 1;
        }
    } else {
        for (const std::string& item : items) {
            const FlatJson row(item);
            Event event;
            event.source = "学习通";
            const std::string kind = row.getString("kind");
            const std::string name = row.getString("name");
            event.title = name.empty() ? row.getString("title") : name;
            if (event.title.empty()) {
                continue;
            }
            const std::string startText = row.getString("start");
            const std::string deadlineText = row.getString("deadline");
            event.start = parseTime(startText);
            event.end = parseTime(deadlineText);
            if (event.start == 0 && event.end != 0) {
                event.start = event.end;
            }
            event.kind = (kind == "class" ||
                          (event.start != 0 && event.start != event.end))
                             ? Kind::Schedule
                             : Kind::Deadline;
            event.course = row.getString("course");
            event.location = row.getString("location");
            event.teacher = row.getString("teacher");
            event.url = row.getString("url");
            event.done = row.getBool("completed");
            if (event.start != 0 || event.end != 0) {
                result.events.push_back(event);
            }
        }
    }
    return result;
}

SyncResult syncZhihuishu(const SyncConfig& config) {
    SyncResult result;
    result.platform = "智慧树";
    int exitCode = 0;
    const std::string output =
        runProcess(config.python, "\"" + config.zhsScript + "\"",
                   rootDir() + "\\data\\zhs", config.timeoutSeconds, exitCode);
    if (exitCode != 0) {
        result.error = exitCode == 2 ? "尚未登录或登录已失效"
                                     : "读取失败（退出码 " +
                                           std::to_string(exitCode) + "）";
        return result;
    }
    const std::string json = trim(output);
    if (json.empty()) {
        result.error = "连接器无输出";
        return result;
    }
    const FlatJson root(json);
    const std::vector<std::string> items = root.getArray("events");
    for (const std::string& item : items) {
        const FlatJson row(item);
        Event event;
        event.source = "智慧树";
        event.title = row.getString("title");
        if (event.title.empty()) {
            event.title = row.getString("examName");
        }
        if (event.title.empty()) {
            continue;
        }
        const std::string kind = row.getString("kind");
        const std::string startText = row.getString("start");
        const std::string deadlineText = row.getString("deadline");
        event.start = parseTime(startText);
        event.end = parseTime(deadlineText);
        if (event.start == 0 && event.end != 0) {
            event.start = event.end;
        }
        event.kind = (event.start != 0 && event.start != event.end)
                         ? Kind::Schedule
                         : Kind::Deadline;
        event.course = row.getString("course");
        event.url = row.getString("url");
        event.done = row.getBool("completed");
        if (event.start != 0 || event.end != 0) {
            result.events.push_back(event);
        }
    }
    return result;
}

void syncAll(const SyncConfig& config, std::shared_ptr<bp::EventStore> store,
             std::function<void(const std::string&, bool)> onPlatform) {
    const SyncResult chaoxing = syncChaoxing(config);
    for (const Event& event : chaoxing.events) {
        store->add(event);
    }
    if (onPlatform) {
        onPlatform(chaoxing.platform, chaoxing.error.empty());
    }

    const SyncResult zhs = syncZhihuishu(config);
    for (const Event& event : zhs.events) {
        store->add(event);
    }
    if (onPlatform) {
        onPlatform(zhs.platform, zhs.error.empty());
    }
}

}  // namespace bp::api
