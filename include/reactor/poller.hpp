#pragma once

#include <sys/epoll.h>

#include <vector>

class Channel;

class Poller {
public:
    static constexpr int kMaxEvents = 4096;

    Poller();
    ~Poller() noexcept;

    void AddChannel(Channel* ch, int events);
    void UpdateChannel(Channel* ch, int events);
    void RemoveChannel(Channel* ch);

    void Poll(std::vector<Channel*>& active_channels, int timeout_ms);

private:
    int epoll_fd_;
    std::vector<epoll_event> events_;
};