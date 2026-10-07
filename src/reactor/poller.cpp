#include "reactor/poller.hpp"

#include <cerrno>
#include <cstring>
#include <unistd.h>

#include <stdexcept>
#include <string>

#include "common/logger.hpp"
#include "reactor/channel.hpp"

Poller::Poller() : events_(kMaxEvents) {
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ == -1) {
        throw std::runtime_error("Failed to create epoll file descriptor");
    }
}

Poller::~Poller() noexcept {
    if (epoll_fd_ != -1) {
        close(epoll_fd_);
    }
}

void Poller::AddChannel(Channel* ch, int events) {
    struct epoll_event ev {};
    ev.events = events;
    ev.data.ptr = ch;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, ch->Fd(), &ev) == -1) {
        LOG_ERROR("AddChannel failed, fd=" << ch->Fd()
                  << ", errno=" << errno << " (" << std::strerror(errno) << ")");
        throw std::runtime_error("Poller::AddChannel failed");
    }
}

void Poller::UpdateChannel(Channel* ch, int events) {
    struct epoll_event ev {};
    ev.events = events;
    ev.data.ptr = ch;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, ch->Fd(), &ev) == -1) {
        LOG_ERROR("UpdateChannel failed, fd=" << ch->Fd()
                  << ", errno=" << errno << " (" << std::strerror(errno) << ")");
        throw std::runtime_error("Poller::UpdateChannel failed");
    }
}

void Poller::RemoveChannel(Channel* ch) {
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, ch->Fd(), nullptr) == -1) {
        LOG_ERROR("RemoveChannel failed, fd=" << ch->Fd()
                  << ", errno=" << errno << " (" << std::strerror(errno) << ")");
        throw std::runtime_error("Poller::RemoveChannel failed");
    }
}

void Poller::Poll(std::vector<Channel*>& active_channels, int timeout_ms) {
    active_channels.clear();

    int nfds = 0;
    do {
        nfds = epoll_wait(epoll_fd_, events_.data(), events_.size(), timeout_ms);
    } while (nfds == -1 && errno == EINTR);

    if (nfds == -1) {
        throw std::runtime_error("Failed to wait on epoll, error: " +
                                 std::string(std::strerror(errno)));
    } else if (nfds == 0) {
        return;
    }

    for (int i = 0; i < nfds; ++i) {
        Channel* ch = static_cast<Channel*>(events_[i].data.ptr);
        ch->SetReadyEvent(events_[i].events);
        active_channels.push_back(ch);
    }
}