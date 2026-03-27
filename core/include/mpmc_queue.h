#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>

#ifndef CACHE_LINE_SIZE
#define CACHE_LINE_SIZE 64
#endif

// Folly-style wait-free ticketed MPMC bounded queue.
// Capacity must be power of two.
template <typename T>
class MPMCQueue {
public:
    explicit MPMCQueue(size_t capacity)
        : capacity_(capacity),
          mask_(capacity - 1),
          slots_(new Slot[capacity]) {
        if (capacity < 2 || (capacity & (capacity - 1)) != 0) {
            throw std::invalid_argument("Capacity must be power of 2");
        }
        tail_ticket_.store(0, std::memory_order_relaxed);
        head_ticket_.store(0, std::memory_order_relaxed);
    }

    size_t capacity() const noexcept {
        return capacity_;
    }

    // Approximate size (may be negative transiently in extreme races, clamp to 0)
    size_t size_approx() const noexcept {
        size_t head = head_ticket_.load(std::memory_order_relaxed);
        size_t tail = tail_ticket_.load(std::memory_order_relaxed);
        return tail >= head ? (tail - head) : 0;
    }

    bool enqueue(T&& item) noexcept {
        uint64_t ticket = tail_ticket_.fetch_add(1, std::memory_order_relaxed);

        size_t idx = static_cast<size_t>(ticket) & mask_;
        uint64_t turn = ticket / capacity_;
        Slot& slot = slots_[idx];

        while (slot.turn.load(std::memory_order_acquire) != 2 * turn) {
            __builtin_ia32_pause();
        }

        slot.data = std::move(item);
        slot.turn.store(2 * turn + 1, std::memory_order_release);
        return true;
    }

    bool dequeue(T& item) noexcept {
        uint64_t ticket = head_ticket_.fetch_add(1, std::memory_order_relaxed);

        size_t idx = static_cast<size_t>(ticket) & mask_;
        uint64_t turn = ticket / capacity_;
        Slot& slot = slots_[idx];

        while (slot.turn.load(std::memory_order_acquire) != 2 * turn + 1) {
            __builtin_ia32_pause();
        }

        item = std::move(slot.data);
        slot.turn.store(2 * turn + 2, std::memory_order_release);
        return true;
    }

private:
    struct Slot {
        alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> turn{0};
        T data;
    };
 
    const size_t capacity_;
    const size_t mask_;
    const std::unique_ptr<Slot[]> slots_;

    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> tail_ticket_;
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> head_ticket_;
};
