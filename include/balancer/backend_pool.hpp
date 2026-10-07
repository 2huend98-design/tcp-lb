#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include "balancer/backend.hpp"
#include "balancer/balancer.hpp"

class BackendPool {
public:
    explicit BackendPool(std::unique_ptr<BaseBalancer> balancer);

    // 仅初始化阶段调用
    void AddBackend(const std::string& address, uint16_t port);
    Backend* GetBackend();
    void ReleaseBackend(Backend* backend);
    void SetBackendAvailability(const std::string& addr, uint16_t port, bool available);

    const std::deque<Backend>& GetBackends() const { return backends_; }

private:
    std::deque<Backend> backends_;
    std::unique_ptr<BaseBalancer> balancer_;
    std::mutex mutex_;   // 保护 active_connections 与 GetBackend
};