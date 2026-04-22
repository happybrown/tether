#pragma once
#include <atomic>
#include <cstddef>

template <typename T, size_t Size>
class SPSCQueue {
private:
    T buffer[Size];
    // std::atomic을 사용하여 스레드 간 동기화 오버헤드(Mutex)를 완전히 제거합니다.
    std::atomic<size_t> head{0};
    std::atomic<size_t> tail{0};

public:
    // 생산자(이벤트 캡처 스레드)가 호출합니다.
    bool push(const T& item) {
        size_t current_tail = tail.load(std::memory_order_relaxed);
        size_t next_tail = (current_tail + 1) % Size;
        
        if (next_tail == head.load(std::memory_order_acquire)) {
            return false; // 큐가 꽉 참 (데이터 유실 허용: 마우스 과거 데이터는 버려도 무방함)
        }
        
        buffer[current_tail] = item;
        tail.store(next_tail, std::memory_order_release);
        return true;
    }

    // 소비자(네트워크 스레드)가 호출합니다.
    bool pop(T& item) {
        size_t current_head = head.load(std::memory_order_relaxed);
        
        if (current_head == tail.load(std::memory_order_acquire)) {
            return false; // 큐가 비어 있음
        }
        
        item = buffer[current_head];
        head.store((current_head + 1) % Size, std::memory_order_release);
        return true;
    }
};