#include "health/health_checker.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

#include "common/logger.hpp"

HealthChecker::HealthChecker(BackendPool* backend_pool, int interval_sec, int timeout_sec)
    : backend_pool_(backend_pool),
      running_(false),
      interval_sec_(interval_sec),
      timeout_sec_(timeout_sec) {
    assert(timeout_sec_ > 0);
}

HealthChecker::~HealthChecker() {
    Stop();
}

void HealthChecker::Start() {
    if (interval_sec_ <= 0) {
        return;
    }

    if (running_.exchange(true)) return;

    thread_ = std::make_unique<std::thread>(&HealthChecker::Run, this);
}

void HealthChecker::Stop() {
    running_.store(false, std::memory_order_release);
    if (thread_ && thread_->joinable()) {
        thread_->join();
    }
}

void HealthChecker::Run() {
    while (running_.load(std::memory_order_acquire)) {
        for (const Backend& backend : backend_pool_->GetBackends()) {
            if (!running_.load(std::memory_order_acquire)) break;

            bool is_available = TcpProbe(backend.address, backend.port);
            bool old_availability = backend.is_available.load(std::memory_order_acquire);

            if (is_available != old_availability) {
                backend_pool_->SetBackendAvailability(backend.address, backend.port, is_available);
                LOG_INFO("HealthChecker: Backend " << backend.address << ":" << backend.port
                          << " is " << (is_available ? "available" : "unavailable"));
            }
        }

        for (int i = 0; i < interval_sec_ && running_.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

bool HealthChecker::TcpProbe(const std::string& address, uint16_t port) {
    int sockfd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sockfd < 0) return false;

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, address.c_str(), &addr.sin_addr) <= 0) {
        ::close(sockfd);
        return false;
    }

    int ret = ::connect(sockfd, (struct sockaddr*)&addr, sizeof(addr));
    if (ret == 0) {
        ::close(sockfd);
        return true;
    }
    if (errno != EINPROGRESS) {
        ::close(sockfd);
        return false;
    }

    struct pollfd pfd;
    pfd.fd = sockfd;
    pfd.events = POLLOUT;

    int poll_ret;
    do {
        poll_ret = ::poll(&pfd, 1, timeout_sec_ * 1000);
    } while (poll_ret == -1 && errno == EINTR);

    if (poll_ret > 0 && (pfd.revents & POLLOUT)) {
        int error = 0;
        socklen_t len = sizeof(error);
        if (::getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
            ::close(sockfd);
            return false;
        }
        ::close(sockfd);
        return true;
    }

    ::close(sockfd);
    return false;
}