#include "reactor/channel.hpp"

#include <cassert>

#include "common/logger.hpp"
#include "reactor/event_loop.hpp"

Channel::Channel(EventLoop *loop, int fd)
    : loop_(loop), fd_(fd), events_(0), revents_(0) {
    events_ |= EPOLLET;
}

Channel::~Channel() {
    ClearCallbacks();
    if (events_ != 0) {
        LOG_ERROR("Channel destroyed without DisableAll, fd=" << fd_ << " events=" << events_);
        assert(false);
    }
}

void Channel::EnableReading() {
    SetEvents(events_ | (EPOLLIN | EPOLLRDHUP));
}

void Channel::EnableWriting() {
    SetEvents(events_ | EPOLLOUT);
}

void Channel::DisableReading() {
    SetEvents(events_ & ~(EPOLLIN | EPOLLRDHUP));
}

void Channel::DisableWriting() {
    SetEvents(events_ & ~EPOLLOUT);
}

void Channel::DisableAll() {
    SetEvents(0);
}

void Channel::HandleEvent() {
    if (revents_ & EPOLLERR) {
        if (close_cb_) {
            close_cb_();
        } else {
            if (fd_ < 0) {
                LOG_INFO("Channel::HandleEvent got error but no close callback, fd=" << fd_ << " events=" << revents_);
            } else {
                LOG_ERROR("Channel::HandleEvent got error but no close callback, fd=" << fd_ << " events=" << revents_);
            }
        }
        return;
    }
    if ((revents_ & (EPOLLIN | EPOLLRDHUP)) && read_cb_) {
        read_cb_();
    }
    if ((revents_ & EPOLLOUT) && write_cb_) {
        write_cb_();
    }
    if ((revents_ & EPOLLHUP) && close_cb_) {
        close_cb_();
    }
}

void Channel::SetReadCallback(std::function<void()> cb) {
    read_cb_ = std::move(cb);
}

void Channel::SetWriteCallback(std::function<void()> cb) {
    write_cb_ = std::move(cb);
}

void Channel::SetCloseCallback(std::function<void()> cb) {
    close_cb_ = std::move(cb);
}

void Channel::ClearCallbacks() {
    read_cb_ = nullptr;
    write_cb_ = nullptr;
    close_cb_ = nullptr;
}

void Channel::Update() {
    loop_->UpdateChannel(this);
}

void Channel::SetEvents(uint32_t events) {
    if (events == events_) return;
    events_ = events;
    Update();
}