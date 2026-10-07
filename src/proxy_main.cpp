#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <unistd.h>
#include <vector>

#include "acceptor/acceptor.hpp"
#include "balancer/backend_pool.hpp"
#include "balancer/balancer.hpp"
#include "common/logger.hpp"
#include "config/config.hpp"
#include "health/health_checker.hpp"
#include "proxy/proxy_session.hpp"
#include "proxy/tcp_connection.hpp"
#include "reactor/event_loop.hpp"

static void OnStats(int) {
    char buf[64];
    ssize_t n = snprintf(buf, sizeof buf, "[stats] alive=%d\n", TcpConnection::AliveCount());
    n = write(STDERR_FILENO, buf, static_cast<size_t>(n));
    (void)n;
}

EventLoop* g_main_loop = nullptr;
Acceptor* g_acceptor = nullptr;
std::atomic<bool> g_proxy_stop{false};

void SignalHandler(int) {
    if (!g_proxy_stop.exchange(true)) {
        if (g_main_loop) g_main_loop->Quit();
    } else {
        static const char msg[] = "[shutdown] ForceQuit: hard exit\n";
        ssize_t n = ::write(STDERR_FILENO, msg, sizeof(msg) - 1);
        (void)n;
        std::_Exit(0);
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <config_file>" << std::endl;
        return 1;
    }

    ::signal(SIGUSR1, OnStats);

    Config cfg = load_config(argv[1]);
    ::signal(SIGPIPE, SIG_IGN);
    ::signal(SIGINT, SignalHandler);
    ::signal(SIGTERM, SignalHandler);

    std::unique_ptr<BaseBalancer> balancer;
    if (cfg.balancer == "least_connections")
        balancer = std::make_unique<LeastConnectionsBalancer>();
    else
        balancer = std::make_unique<RoundRobinBalancer>();
    BackendPool backend_pool(std::move(balancer));
    for (const auto& bc : cfg.backends) backend_pool.AddBackend(bc.address, bc.port);
    HealthChecker checker(&backend_pool, cfg.health_check_interval_sec, cfg.health_check_timeout_sec);
    checker.Start();

    TcpConnection::SetOutputBufferCap(
        static_cast<size_t>(cfg.output_buffer_kb) * 1024);
    LOG_INFO("output_buffer=" << cfg.output_buffer_kb << "KB (bit_ceil 后 "
             << TcpConnection::OutputBufferCap() / 1024 << "KB)");

    int kWorkerThreads = cfg.worker_threads;
    if (const char* e = ::getenv("TCP_WORKERS")) {
        int n = atoi(e);
        if (n >= 1 && n <= 64) kWorkerThreads = n;
    }
    std::vector<std::unique_ptr<EventLoop>> worker_loops;
    std::vector<std::thread> worker_threads;

    for (int i = 0; i < kWorkerThreads; ++i) {
        auto loop = std::make_unique<EventLoop>();
        EventLoop* raw = loop.get();
        worker_loops.push_back(std::move(loop));
        worker_threads.emplace_back([raw]() { raw->Loop(); });
    }

    EventLoop main_loop;
    g_main_loop = &main_loop;
    Acceptor acceptor(&main_loop, cfg.listen_port);
    acceptor.Start();
    g_acceptor = &acceptor;

    main_loop.SetBeforeIterationCallback([&]() {
        if (g_proxy_stop.load()) {
            if (g_acceptor) { g_acceptor->Stop(); g_acceptor = nullptr; }
        }
    });

    std::atomic<int> next_worker{0};
    acceptor.SetNewConnectionCallback([&](int client_fd) {
        int idx = next_worker.fetch_add(1) % kWorkerThreads;
        EventLoop* worker = worker_loops[idx].get();
        worker->RunTask([worker, client_fd, &backend_pool]() {
            auto session = std::make_shared<ProxySession>(worker, client_fd, &backend_pool);
            session->Start();
        });
    });

    LOG_INFO("TCP Proxy started on port " << cfg.listen_port);
    main_loop.Loop();
    LOG_INFO("TCP Proxy stopping... (graceful drain)");

    acceptor.Stop();
    for (auto& loop : worker_loops) loop->Quit();
    for (auto& t : worker_threads) if (t.joinable()) t.join();
    checker.Stop();
    LOG_INFO("TCP Proxy stopped.");
    return 0;
}
