#include "proxy/tcp_connection.hpp"

#include <bit>
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <linux/sockios.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/uio.h>
#include <unistd.h>

#include "common/logger.hpp"
#include "common/time_util.hpp"
#include "reactor/event_loop.hpp"

std::atomic<int> TcpConnection::alive_count_{0};

void TcpConnection::SetOutputBufferCap(size_t bytes) {
    // 提前对齐，保证 OutputBufferCap() 与 RingBuffer 实际容量一致
    s_output_cap_ = std::bit_ceil(bytes);
}

TcpConnection::TcpConnection(EventLoop *loop, int sockfd)
    : loop_(loop), sockfd_(sockfd), channel_(loop, sockfd) {
    assert(sockfd_ >= 0);
    {
        static const int rcvbuf = [] {
            const char* e = ::getenv("TCP_RCVBUF_BYTES");
            return e ? ::atoi(e) : 0;
        }();
        if (rcvbuf > 0) {
            int v = rcvbuf;
            ::setsockopt(sockfd_, SOL_SOCKET, SO_RCVBUF, &v, sizeof(v));
        }
    }
    {
        static const int sndbuf = [] {
            const char* e = ::getenv("TCP_SNDBUF_BYTES");
            return e ? ::atoi(e) : 0;
        }();
        if (sndbuf > 0) {
            int v = sndbuf;
            ::setsockopt(sockfd_, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v));
        }
    }
    alive_count_.fetch_add(1, std::memory_order_relaxed);
}

TcpConnection::~TcpConnection() {
    DisarmDeadline();
    if (life_ == Active) {
        channel_.DisableAll();
        channel_.ClearCallbacks();
        channel_.SetFd(-1);
    }
    if (sockfd_ >= 0) {
        ::close(sockfd_);
        sockfd_ = -1;
    }
    alive_count_.fetch_sub(1, std::memory_order_relaxed);
}

void TcpConnection::Start() {
    assert(close_cb_ != nullptr);
    std::weak_ptr<TcpConnection> weak_self = shared_from_this();
    channel_.SetReadCallback([weak_self]() {
        if (auto self = weak_self.lock()) self->OnRead();
    });
    channel_.SetWriteCallback([weak_self]() {
        if (auto self = weak_self.lock()) self->SendInLoop();
    });
    // ERR/HUP 也走 OnRead，靠 readv 返回值裁决
    channel_.SetCloseCallback([weak_self]() {
        if (auto self = weak_self.lock()) self->OnRead();
    });
    channel_.EnableReading();
    last_io_ms_ = NowMs();
    ArmDeadline();
}

void TcpConnection::Shutdown() {
    if (life_ != Active || wr_pend_) return;
    wr_pend_ = true;
    if (output_buffer_.empty()) {
        DoShutdownWrite();
    }
}

void TcpConnection::Close() {
    if (life_ != Active) return;
    auto self = shared_from_this();
    loop_->RunTask([self]() { self->RetireInLoop(); });
}

void TcpConnection::PipeTo(TcpConnection *peer) {
    assert(loop_ == peer->loop_);
    peer_ = peer->shared_from_this();
}

void TcpConnection::TrySend() { SendInLoop(); }

