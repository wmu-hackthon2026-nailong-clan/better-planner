#include "api/desktop_connector.hpp"

#include <windows.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>

namespace bp::api {
namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;

std::wstring wide(const std::string& text) {
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(length, L'\0');
    if (length) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

fs::path projectRoot() {
    wchar_t filename[32768] = {};
    GetModuleFileNameW(nullptr, filename, 32768);
    auto directory = fs::path(filename).parent_path();
    for (int i = 0; i < 5; ++i) {
        if (fs::exists(directory / "app" / "desktop_service.py")) return directory;
        directory = directory.parent_path();
    }
    throw std::runtime_error("Missing application files");
}

fs::path pythonPath(const fs::path& root) {
    Json cfg = Json::object();
    std::ifstream input(root / "config.local.json");
    if (input) input >> cfg;
    if (cfg.contains("desktop_python") && cfg["desktop_python"].is_string()) {
        auto path = fs::path(wide(cfg["desktop_python"].get<std::string>()));
        return path.is_absolute() ? path : root / path;
    }
    for (const auto& path : {root / "runtime" / "python.exe", root / ".deps" / "connectors-env" / "Scripts" / "python.exe"}) {
        if (fs::exists(path)) return path;
    }
    throw std::runtime_error("Missing Python runtime");
}
}

struct DesktopConnector::Impl {
    std::shared_ptr<EventStore> store;
    std::function<void()> onChange;
    std::thread thread;
    std::atomic<bool> stopping{false}, alive{false}, pendingOpen{false};
    mutable std::mutex mutex;
    std::wstring url, message = L"点击“连接与同步”登录学习通或智慧树";

    void notify(std::wstring text) {
        { std::lock_guard lock(mutex); message = std::move(text); }
        if (!stopping && onChange) onChange();
    }

    void receive(const std::string& line) {
        const auto data = Json::parse(line);
        if (data.at("type") == "ready") {
            auto address = data.at("url").get<std::string>();
            const std::string prefix = "http://127.0.0.1:";
            if (!address.starts_with(prefix) || address.back() != '/') throw std::runtime_error("Invalid service URL");
            const auto port = address.substr(prefix.size(), address.size() - prefix.size() - 1);
            if (port.empty() || port.find_first_not_of("0123456789") != std::string::npos || std::stoul(port) > 65535) throw std::runtime_error("Invalid service port");
            { std::lock_guard lock(mutex); url = wide(address); }
            notify(L"连接服务已就绪 · 点击“连接与同步”登录平台");
            return;
        }
        if (data.at("type") != "state") return;
        std::vector<Event> events;
        for (const auto& row : data.at("events")) {
            Event e;
            e.externalId = row.at("id").get<std::string>();
            e.source = row.at("source").get<std::string>();
            if (e.source != "学习通" && e.source != "智慧树") throw std::runtime_error("Invalid event source");
            e.kind = row.at("kind") == "schedule" ? Kind::Schedule : Kind::Deadline;
            e.start = row.at("start").get<std::int64_t>();
            e.end = row.at("end").get<std::int64_t>();
            if (e.end < e.start) throw std::runtime_error("Invalid event range");
            e.title = row.at("title").get<std::string>();
            e.course = row.value("course", "");
            e.location = row.value("location", "");
            e.teacher = row.value("teacher", "");
            e.url = row.value("url", "");
            e.done = row.value("done", false);
            events.push_back(std::move(e));
        }
        store->mergeImported(std::move(events));
        bool failed = false;
        for (const auto& row : data.at("sync")) if (!row.at("error").is_null()) failed = true;
        std::wstring text = data.value("running", false) ? L"平台同步中…可在连接页查看进度" :
            (failed ? L"部分同步未成功 · 点击“连接与同步”查看原因并重试" :
            L"已载入 " + std::to_wstring(data.at("events").size()) + L" 项平台日程 · 连接页可登录与同步");
        if (data.value("undated", 0) > 0) text += L" · 无日期任务请在连接页查看";
        notify(text);
    }

