#pragma once

#include <memory>

#include "proxy/tcp_connection.hpp"
#include "reactor/channel.hpp"

class EventLoop;
class Backend;
class BackendPool;

class ProxySession : public std::enable_shared_from_this<ProxySession> {
public:
    ProxySession(EventLoop *loop, int client_fd, BackendPool* backend_pool);
    ~ProxySession();

    void Start();

private:
    void ConnectBackend();
    void OnBackendConnected();
    void CloseBoth();

    // 建连超时兜底：后端黑洞时不会有 EPOLLOUT，事件表达不了，只能用计时器
    void ArmConnectTimeout();
    void DisarmConnectTimeout();
    void OnConnectTimeout();

    // eof/write-complete/close 共同驱动，不轮询
    void CheckCondition();

    static constexpr int kConnectTimeoutSec = 3;

    EventLoop *loop_;
    Backend* backend_info_;
    BackendPool* backend_pool_;

    std::shared_ptr<TcpConnection> client_conn_;

    int backend_conn_fd_ = -1;
    std::shared_ptr<Channel> backend_conn_channel_;
    std::shared_ptr<TcpConnection> backend_conn_;

    int connect_tfd_ = -1;
    std::shared_ptr<Channel> connect_timer_ch_;

    bool closed_ = false;
};