#include "proxy/proxy_session.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <cstring>

#include "balancer/backend.hpp"
#include "balancer/backend_pool.hpp"
#include "common/logger.hpp"
#include "reactor/event_loop.hpp"

ProxySession::ProxySession(EventLoop *loop, int client_fd, BackendPool* backend_pool)
    : loop_(loop),
      backend_pool_(backend_pool),
      client_conn_(std::make_shared<TcpConnection>(loop, client_fd)) {
    backend_info_ = backend_pool_->GetBackend();
    if (!backend_info_) {
        LOG_ERROR("ProxySession: No available backend!");
    }
    loop_->AddSession();
}

ProxySession::~ProxySession() {
    loop_->RemoveSession();
    if (!closed_) {
        CloseBoth();
    }
}

void ProxySession::Start() {
    if (!backend_info_) {
        CloseBoth();
        return;
    }

    auto self = shared_from_this();

    client_conn_->SetEofCallback([self]() {
        if (self->backend_conn_) self->backend_conn_->Shutdown();
        self->CheckCondition();
    });

    client_conn_->SetWriteCompleteCallback([self]() {
        self->CheckCondition();
    });

    client_conn_->SetCloseCallback([self](TcpConnection& c) {
        self->CloseBoth();
    });

    ConnectBackend();
}

void ProxySession::ConnectBackend() {
    auto self = shared_from_this();

    backend_conn_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (backend_conn_fd_ < 0) {
        LOG_ERROR("ProxySession: failed to create backend socket");
        CloseBoth();
        return;
    }

    int nd = 1;
    setsockopt(backend_conn_fd_, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(backend_info_->port);
    if (::inet_pton(AF_INET, backend_info_->address.c_str(), &addr.sin_addr) <= 0) {
        LOG_ERROR("ProxySession: invalid IP");
        CloseBoth();
        return;
    }

    int ret = ::connect(backend_conn_fd_, (struct sockaddr*)&addr, sizeof(addr));
    if (ret == 0) {
        OnBackendConnected();
    } else if (errno == EINPROGRESS) {
        backend_conn_channel_ = std::make_shared<Channel>(loop_, backend_conn_fd_);
        backend_conn_channel_->SetWriteCallback([self]() { self->OnBackendConnected(); });
        backend_conn_channel_->SetCloseCallback([self]() {
            LOG_ERROR("ProxySession: backend connect socket closed prematurely");
            self->CloseBoth();
        });
        backend_conn_channel_->EnableWriting();
        ArmConnectTimeout();
    } else {
        LOG_ERROR("ProxySession: backend connect error: " << std::strerror(errno));
        CloseBoth();
    }
}

void ProxySession::OnBackendConnected() {
    auto self = shared_from_this();

    DisarmConnectTimeout();

    std::shared_ptr<Channel> channel = std::move(backend_conn_channel_);
    backend_conn_channel_.reset();

    int conn_fd = backend_conn_fd_;
    backend_conn_fd_ = -1;

    if (channel) {
        channel->ClearCallbacks();
        channel->DisableAll();
        channel->SetFd(-1);
        loop_->QueueTask([ch = std::move(channel)]() {});
    }

    if (conn_fd < 0) {
        CloseBoth();
        return;
    }

    int error = 0;
    socklen_t len = sizeof(error);
    if (::getsockopt(conn_fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
        LOG_ERROR("ProxySession: backend connect error: " << std::strerror(error));
        ::close(conn_fd);
        CloseBoth();
        return;
    }

    backend_conn_ = std::make_shared<TcpConnection>(loop_, conn_fd);

    backend_conn_->SetEofCallback([self]() {
        if (self->client_conn_) self->client_conn_->Shutdown();
        self->CheckCondition();
    });

    backend_conn_->SetWriteCompleteCallback([self]() {
        self->CheckCondition();
    });

    backend_conn_->SetCloseCallback([self](TcpConnection&) {
        if (self->client_conn_) {
            self->client_conn_->Shutdown();
        }
        if (!self->client_conn_ || self->client_conn_->IsClosed()) {
            self->CloseBoth();
        }
    });

    client_conn_->PipeTo(backend_conn_.get());
    backend_conn_->PipeTo(client_conn_.get());

    client_conn_->Start();
    backend_conn_->Start();
}

void ProxySession::ArmConnectTimeout() {
    connect_tfd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (connect_tfd_ < 0) return;
    connect_timer_ch_ = std::make_shared<Channel>(loop_, connect_tfd_);
    connect_timer_ch_->SetReadCallback([self = shared_from_this()]() {
        self->OnConnectTimeout();
    });
    connect_timer_ch_->EnableReading();
    itimerspec its {};
    its.it_value.tv_sec = kConnectTimeoutSec;
    ::timerfd_settime(connect_tfd_, 0, &its, nullptr);
}

void ProxySession::DisarmConnectTimeout() {
    if (connect_tfd_ < 0) return;
    if (connect_timer_ch_) {
        connect_timer_ch_->DisableAll();
    }
    ::close(connect_tfd_);
    connect_tfd_ = -1;
    if (connect_timer_ch_) {
        connect_timer_ch_->ClearCallbacks();
        connect_timer_ch_->SetFd(-1);
        loop_->QueueTask([ch = std::move(connect_timer_ch_)]() {});
    }
}

void ProxySession::OnConnectTimeout() {
    uint64_t exp = 0;
    ssize_t n = ::read(connect_tfd_, &exp, sizeof exp);
    (void)n;
    if (closed_ || backend_conn_) return;
    LOG_ERROR("ProxySession: backend connect timeout (" << kConnectTimeoutSec << "s), giving up");
    CloseBoth();
}

void ProxySession::CheckCondition() {
    if (closed_) return;
    if (!client_conn_ || !backend_conn_) return;

    const bool cd = client_conn_->IsEof() && client_conn_->IsOutputBufferEmpty();
    const bool bd = backend_conn_->IsEof() && backend_conn_->IsOutputBufferEmpty();

    if (cd && bd) {
        auto self = shared_from_this();
        loop_->QueueTask([self]() {
            self->CloseBoth();
        });
    }
}

void ProxySession::CloseBoth() {
    if (closed_) return;
    closed_ = true;

    DisarmConnectTimeout();

    auto client = std::move(client_conn_);
    auto backend = std::move(backend_conn_);
    auto backend_channel = std::move(backend_conn_channel_);

    if (backend_channel) {
        backend_channel->DisableAll();
        backend_channel->SetFd(-1);
    }

    if (client) client->Close();
    if (backend) backend->Close();

    if (backend_conn_fd_ >= 0) {
        ::close(backend_conn_fd_);
        backend_conn_fd_ = -1;
    }

    if (backend_pool_ && backend_info_) {
        backend_pool_->ReleaseBackend(backend_info_);
        backend_info_ = nullptr;
    }

    loop_->QueueTask([client = std::move(client), backend = std::move(backend),
                      backend_channel = std::move(backend_channel)]() {});
}