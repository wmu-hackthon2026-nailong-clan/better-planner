// api/sync.hpp —— 学习通/智慧树平台同步（接口层契约）
//
// 同步复用旧 Python 版课期 Keqi 的只读连接器脚本（不重写爬虫）：
//   * 学习通：scripts/xxt_readonly.py，输出 {"date","start","end","name",
//     "location","teacher"} 课表行，或 {"external_id","kind","title",
//     "course","start","deadline","completed","url"} 作业/考试；
//   * 智慧树：scripts/zhs_readonly.py，输出 {"events":[...],"errors":[...]}，
//     每条含 kind/title/course/start/deadline/completed/url。
//
// 连接器路径从项目根 config.local.json 读取（缺失时用默认命令），
// 子进程通过 CreateProcess 启动、stdout 捕获为 JSON，再归一化为 Event。
// 同步是同步阻塞操作，UI 层应放到后台线程调用，完成后经消息刷新。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/event.hpp"
#include "core/store.hpp"

namespace bp::api {

// 单条同步结果（用于 UI 提示）。
struct SyncResult {
    std::string platform;              // "学习通" / "智慧树"
    std::vector<Event> events;         // 归一化后的事件
    std::string error;                 // 非空表示该平台同步失败
    std::vector<std::string> warnings; // 部分课程失败等非致命提示
};

// 读取 config.local.json 中的连接器配置。
struct SyncConfig {
    std::string python;                // 运行脚本的 python 解释器
    std::string xxtScript;             // 学习通脚本绝对路径
    std::string zhsScript;             // 智慧树脚本绝对路径
    int timeoutSeconds = 300;          // 单平台超时
};

// 解析项目根目录的 config.local.json（不存在时回退默认配置）。
SyncConfig loadSyncConfig();

// 同步学习通：调用 xxt_readonly.py 拉取课表/作业/考试。
SyncResult syncChaoxing(const SyncConfig& config);

// 同步智慧树：调用 zhs_readonly.py 拉取作业/考试。
SyncResult syncZhihuishu(const SyncConfig& config);

// 解析智慧树 events 协议 JSON（书签脚本导出的文件或连接器输出），
// 归一化为 Event 列表；供「导入智慧树考试 JSON」复用。
std::vector<Event> parseZhsEvents(const std::string& json);

// 依次同步两个平台，把结果写入 store，进度经 onPlatform 回调。
// 返回各平台汇总；调用方负责线程与 UI 刷新。
void syncAll(const SyncConfig& config, std::shared_ptr<bp::EventStore> store,
             std::function<void(const std::string&, bool)> onPlatform = {});

}  // namespace bp::api
