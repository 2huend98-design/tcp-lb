#include <csignal>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <iostream>
#include <memory>
#include <vector>

#include <unistd.h>

#include "acceptor/acceptor.hpp"
#include "proxy/tcp_connection.hpp"
#include "common/logger.hpp"
#include "reactor/channel.hpp"
#include "reactor/event_loop.hpp"

static void OnStats(int) {
    char buf[64];
    ssize_t n = snprintf(buf, sizeof buf, "[stats] alive=%d\n", TcpConnection::AliveCount());
    n = write(STDERR_FILENO, buf, static_cast<size_t>(n));
    (void)n;
}

EventLoop* g_loop = nullptr;
std::vector<std::shared_ptr<TcpConnection>> g_connections;

void SignalHandler(int) { if (g_loop) g_loop->Quit(); }

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <port>" << std::endl;
        return 1;
    }
    ::signal(SIGUSR1, OnStats);
    uint16_t port = static_cast<uint16_t>(std::stoi(argv[1]));
    ::signal(SIGPIPE, SIG_IGN);
    ::signal(SIGINT, SignalHandler);

    EventLoop loop;
    g_loop = &loop;

    Acceptor acceptor(&loop, port);
    acceptor.Start();

    acceptor.SetNewConnectionCallback([&loop](int client_fd) {
        auto conn = std::make_shared<TcpConnection>(&loop, client_fd);
        conn->PipeTo(conn.get());
        std::weak_ptr<TcpConnection> weak_conn = conn;
        conn->SetEofCallback([weak_conn]() {
            if (auto c = weak_conn.lock()) {
                c->Shutdown();
                if (c->IsOutputBufferEmpty()) c->Close();
            }
        });
        conn->SetWriteCompleteCallback([weak_conn]() {
            if (auto c = weak_conn.lock()) {
                if (c->IsEof() && c->IsOutputBufferEmpty()) c->Close();
            }
        });
        conn->SetCloseCallback([&loop](TcpConnection&) {
            loop.QueueTask([]() {
                g_connections.erase(
                    std::remove_if(g_connections.begin(), g_connections.end(),
                        [](const std::shared_ptr<TcpConnection>& p) { return p->GetFd() < 0; }),
                    g_connections.end());
            });
        });
        conn->Start();
        g_connections.push_back(conn);
    });

    LOG_INFO("Echo Server started on port " << port);

    loop.Loop();
    acceptor.Stop();

    auto connections_copy = g_connections;
    for (auto& conn : connections_copy) if (conn) conn->Close();
    g_connections.clear();
    LOG_INFO("Echo Server stopped.");
    return 0;
}
