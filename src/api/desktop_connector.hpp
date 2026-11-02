#pragma once

#include <functional>
#include <memory>
#include <string>

#include "core/store.hpp"

namespace bp::api {

// 启动并拥有本地 Python 登录服务；端口由系统分配，不占用外部 HTTP API 的端口。
// 密码只经本机浏览器提交，标准输出只接收就绪消息、日历快照与同步摘要。
// onChange 在后台线程执行，调用方只能投递窗口消息；析构会停止并等待服务线程。
class DesktopConnector {
public:
    DesktopConnector(std::shared_ptr<EventStore> store, std::function<void()> onChange);
    ~DesktopConnector();
    DesktopConnector(const DesktopConnector&) = delete;
    DesktopConnector& operator=(const DesktopConnector&) = delete;

    // 启动本机连接服务并恢复已保存的日历；不会自动登录或访问教学平台。
    void start();
    // 请求打开连接页；服务未启动时先启动。URL 由主线程 takeOpenUrl 后打开。
    void requestOpen();
    std::wstring takeOpenUrl();
    std::wstring status() const;
    // 关闭窗口前调用，确保后台不再向已销毁窗口投递消息。
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bp::api