void TcpConnection::OnRead() {
    if (life_ != Active) return;
    if (rd_fin_) {
        // EOF 后的事件只可能是迟到 RST
        int so_err = 0; socklen_t el = sizeof(so_err);
        if (::getsockopt(sockfd_, SOL_SOCKET, SO_ERROR, &so_err, &el) == 0 && so_err != 0) {
            LOG_ERROR("[die] fd=" << sockfd_ << " EOF后SO_ERROR=" << so_err
                     << "（" << std::strerror(so_err) << "）");
            Close();
            return;
        }
        return;
    }
    auto peer = peer_.lock();
    if (!peer) {
        Close();
        return;
    }
    auto guard = shared_from_this();
    while (true) {
        size_t avail = peer->GetOutputBufferAvailable();
        if (avail == 0) {
            if (!paused_) {
                int so_err = 0;
                socklen_t el = sizeof(so_err);
                if (::getsockopt(sockfd_, SOL_SOCKET, SO_ERROR, &so_err, &el) == 0 && so_err != 0) {
                    LOG_ERROR("[die] fd=" << sockfd_ << " 暂停点SO_ERROR=" << so_err
                             << "（" << std::strerror(so_err) << "）paused=" << paused_);
                    Close();
                    return;
                }
                paused_ = true;
                LOG_ERROR("[dbg-pause] fd=" << sockfd_
                         << " peeroutbuf=" << peer->output_buffer_.readableBytes()
                         << " peerpaused=" << peer->paused_
                         << " peerlife=" << (int)peer->life_);
            }
            return;
        }

        struct iovec iov[2];
        int iovcnt = 0;
        size_t len1 = peer->output_buffer_.writeContiguousBytes();
        iov[iovcnt].iov_base = peer->output_buffer_.writePtr();
        iov[iovcnt].iov_len = len1;
        iovcnt++;
        if (auto p2 = peer->output_buffer_.writePtr2()) {
            size_t len2 = avail - len1;
            if (len2 > 0) {
                iov[iovcnt].iov_base = p2;
                iov[iovcnt].iov_len = len2;
                iovcnt++;
            }
        }
        ssize_t n = readv(sockfd_, iov, iovcnt);
        if (n > 0) {
            total_read_ += n;
            last_io_ms_ = NowMs();
            peer->output_buffer_.produce(n);
            peer->TrySend();
        } else if (n == 0) {
            rd_fin_ = true;
            paused_ = false;
            channel_.DisableReading();
            if (eof_cb_) eof_cb_();
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                paused_ = false;
                channel_.EnableReading();
                LOG_ERROR("[dbg-eagain] fd=" << sockfd_
                         << " outbuf=" << output_buffer_.readableBytes()
                         << " peerpaused=" << (peer && peer->paused_)
                         << " peerlife=" << (peer ? (int)peer->life_ : -1));
                return;
            } else if (errno == EINTR) {
                continue;
            } else {
                LOG_ERROR("[die] fd=" << sockfd_ << " readv errno=" << errno
                         << "（" << std::strerror(errno) << "）");
                Close();
                return;
            }
        }
    }
}

void TcpConnection::SendInLoop() {
    if (life_ != Active) return;
    auto guard = shared_from_this();
    while (!output_buffer_.empty()) {
        struct iovec iov[2];
        int iovcnt = 0;
        size_t total_len = output_buffer_.readableBytes();
        size_t len1 = output_buffer_.readContiguousBytes();
        if (len1 > 0) {
            iov[iovcnt].iov_base = const_cast<char*>(output_buffer_.readPtr());
            iov[iovcnt].iov_len = len1;
            iovcnt++;
        }
        if (total_len > len1) {
            size_t len2 = total_len - len1;
            if (auto p2 = output_buffer_.readPtr2()) {
                iov[iovcnt].iov_base = const_cast<char*>(p2);
                iov[iovcnt].iov_len = len2;
                iovcnt++;
            }
        }
        if (iovcnt == 0) {
            break;
        }
        ssize_t n = writev(sockfd_, iov, iovcnt);
        if (n > 0) {
            total_written_ += n;
            last_io_ms_ = NowMs();
            output_buffer_.consume(n);
            continue;
        } else if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                channel_.EnableWriting();
                auto peer = peer_.lock();
                LOG_ERROR("[dbg-sendloop-eagain] fd=" << sockfd_
                         << " outbuf=" << output_buffer_.readableBytes()
                         << " peerpaused=" << (peer && peer->paused_)
                         << " peerlife=" << (peer ? (int)peer->life_ : -1)
                         << " peer_rdfin=" << (peer && peer->rd_fin_));
                if (peer && peer->paused_ && !peer->rd_fin_ && peer->life_ == Active
                    && output_buffer_.readableBytes() < OutputBufferCap() / 2) {
                    peer->paused_ = false;
                    peer->channel_.EnableReading();
                    loop_->QueueTask([peer]() { peer->OnRead(); });
                    LOG_ERROR("[dbg-recover] fd=" << peer->sockfd_ << " from=" << sockfd_
                             << " reason=hwm-half outbuf=" << output_buffer_.readableBytes());
                }
                return;
            } else if (errno == EINTR) {
                continue;
            } else {
                LOG_ERROR("[die-w] fd=" << sockfd_ << " writev errno=" << errno
                         << "(" << std::strerror(errno) << ")");
                Close();
                return;
            }
        }
    }
    channel_.DisableWriting();
    if (wr_pend_ && output_buffer_.empty()) {
        DoShutdownWrite();
    }
    if (write_complete_cb_) {
        write_complete_cb_();
    }

    auto peer = peer_.lock();
    if (peer && peer->paused_ && !peer->rd_fin_ && peer->life_ == Active) {
        peer->paused_ = false;
        peer->channel_.EnableReading();
        loop_->QueueTask([peer]() { peer->OnRead(); });
        LOG_ERROR("[dbg-recover] fd=" << peer->sockfd_ << " from=" << sockfd_
                 << " reason=drained");
    }
}

