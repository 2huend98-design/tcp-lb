#pragma once

#include <cstddef>
#include <memory>

// 容量构造时按 2 的幂取整，之后不扩容。
// 写满 => 对端消费不动了，上层暂停/拒绝，不静默增长。
// readPtr2/writePtr2 覆盖跨回绕的第二段。
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity);
    ~RingBuffer() = default;

    size_t readableBytes() const;
    size_t writableBytes() const;
    bool empty() const;

    void consume(size_t len);

    const char* readPtr() const;
    const char* readPtr2() const;
    size_t readContiguousBytes() const;

    char* writePtr();
    char* writePtr2() const;
    size_t writeContiguousBytes() const;
    void produce(size_t len);

private:
    std::unique_ptr<char[]> buf_;
    size_t capacity_;
    size_t read_idx_;
    size_t write_idx_;
};