#include "balancer/backend_pool.hpp"

#include "balancer/backend.hpp"
#include "balancer/balancer.hpp"
#include "common/logger.hpp"

BackendPool::BackendPool(std::unique_ptr<BaseBalancer> balancer)
    : balancer_(std::move(balancer)) {}

void BackendPool::AddBackend(const std::string& addr, uint16_t port) {
    backends_.emplace_back(addr, port);
}

Backend* BackendPool::GetBackend() {
    std::lock_guard<std::mutex> lock(mutex_);

    Backend* backend = balancer_->GetBackend(backends_);
    if (backend) {
        backend->active_connections++;
    }
    return backend;
}

void BackendPool::ReleaseBackend(Backend* backend) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (backend) {
        backend->active_connections--;
    }
}

void BackendPool::SetBackendAvailability(const std::string& addr, uint16_t port, bool available) {
    // backends_ 初始化后只读；is_available 是 atomic，此处无需加锁
    for (auto& backend : backends_) {
        if (backend.address == addr && backend.port == port) {
            backend.is_available.store(available, std::memory_order_release);
            break;
        }
    }
}