void TcpConnection::DoShutdownWrite() {
    if (life_ != Active) return;
    ::shutdown(sockfd_, SHUT_WR);
    // rd_fin_ 已置 -> 双方 FIN 都发完，正常收尾；graceful_retire_ 让退役不级联
    if (rd_fin_) {
        graceful_retire_ = true;
        Close();
    }
}

void TcpConnection::RetireInLoop() {
    if (!loop_->IsInLoopThread()) return;
    if (life_ != Active) return;
    auto self = shared_from_this();
    life_ = Retired;
    DisarmDeadline();
    channel_.DisableAll();
    int fd = sockfd_;
    if (!rd_fin_ || !output_buffer_.empty())
        LOG_INFO("[retire] fd=" << fd << " buf=" << output_buffer_.readableBytes()
                 << " rdfin=" << rd_fin_ << " wrpend=" << wr_pend_
                 << " paused=" << paused_);
    channel_.SetFd(-1);
    if (fd >= 0) {
        // rcvbuf 非空时 close 会触发 RST
        int unread = -1;
        ::ioctl(fd, FIONREAD, &unread);
        if (unread > 0) {
            struct tcp_info ti {};
            socklen_t sl = sizeof ti;
            ::getsockopt(fd, IPPROTO_TCP, TCP_INFO, &ti, &sl);
            int so_err = 0;
            socklen_t el = sizeof so_err;
            ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &el);
            LOG_ERROR("[retire-state] fd=" << fd << " state=" << (int)ti.tcpi_state
                    << " drained=0 unread_after=" << unread
                    << " so_err=" << so_err << " graceful=" << graceful_retire_);
        }
        ::close(fd);
        sockfd_ = -1;
    }
    if (!graceful_retire_) {
        if (auto p = peer_.lock()) {
            if (p.get() != this) p->Close();
        }
    }

    if (auto p = peer_.lock()) {
        if (p.get() != this && p->paused_ && p->life_ == Active) {
            p->paused_ = false;
            p->channel_.EnableReading();
            loop_->QueueTask([p]() { p->OnRead(); });
            LOG_ERROR("[dbg-recover] fd=" << p->sockfd_ << " from=" << fd
                     << " reason=peer-retired");
        }
    }

    loop_->QueueTask([self]() {
        if (self->life_ != Retired) return;
        if (self->close_cb_) self->close_cb_(*self);
        self->life_ = Closed;
    });
}

// 停滞计时器超时时间，默认 300s
static int IdleDeadlineSec() {
    static const int v = [] {
        if (const char* e = ::getenv("TCP_IDLE_DEADLINE_SEC")) {
            int n = atoi(e);
            if (n > 0) return n;
        }
        return 300;
    }();
    return v;
}

void TcpConnection::ArmDeadline(int remaining_sec) {
    if (life_ != Active) return;
    int sec = IdleDeadlineSec();
    if (remaining_sec >= 0) sec = remaining_sec;
    if (deadline_tfd_ < 0) {
        deadline_tfd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (deadline_tfd_ < 0) return;
        deadline_ch_ = std::make_shared<Channel>(loop_, deadline_tfd_);
        std::weak_ptr<TcpConnection> weak_self = shared_from_this();
        deadline_ch_->SetReadCallback([weak_self]() {
            if (auto self = weak_self.lock()) self->OnDeadline();
        });
        deadline_ch_->EnableReading();
    }
    itimerspec its {};
    its.it_value.tv_sec = sec;
    ::timerfd_settime(deadline_tfd_, 0, &its, nullptr);
}

void TcpConnection::DisarmDeadline() {
    if (deadline_tfd_ < 0) return;
    if (deadline_ch_) {
        deadline_ch_->DisableAll();
    }
    ::close(deadline_tfd_);
    deadline_tfd_ = -1;
    if (deadline_ch_) {
        deadline_ch_->ClearCallbacks();
        deadline_ch_->SetFd(-1);
        loop_->QueueTask([ch = std::move(deadline_ch_)]() {});
    }
}

