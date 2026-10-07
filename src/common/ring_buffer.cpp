#include "common/ring_buffer.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstring>

RingBuffer::RingBuffer(size_t capacity)
    : buf_(new char[std::bit_ceil(capacity)]),
      capacity_(std::bit_ceil(capacity)),
      read_idx_(0),
      write_idx_(0) {}

size_t RingBuffer::readableBytes() const {
    return write_idx_ - read_idx_;
}

size_t RingBuffer::writableBytes() const {
    return capacity_ - readableBytes();
}

bool RingBuffer::empty() const {
    return readableBytes() == 0;
}

void RingBuffer::consume(size_t len) {
    assert(len <= readableBytes());
    read_idx_ += len;
    if (read_idx_ == write_idx_) {
        read_idx_ = write_idx_ = 0;
    }
}

const char* RingBuffer::readPtr() const {
    return buf_.get() + (read_idx_ & (capacity_ - 1));
}

const char* RingBuffer::readPtr2() const {
    if ((read_idx_ & (capacity_ - 1)) + readableBytes() > capacity_) {
        return buf_.get();
    }
    return nullptr;
}

size_t RingBuffer::readContiguousBytes() const {
    size_t end = std::min(read_idx_ + readableBytes(),
                          (read_idx_ & ~(capacity_ - 1)) + capacity_);
    return end - read_idx_;
}

char* RingBuffer::writePtr() {
    return buf_.get() + (write_idx_ & (capacity_ - 1));
}

char* RingBuffer::writePtr2() const {
    if ((write_idx_ & (capacity_ - 1)) + writableBytes() > capacity_) {
        return buf_.get();
    }
    return nullptr;
}

size_t RingBuffer::writeContiguousBytes() const {
    size_t start = write_idx_ & (capacity_ - 1);
    return std::min(writableBytes(), capacity_ - start);
}

void RingBuffer::produce(size_t len) {
    assert(len <= writableBytes());
    write_idx_ += len;
}