#include "api/registry.hpp"

#include <stdexcept>

namespace bp::api {

Registry& Registry::instance() {
    static Registry registry;
    return registry;
}

void Registry::provideStore(std::shared_ptr<bp::EventStore> store) {
    store_ = std::move(store);
}

void Registry::provideLayoutEngine(std::shared_ptr<bp::LayoutEngine> engine) {
    layout_ = std::move(engine);
}

std::shared_ptr<bp::EventStore> Registry::store() const {
    if (!store_) {
        throw std::runtime_error("EventStore not registered");
    }
    return store_;
}

std::shared_ptr<bp::LayoutEngine> Registry::layoutEngine() const {
    if (!layout_) {
        return std::make_shared<bp::LayoutEngine>();
    }
    return layout_;
}

}  // namespace bp::api
