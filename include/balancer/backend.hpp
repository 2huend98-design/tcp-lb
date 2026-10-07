#pragma once

#include <atomic>
#include <cstdint>
#include <string>

struct Backend {
    const std::string address;
    const uint16_t port;

    std::atomic<bool> is_available;  // 健康检查线程写，故用 atomic
    uint32_t active_connections;     // BackendPool 锁保护

    Backend(const std::string& addr, uint16_t port)
        : address(addr), port(port), is_available(true), active_connections(0) {}
};