// api/http_server.hpp —— 本机 HTTP 外接接口（接口层契约）
//
// 仅绑定 127.0.0.1，不暴露到局域网。端口传 0 时由系统自动分配，
// 可通过 port() 取回实际端口。
//
//   POST /events  Content-Type: application/json
//       请求体为 json_io.hpp 约定的单个对象或对象数组；整批校验通过后
//       才写入 EventStore，返回 {"added":N}。
//   GET /events   导出全部事件 JSON 数组。
//   GET /health   返回 {"ok":true}。
//
// 非法 JSON/字段映射为 400；未知路径为 404；方法不匹配为 405。
// 请求头最大 16 KiB，请求体最大 1 MiB，连接读写超时 5 秒。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "core/store.hpp"

namespace bp::api {

class HttpServer {
public:
    explicit HttpServer(std::uint16_t port = 8765);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // 启动监听与后台服务线程；onChange 在成功提交后于服务线程执行，
    // UI 层应仅在回调里 PostMessage，不得直接操作 D2D 资源。
    bool start(std::shared_ptr<bp::EventStore> store,
               std::function<void()> onChange = {});

    // 幂等停止；关闭监听并等待后台线程退出。
    void stop();

    bool running() const;
    std::uint16_t port() const;
    std::string lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace bp::api
