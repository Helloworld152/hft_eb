#pragma once

#include <atomic>
#include <cstddef>
#include <algorithm> // for std::min
#include <utility>   // for std::pair
#include <new>       // for hardware_destructive_interference_size

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



// ============================================================================
//  HFT Single Producer Single Consumer (SPSC) Batch RingBuffer
// ============================================================================
#define CACHE_LINE_SIZE 64

template <typename T, size_t Capacity>
class BatchRingBuffer {
    // 强制 Capacity 必须是 2 的幂
    static_assert((Capacity > 0) && ((Capacity & (Capacity - 1)) == 0), 
                  "Capacity must be power of 2");

public:
    BatchRingBuffer() 
        : head_(0), tail_(0), cached_head_(0), cached_tail_(0) {}

    // 禁止拷贝
    BatchRingBuffer(const BatchRingBuffer&) = delete;
    BatchRingBuffer& operator=(const BatchRingBuffer&) = delete;

    // ========================================================================
    //  生产者接口 (Producer)
    // ========================================================================

    /**
     * [核心接口] 1. 占坑 (Reserve)
     * 返回一个指针，指向内部可写内存。支持批量申请。
     * @return pair<T* 指针, size_t 连续可用长度>
     */
    std::pair<T*, size_t> reserve() noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        
        // [Shadow Index]
        if (tail - cached_head_ >= Capacity) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (tail - cached_head_ >= Capacity) return {nullptr, 0};
        }

        size_t index = tail & (Capacity - 1);
        size_t contiguous = std::min(Capacity - (tail - cached_head_), Capacity - index);

        return { &buffer_[index], contiguous };
    }

    /**
     * [核心接口] 2. 提交 (Commit)
     * 批量提交 n 个数据
     */
    void commit(size_t n) noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        tail_.store(tail + n, std::memory_order_release);
    }

    /**
     * [兼容接口] 单个 Push
     * 编译器会自动内联优化为 reserve(1) + write + commit(1)
     */
    bool push(const T& item) noexcept {
        auto [ptr, len] = reserve();
        if (len == 0) return false;
        *ptr = item; // Copy Assignment
        commit(1);
        return true;
    }

    // ========================================================================
    //  消费者接口 (Consumer)
    // ========================================================================

    /**
     * [核心接口] 1. 偷看 (Peek)
     * 返回可读数据的指针。支持批量读取。
     * @return pair<T* 指针, size_t 连续可读长度>
     */
    std::pair<T*, size_t> peek() noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);

        // [Shadow Index]
        if (cached_tail_ <= head) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (cached_tail_ <= head) return {nullptr, 0};
        }

        size_t index = head & (Capacity - 1);
        size_t contiguous = std::min(cached_tail_ - head, Capacity - index);

        return { &buffer_[index], contiguous };
    }

    /**
     * [核心接口] 2. 前进 (Advance)
     * 批量归还 n 个数据槽位
     */
    void advance(size_t n) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        head_.store(head + n, std::memory_order_release);
    }

    /**
     * [兼容接口] 单个 Pop
     */
    bool pop(T& item) noexcept {
        auto [ptr, len] = peek();
        if (len == 0) return false;
        item = *ptr; // Copy Assignment
        advance(1);
        return true;
    }

private:
    // ========================================================================
    //  内存布局 (False Sharing 防护)
    // ========================================================================

    alignas(CACHE_LINE_SIZE) std::atomic<size_t> head_;
    size_t cached_tail_;
    char pad1_[CACHE_LINE_SIZE - sizeof(std::atomic<size_t>) - sizeof(size_t)];

    alignas(CACHE_LINE_SIZE) std::atomic<size_t> tail_;
    size_t cached_head_;
    char pad2_[CACHE_LINE_SIZE - sizeof(std::atomic<size_t>) - sizeof(size_t)];

    alignas(CACHE_LINE_SIZE) T buffer_[Capacity];
};


// ============================================================================
//  HFT Multi-Producer Multi-Consumer (MPMC) Bounded Queue
//  Implementation based on Dmitri Vyukov's MPMC algorithm.
// ============================================================================
template <typename T, size_t Capacity>
class MPMCRingBuffer {
    static_assert((Capacity > 0) && ((Capacity & (Capacity - 1)) == 0), 
                  "Capacity must be power of 2");

    struct Cell {
        std::atomic<size_t> sequence;
        T data;
    };

public:
    MPMCRingBuffer() {
        for (size_t i = 0; i < Capacity; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
        enqueue_pos_.store(0, std::memory_order_relaxed);
        dequeue_pos_.store(0, std::memory_order_relaxed);
    }

    bool push(const T& data) {
        Cell* cell;
        size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        
        for (;;) {
            cell = &buffer_[pos & (Capacity - 1)];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t dif = (intptr_t)seq - (intptr_t)pos;

            if (dif == 0) {
                // Buffer slot is empty, try to reserve it
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (dif < 0) {
                // Buffer is full
                return false;
            } else {
                // Sequence skew, reload pos
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }

        cell->data = data;
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& data) {
        Cell* cell;
        size_t pos = dequeue_pos_.load(std::memory_order_relaxed);

        for (;;) {
            cell = &buffer_[pos & (Capacity - 1)];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t dif = (intptr_t)seq - (intptr_t)(pos + 1);

            if (dif == 0) {
                // Slot has data, try to reserve
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (dif < 0) {
                // Buffer empty
                return false;
            } else {
                // Sequence skew
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }

        data = cell->data;
        cell->sequence.store(pos + Capacity, std::memory_order_release);
        return true;
    }

private:
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> enqueue_pos_;
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> dequeue_pos_;
    alignas(CACHE_LINE_SIZE) Cell buffer_[Capacity];
};