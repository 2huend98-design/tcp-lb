#pragma once

#include "reactor/channel.hpp"
#include "reactor/poller.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

class EventLoop {
public:
    EventLoop();
    ~EventLoop() noexcept;

    void Loop();
    void Quit();

    void UpdateChannel(Channel* ch);

    // 会话计数；Loop() 退出条件之一
    void AddSession() { active_sessions_.fetch_add(1, std::memory_order_release); }
    void RemoveSession() { active_sessions_.fetch_sub(1, std::memory_order_release); }

    void AssertInLoopThread();
    bool IsInLoopThread() const;

    void QueueTask(std::function<void()> cb);
    void RunTask(std::function<void()> cb);

    // 每轮 Loop 前调用
    void SetBeforeIterationCallback(std::function<void()> cb) {
        before_iteration_cb_ = std::move(cb);
    }

private:
    void ProcessTasks();
    void Wakeup();
    void HandleWakeup();

    std::unique_ptr<Poller> epoll_;
    std::unordered_set<int> registered_fds_;   // 避免重复注册
    std::vector<Channel*> ready_channels_;

    std::thread::id thread_id_;
    std::atomic<int> active_sessions_{0};
    std::atomic<bool> quit_;

    int wakeup_fd_;
    std::unique_ptr<Channel> wakeup_channel_;

    std::vector<std::function<void()>> tasks_;
    std::atomic<bool> is_processing_tasks_{false};
    std::mutex mutex_;

    std::function<void()> before_iteration_cb_;
};