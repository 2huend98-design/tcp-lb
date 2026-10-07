#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "common/ring_buffer.hpp"
#include "reactor/channel.hpp"

class EventLoop;

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    enum Life { Active, Retired, Closed };

    TcpConnection(EventLoop *loop, int sockfd);
    ~TcpConnection();

    static int AliveCount() { return alive_count_.load(std::memory_order_relaxed); }

    void Start();
    void Shutdown();
    void Close();
    void PipeTo(TcpConnection *peer);
    void TrySend();

    void SetEofCallback(std::function<void()> cb) { eof_cb_ = std::move(cb); }
    void SetCloseCallback(std::function<void(TcpConnection&)> cb) { close_cb_ = std::move(cb); }
    void SetWriteCompleteCallback(std::function<void()> cb) { write_complete_cb_ = std::move(cb); }

    bool IsEof() const { return rd_fin_; }
    bool IsClosed() const { return life_ != Active; }
    bool IsOutputBufferEmpty() const { return output_buffer_.empty(); }
    int GetFd() const { return sockfd_; }
    size_t GetOutputBufferAvailable() const {
        return OutputBufferCap() - output_buffer_.readableBytes();
    }

    static size_t OutputBufferCap() { return s_output_cap_; }
    static void SetOutputBufferCap(size_t bytes);

private:
    void OnRead();
    void SendInLoop();
    void DoShutdownWrite();
    void RetireInLoop();

    void ArmDeadline(int remaining_sec = -1);
    void OnDeadline();
    void DisarmDeadline();

    static inline size_t s_output_cap_ = 256 * 1024;

    EventLoop *loop_;
    int sockfd_;
    Channel channel_;

    Life life_ = Active;
    bool rd_fin_ = false;
    bool wr_pend_ = false;
    bool paused_ = false;
    bool graceful_retire_ = false;  // 正常收尾（双方 FIN 交换完毕）；置位则退役时不级联 peer
    RingBuffer output_buffer_{OutputBufferCap()};
    std::weak_ptr<TcpConnection> peer_;

    // 停滞计时器：timerfd 单实例，距上次 I/O 字节超窗即放弃
    int deadline_tfd_ = -1;
    std::shared_ptr<Channel> deadline_ch_;
    int64_t last_io_ms_ = 0;

    std::function<void()> eof_cb_;
    std::function<void(TcpConnection&)> close_cb_;
    std::function<void()> write_complete_cb_;
    uint64_t total_read_ = 0;
    uint64_t total_written_ = 0;

    static std::atomic<int> alive_count_;
};