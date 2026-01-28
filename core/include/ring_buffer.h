#pragma once

#include <atomic>
#include <vector>
#include <cstddef>
#include <iostream>

// Cache line size (usually 64 bytes) to prevent false sharing
#define CACHE_LINE_SIZE 64

template <typename T, size_t Capacity>
class RingBuffer {
public:
    RingBuffer() : head_(0), tail_(0) {
        // Buffer size must be power of 2 for bitwise masking optimization
        static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    }

    // Producer only: Push item
    // Returns true if successful, false if full
    bool push(const T& item) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t head = head_.load(std::memory_order_acquire);

        if (tail - head >= Capacity) {
            return false; // Full
        }

        buffer_[tail & (Capacity - 1)] = item;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer only: Pop item
    // Returns true if successful, false if empty
    bool pop(T& item) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_acquire);

        if (head == tail) {
            return false; // Empty
        }

        item = buffer_[head & (Capacity - 1)];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

private:
    // Padding to avoid false sharing
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> head_;
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> tail_;
    
    // Data storage
    T buffer_[Capacity];
};