void TcpConnection::OnDeadline() {
    uint64_t exp = 0;
    // ET：排空到期计数恢复边沿
    ssize_t n = ::read(deadline_tfd_, &exp, sizeof exp);
    (void)n;
    if (life_ != Active) return;

    // 距上次 I/O 超窗即放弃；EOF/shutdown/暂停不重置计时器
    const int64_t now = NowMs();
    const int64_t elapsed = now - last_io_ms_;
    const int win = IdleDeadlineSec();
    if (elapsed < (int64_t)win * 1000) {
        ArmDeadline(static_cast<int>(win - elapsed / 1000 - 1) > 0
                        ? static_cast<int>(win - elapsed / 1000 - 1) : 1);
        return;
    }
    LOG_INFO("[stall-giveup] fd=" << sockfd_ << " rd=" << total_read_
             << " wr=" << total_written_ << " paused=" << paused_
             << " rdfin=" << rd_fin_ << " wrpend=" << wr_pend_
             << "（距上次 I/O " << elapsed << "ms）");
    static const bool s_dry = ::getenv("TCP_KILL_DRYRUN") != nullptr;
    if (s_dry) { ArmDeadline(); return; }
    
    
    auto peer = peer_.lock();

    LOG_ERROR("[freeze-dump] fd=" << sockfd_
        << " buf=" << output_buffer_.readableBytes() << "/" << OutputBufferCap()
        << " paused=" << paused_
        << " rd_fin=" << rd_fin_ << " wr_pend=" << wr_pend_
        << " peer_buf=" << (peer ? peer->output_buffer_.readableBytes() : 0)
        << " peer_paused=" << (peer ? peer->paused_ : 0)
        << " peer_life=" << (peer ? (int)peer->life_ : -1)
        << " peer_rd_fin=" << (peer ? peer->rd_fin_ : 0)
        << " io_age_ms=" << elapsed);

    int outq = -1, inq = -1;
    ::ioctl(sockfd_, SIOCOUTQ, &outq);
    ::ioctl(sockfd_, SIOCINQ,  &inq);
    int so_err = 0; socklen_t el = sizeof(so_err);
    ::getsockopt(sockfd_, SOL_SOCKET, SO_ERROR, &so_err, &el);
    LOG_ERROR("[freeze-kernel] fd=" << sockfd_
        << " SIOCOUTQ=" << outq
        << " SIOCINQ="  << inq
        << " SO_ERROR=" << so_err);

    // 内核 TCP 状态 —— persist 死锁的签名在 probes/backoff/unacked 上
    struct tcp_info ti{};
    socklen_t tl = sizeof(ti);
    ::getsockopt(sockfd_, IPPROTO_TCP, TCP_INFO, &ti, &tl);
    LOG_ERROR("[freeze-tcpinfo] fd=" << sockfd_
        << " state="        << (int)ti.tcpi_state
        << " cwnd="         << ti.tcpi_snd_cwnd
        << " ssthresh="     << ti.tcpi_snd_ssthresh
        << " probes="       << (int)ti.tcpi_probes
        << " backoff="      << (int)ti.tcpi_backoff
        << " unacked="      << ti.tcpi_unacked
        << " retrans="      << ti.tcpi_total_retrans
        << " rcv_space="    << ti.tcpi_rcv_space
        << " rcv_ssthresh=" << ti.tcpi_rcv_ssthresh);

    LOG_ERROR("[freeze-epoll] fd=" << sockfd_
        << " self_events=" << channel_.Events()
        << " peer_events=" << (peer ? peer->channel_.Events() : 0));
        
    if (peer) {
        int p_outq = -1, p_inq = -1;
        ::ioctl(peer->sockfd_, SIOCOUTQ, &p_outq);
        ::ioctl(peer->sockfd_, SIOCINQ,  &p_inq);

        struct tcp_info pti{};
        socklen_t ptl = sizeof(pti);
        ::getsockopt(peer->sockfd_, IPPROTO_TCP, TCP_INFO, &pti, &ptl);

        LOG_ERROR("[freeze-peer] fd=" << peer->sockfd_
            << " SIOCOUTQ=" << p_outq
            << " SIOCINQ="  << p_inq
            << " state="     << (int)pti.tcpi_state
            << " cwnd="      << pti.tcpi_snd_cwnd
            << " ssthresh="  << pti.tcpi_snd_ssthresh
            << " probes="    << (int)pti.tcpi_probes
            << " backoff="   << (int)pti.tcpi_backoff
            << " unacked="   << pti.tcpi_unacked
            << " retrans="   << pti.tcpi_total_retrans
            << " rcv_space=" << pti.tcpi_rcv_space);
    }
    
    
    Close();
}

