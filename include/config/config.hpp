#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct BackendConfig {
    std::string address;
    uint16_t port;
};

struct Config {
    uint16_t listen_port = 8080;
    std::vector<BackendConfig> backends;
    std::string balancer = "round_robin";
    int health_check_interval_sec = 3;
    int health_check_timeout_sec = 1;

    // worker_threads: reactor 线程数（TCP_WORKERS 仅限诊断）
    // output_buffer_kb: 每连接输出缓冲上限 KB（RingBuffer 向上取 2 的幂）
    int worker_threads = 4;
    int output_buffer_kb = 512;
};

Config load_config(const std::string& filename);