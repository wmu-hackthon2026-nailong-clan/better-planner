// api/registry.hpp —— class 注册表（层间 public 接口互联的枢纽）
//
// 需求原文（张励志，2026-11-01 群聊）："分别注册 class，通过 public
// 接口互联"。各层实现类在此注册，跨层只拿这里发出的引用/指针，
// 禁止直接构造他层对象。
//
// 首版提供数据层两个核心服务的获取口；后续新增服务（同步器、
// 导入导出器等）在 provide 系列方法后追加。
#pragma once

#include <memory>

#include "core/layout.hpp"
#include "core/store.hpp"

namespace bp::api {

class Registry {
public:
    // 进程级单例。UI 层与接口层均从这里取依赖。
    static Registry& instance();

    // 注册/替换数据层事件存储（首次取用前必须注册）。
    void provideStore(std::shared_ptr<bp::EventStore> store);

    // 注册/替换排布引擎；未注册时 getLayoutEngine 自动返回默认实例。
    void provideLayoutEngine(std::shared_ptr<bp::LayoutEngine> engine);

    // 获取事件存储；未注册时抛出 std::runtime_error。
    std::shared_ptr<bp::EventStore> store() const;

    // 获取排布引擎；未注册时内部新建默认实例并缓存。
    std::shared_ptr<bp::LayoutEngine> layoutEngine() const;

private:
    Registry() = default;
    std::shared_ptr<bp::EventStore> store_;
    std::shared_ptr<bp::LayoutEngine> layout_;
};

}  // namespace bp::api
