#pragma once

#include "balancer/backend_pool.hpp"

#include <atomic>
#include <thread>

class HealthChecker {
public:
    HealthChecker(BackendPool* backend_pool, int interval_sec = 3, int timeout_sec = 1);
    ~HealthChecker();

    void Start();
    void Stop();

private:
    void Run();
    bool TcpProbe(const std::string &address, uint16_t port);

    BackendPool* backend_pool_;
    std::unique_ptr<std::thread> thread_;
    std::atomic<bool> running_;
    int interval_sec_;
    int timeout_sec_;
};