#pragma once
#include <atomic>
#include <cstddef>

// Single-Producer Single-Consumer 무잠금 큐
template <typename T, size_t Size>
class SPSCQueue {
public:
    bool push(const T& item) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t next = (tail + 1) % Size;
        if (next == head_.load(std::memory_order_acquire))
            return false; // 큐 가득 참
        buffer_[tail] = item;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire))
            return false; // 큐 비어 있음
        item = buffer_[head];
        head_.store((head + 1) % Size, std::memory_order_release);
        return true;
    }

private:
    T buffer_[Size];
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
};
