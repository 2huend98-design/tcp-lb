#include "balancer/balancer.hpp"

Backend* RoundRobinBalancer::GetBackend(std::deque<Backend>& pool) {
    if (pool.empty()) return nullptr;

    size_t len = pool.size();
    size_t curr = index_;

    for (size_t i = 0; i < len; ++i) {
        Backend& backend = pool[curr];
        if (backend.is_available.load(std::memory_order_acquire)) {
            index_ = (curr + 1) % len;
            return &backend;
        }
        curr = (curr + 1) % len;
    }

    return nullptr;
}

Backend* LeastConnectionsBalancer::GetBackend(std::deque<Backend>& pool) {
    if (pool.empty()) return nullptr;

    Backend* best = nullptr;
    for (Backend& backend : pool) {
        if (backend.is_available.load(std::memory_order_acquire)) {
            if (!best || backend.active_connections < best->active_connections) {
                best = &backend;
            }
        }
    }

    return best;
}