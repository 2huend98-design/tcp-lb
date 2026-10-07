#include "reactor/event_loop.hpp"

#include <sys/eventfd.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>

#include "common/logger.hpp"

EventLoop::EventLoop()
    : epoll_(std::make_unique<Poller>()),
      thread_id_(std::this_thread::get_id()),
      quit_(false),
      wakeup_fd_(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)),
      wakeup_channel_(std::make_unique<Channel>(this, wakeup_fd_)) {
    ready_channels_.reserve(Poller::kMaxEvents);

    if (wakeup_fd_ < 0) {
        throw std::runtime_error("Eventloop: Failed to create eventfd");
    }

    wakeup_channel_->SetReadCallback([this] { HandleWakeup(); });
    wakeup_channel_->EnableReading();
}

EventLoop::~EventLoop() {
    // 析构可能发生在创建线程或主线程，此时 loop 线程已退出，
    // 将当前线程标记为 loop 线程，避免 AssertInLoopThread 误报
    thread_id_ = std::this_thread::get_id();

    wakeup_channel_->ClearCallbacks();
    wakeup_channel_->DisableAll();
    if (wakeup_fd_ >= 0) {
        close(wakeup_fd_);
    }
}

void EventLoop::Loop() {
    thread_id_ = std::this_thread::get_id();

    while (!quit_ || active_sessions_.load(std::memory_order_acquire) > 0) {
        if (before_iteration_cb_) {
            before_iteration_cb_();
        }

        epoll_->Poll(ready_channels_, 1000);

        for (Channel* ch : ready_channels_) {
            ch->HandleEvent();
        }

        is_processing_tasks_ = true;
        ProcessTasks();
        is_processing_tasks_ = false;
    }
}

void EventLoop::Quit() {
    // 只首次唤醒：before_iteration_cb_ 每轮调 Quit()，
    // 无条件 Wakeup 会让 poll 永不阻塞、空转烧 CPU
    if (!quit_.exchange(true, std::memory_order_acq_rel)) {
        Wakeup();
    }
}

void EventLoop::UpdateChannel(Channel* ch) {
    AssertInLoopThread();
    int fd = ch->Fd();
    if (ch->Events() != 0) {
        if (registered_fds_.count(fd) == 0) {
            epoll_->AddChannel(ch, ch->Events());
            registered_fds_.insert(fd);
        } else {
            epoll_->UpdateChannel(ch, ch->Events());
        }
    } else {
        if (registered_fds_.count(fd)) {
            epoll_->RemoveChannel(ch);
            registered_fds_.erase(fd);
        }
    }
}

void EventLoop::AssertInLoopThread() {
    if (!IsInLoopThread()) {
        throw std::runtime_error("Not in loop thread");
    }
}

bool EventLoop::IsInLoopThread() const {
    return thread_id_ == std::this_thread::get_id();
}

void EventLoop::QueueTask(std::function<void()> cb) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push_back(std::move(cb));
    }

    // 如果不在 loop 线程，或者 loop 正在处理任务，需要唤醒，避免新任务被延迟到下一轮 poll
    if (!IsInLoopThread() || is_processing_tasks_.load(std::memory_order_acquire)) {
        Wakeup();
    }
}

void EventLoop::RunTask(std::function<void()> cb) {
    if (IsInLoopThread()) {
        cb();
    } else {
        QueueTask(std::move(cb));
    }
}

void EventLoop::ProcessTasks() {
    std::vector<std::function<void()>> tasks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks.swap(tasks_);
    }

    for (const auto& task : tasks) {
        task();
    }
}

void EventLoop::Wakeup() {
    uint64_t one = 1;
    ssize_t n = ::write(wakeup_fd_, &one, sizeof(uint64_t));
    if (n != sizeof(uint64_t)) {
        LOG_ERROR("EventLoop::Wakeup() failed, error: " + std::string(std::strerror(errno)));
    }
}

void EventLoop::HandleWakeup() {
    uint64_t one = 0;
    // 读到 EAGAIN，确保后续 write 产生新边沿
    while (::read(wakeup_fd_, &one, sizeof(uint64_t)) > 0) {
    }
}