    void run() {
        try {
            const auto root = projectRoot();
            const auto python = pythonPath(root);
            SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
            Handle read, write, nullFile, job, process, primaryThread;
            if (!CreatePipe(&read.value, &write.value, &sa, 0)) throw std::runtime_error("Pipe failed");
            SetHandleInformation(read.value, HANDLE_FLAG_INHERIT, 0);
            nullFile.value = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
            if (nullFile.value == INVALID_HANDLE_VALUE) throw std::runtime_error("NUL failed");
            job.value = CreateJobObjectW(nullptr, nullptr);
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) throw std::runtime_error("Job failed");
            std::wstring command = L"\"" + python.wstring() + L"\" -X utf8 -u -m app.desktop_service";
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdOutput = write.value;
            startup.hStdError = nullFile.value;
            startup.hStdInput = nullFile.value;
            PROCESS_INFORMATION info{};
            if (!CreateProcessW(python.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, root.c_str(), &startup, &info)) {
                notify(L"连接服务无法启动（Windows 错误 " + std::to_wstring(GetLastError()) + L"）· 请检查 runtime 或应用控制策略");
                alive = false;
                return;
            }
            process.value = info.hProcess;
            primaryThread.value = info.hThread;
            if (!AssignProcessToJobObject(job.value, process.value)) {
                TerminateProcess(process.value, 1);
                throw std::runtime_error("Job assignment failed");
            }
            ResumeThread(primaryThread.value);
            CloseHandle(write.value); write.value = nullptr;
            std::string buffer;
            const auto deadline = GetTickCount64() + 20000;
            bool ready = false;
            while (!stopping) {
                DWORD available = 0;
                if (!PeekNamedPipe(read.value, nullptr, 0, nullptr, &available, nullptr)) break;
                if (available) {
                    char bytes[8192]; DWORD count = 0;
                    if (!ReadFile(read.value, bytes, sizeof(bytes), &count, nullptr)) break;
                    buffer.append(bytes, count);
                    if (buffer.size() > 16 * 1024 * 1024) throw std::runtime_error("Snapshot too large");
                    std::size_t end;
                    while ((end = buffer.find('\n')) != std::string::npos) {
                        receive(buffer.substr(0, end));
                        buffer.erase(0, end + 1);
                        ready = true;
                    }
                } else if (WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0) break;
                else Sleep(30);
                if (!ready && GetTickCount64() > deadline) throw std::runtime_error("Startup timed out");
            }
            if (!stopping) notify(L"连接服务已停止 · 点击“连接与同步”重新启动");
        } catch (...) {
            notify(L"连接服务未就绪 · 请运行 scripts/setup_desktop.py 并检查 config.local.json");
        }
        { std::lock_guard lock(mutex); url.clear(); }
        alive = false;
    }
};

DesktopConnector::DesktopConnector(std::shared_ptr<EventStore> store, std::function<void()> onChange) : impl_(std::make_unique<Impl>()) {
    impl_->store = std::move(store);
    impl_->onChange = std::move(onChange);
}
DesktopConnector::~DesktopConnector() { stop(); }
void DesktopConnector::start() {
    if (impl_->alive) return;
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_->stopping = false;
    impl_->alive = true;
    impl_->thread = std::thread([this] { impl_->run(); });
}
void DesktopConnector::requestOpen() { impl_->pendingOpen = true; start(); if (impl_->onChange) impl_->onChange(); }
std::wstring DesktopConnector::takeOpenUrl() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->url.empty() || !impl_->pendingOpen.exchange(false)) return L"";
    return impl_->url;
}
std::wstring DesktopConnector::status() const { std::lock_guard lock(impl_->mutex); return impl_->message; }
void DesktopConnector::stop() { impl_->stopping = true; if (impl_->thread.joinable()) impl_->thread.join(); }
}
