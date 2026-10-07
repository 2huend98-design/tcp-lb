#pragma once

#include <sys/epoll.h>

#include <functional>

class EventLoop;

class Channel {
public:
    Channel(EventLoop *loop, int fd);
    ~Channel() noexcept;

    void EnableReading();
    void EnableWriting();
    void DisableReading();
    void DisableWriting();
    void DisableAll();

    void HandleEvent();

    int Fd() const { return fd_; }
    uint32_t Events() const { return events_; }
    void SetFd(int fd) { fd_ = fd; }
    void SetReadyEvent(uint32_t revents) { revents_ = revents; }

    void SetReadCallback(std::function<void()> cb);
    void SetWriteCallback(std::function<void()> cb);
    void SetCloseCallback(std::function<void()> cb);
    void ClearCallbacks();

private:
    void Update();
    void SetEvents(uint32_t events);

    EventLoop *loop_;
    int fd_;
    uint32_t events_;
    uint32_t revents_;

    std::function<void()> read_cb_;
    std::function<void()> write_cb_;
    std::function<void()> close_cb_;
};