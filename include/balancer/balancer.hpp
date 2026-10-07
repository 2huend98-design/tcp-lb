#pragma once

#include <cstddef>
#include <deque>

#include "balancer/backend.hpp"

class BaseBalancer {
public:
    virtual ~BaseBalancer() = default;
    virtual Backend* GetBackend(std::deque<Backend>& pool) = 0;
};

// index_ 由 BackendPool 锁保护
class RoundRobinBalancer : public BaseBalancer {
public:
    Backend* GetBackend(std::deque<Backend>& pool) override;

private:
    size_t index_ = 0;
};

class LeastConnectionsBalancer : public BaseBalancer {
public:
    Backend* GetBackend(std::deque<Backend>& pool) override;
};