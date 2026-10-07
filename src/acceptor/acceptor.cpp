#include "acceptor/acceptor.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/logger.hpp"

int Acceptor::CreateListenSocket(uint16_t port) {
    int sockfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sockfd == -1) {
        throw std::runtime_error("Failed to create listening socket: " +
                                 std::string(std::strerror(errno)));
    }

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        close(sockfd);
        throw std::runtime_error("Failed to bind listening socket: " +
                                 std::string(std::strerror(errno)));
    }

    return sockfd;
}

Acceptor::Acceptor(EventLoop *loop, uint16_t port)
    : loop_(loop),
      listen_fd_(CreateListenSocket(port)),
      channel_(loop, listen_fd_) {
}

Acceptor::~Acceptor() {
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
}

void Acceptor::Start() {
    if (listen(listen_fd_, SOMAXCONN) == -1) {
        close(listen_fd_);
        listen_fd_ = -1;
        throw std::runtime_error("Failed to listen on socket: " +
                                 std::string(std::strerror(errno)));
    }

    channel_.SetReadCallback([this]() { OnAccept(); });
    channel_.EnableReading();
}

void Acceptor::Stop() {
    if (listen_fd_ < 0) return;

    channel_.ClearCallbacks();
    channel_.DisableAll();
    close(listen_fd_);
    listen_fd_ = -1;
}

void Acceptor::SetNewConnectionCallback(std::function<void(int sockfd)> cb) {
    conn_cb_ = std::move(cb);
}

void Acceptor::OnAccept() {
    struct sockaddr_in client_addr {};

    while (true) {
        socklen_t client_len = sizeof(client_addr);
        int conn_fd = accept4(listen_fd_, (struct sockaddr*)&client_addr, &client_len,
                              SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (conn_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else if (errno == ECONNABORTED || errno == EPROTO || errno == EPERM) {
                continue;
            } else if (errno == EINTR) {
                continue;
            } else if (errno == EMFILE || errno == ENFILE) {
                LOG_ERROR("FD limit reached (" << std::strerror(errno)
                          << "), pausing accept (listen fd kept, process alive)");
                break;
            } else {
                LOG_ERROR("Failed to accept new connection: " << std::strerror(errno));
                break;
            }
        }

        if (conn_cb_) {
            int one = 1;
            setsockopt(conn_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            conn_cb_(conn_fd);
        } else {
            close(conn_fd);
        }
    }
}