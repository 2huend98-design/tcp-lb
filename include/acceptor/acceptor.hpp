#pragma once

#include <cstdint>
#include <functional>

#include "reactor/channel.hpp"

class EventLoop;

class Acceptor {
public:
    Acceptor(EventLoop *loop, uint16_t port);
    ~Acceptor() noexcept;

    void Start();
    void Stop();

    void SetNewConnectionCallback(std::function<void(int sockfd)> cb);

private:
    static int CreateListenSocket(uint16_t port);
    void OnAccept();

    EventLoop *loop_;
    int listen_fd_;
    Channel channel_;
    std::function<void(int sockfd)> conn_cb_;
};