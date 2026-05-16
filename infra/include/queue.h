#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

#ifndef CACHE_LINE_SIZE
#define CACHE_LINE_SIZE 64
#endif

// 运行时容量的单生产者单消费者（SPSC）队列。
// 实现方式：基于 sequence 的有界环形队列。
// 生产者和消费者各自推进本地 ticket，只通过槽位上的 sequence 协调，
// 因此热路径上不需要共享 head/tail 原子索引。
template <typename T>
class SpscQueue {
public:
    explicit SpscQueue(size_t capacity)
        : capacity_(capacity),
          mask_(capacity - 1),
          slots_(new Slot[capacity_]),
          head_local_(0),
          tail_local_(0) {
        if (capacity < 2 || (capacity & (capacity - 1)) != 0) {
            throw std::invalid_argument("Capacity must be power of 2");
        }
        for (size_t i = 0; i < capacity_; ++i) {
            slots_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    size_t capacity() const noexcept { return capacity_; }

    size_t size() const noexcept {
        return tail_local_ >= head_local_ ? (tail_local_ - head_local_) : 0;
    }

    bool push(const T& item) {
        const size_t ticket = tail_local_;
        Slot& slot = slots_[ticket & mask_];
        if (slot.sequence.load(std::memory_order_acquire) != ticket) {
            return false;
        }
        slot.data = item;
        slot.sequence.store(ticket + 1, std::memory_order_release);
        tail_local_ = ticket + 1;
        return true;
    }

    bool pop(T& item) {
        const size_t ticket = head_local_;
        Slot& slot = slots_[ticket & mask_];
        if (slot.sequence.load(std::memory_order_acquire) != ticket + 1) {
            return false;
        }
        item = std::move(slot.data);
        slot.sequence.store(ticket + capacity_, std::memory_order_release);
        head_local_ = ticket + 1;
        return true;
    }

    T* claim() noexcept {
        const size_t ticket = tail_local_;
        Slot& slot = slots_[ticket & mask_];
        if (slot.sequence.load(std::memory_order_acquire) != ticket) {
            return nullptr;
        }
        return &slot.data;
    }

    void publish() noexcept {
        const size_t ticket = tail_local_;
        Slot& slot = slots_[ticket & mask_];
        slot.sequence.store(ticket + 1, std::memory_order_release);
        tail_local_ = ticket + 1;
    }

    const T* peek() const noexcept {
        const size_t ticket = head_local_;
        const Slot& slot = slots_[ticket & mask_];
        if (slot.sequence.load(std::memory_order_acquire) != ticket + 1) {
            return nullptr;
        }
        return &slot.data;
    }

    T* peek() noexcept {
        const size_t ticket = head_local_;
        Slot& slot = slots_[ticket & mask_];
        if (slot.sequence.load(std::memory_order_acquire) != ticket + 1) {
            return nullptr;
        }
        return &slot.data;
    }

    void commit() noexcept {
        const size_t ticket = head_local_;
        Slot& slot = slots_[ticket & mask_];
        slot.sequence.store(ticket + capacity_, std::memory_order_release);
        head_local_ = ticket + 1;
    }

private:
    struct Slot {
        alignas(CACHE_LINE_SIZE) std::atomic<size_t> sequence{0};
        T data;
    };

    const size_t capacity_;
    const size_t mask_;
    const std::unique_ptr<Slot[]> slots_;
    alignas(CACHE_LINE_SIZE) size_t head_local_;
    alignas(CACHE_LINE_SIZE) size_t tail_local_;
};

// 有界的 sequence-based MPSC 队列。
// 实现方式：多个生产者通过原子 tail ticket 抢占槽位，单消费者使用本地
// head ticket 顺序消费，并额外发布一个近似进度用于 size_approx()。
// 容量必须是 2 的幂。
template <typename T>
class MPSCQueue {
public:
    explicit MPSCQueue(size_t capacity)
        : capacity_(capacity),
          mask_(capacity - 1),
          slots_(new Slot[capacity]),
          head_ticket_local_(0) {
        if (capacity < 2 || (capacity & (capacity - 1)) != 0) {
            throw std::invalid_argument("Capacity must be power of 2");
        }

        for (size_t i = 0; i < capacity_; ++i) {
            slots_[i].sequence.store(i, std::memory_order_relaxed);
        }

        tail_ticket_.store(0, std::memory_order_relaxed);
        head_ticket_published_.store(0, std::memory_order_relaxed);
    }

    size_t capacity() const noexcept {
        return capacity_;
    }

    size_t size_approx() const noexcept {
        uint64_t head = head_ticket_published_.load(std::memory_order_relaxed);
        uint64_t tail = tail_ticket_.load(std::memory_order_relaxed);
        return tail >= head ? static_cast<size_t>(tail - head) : 0;
    }

    bool push(T&& item) noexcept {
        uint64_t ticket = tail_ticket_.fetch_add(1, std::memory_order_relaxed);

        Slot& slot = slots_[static_cast<size_t>(ticket) & mask_];
        while (slot.sequence.load(std::memory_order_acquire) != ticket) {
            __builtin_ia32_pause();
        }

        slot.data = std::move(item);
        slot.sequence.store(ticket + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& item) noexcept {
        uint64_t ticket = head_ticket_local_;

        Slot& slot = slots_[static_cast<size_t>(ticket) & mask_];
        while (slot.sequence.load(std::memory_order_acquire) != ticket + 1) {
            __builtin_ia32_pause();
        }

        item = std::move(slot.data);
        slot.sequence.store(ticket + capacity_, std::memory_order_release);
        head_ticket_local_ = ticket + 1;
        head_ticket_published_.store(head_ticket_local_, std::memory_order_relaxed);
        return true;
    }

private:
    struct Slot {
        alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> sequence{0};
        T data;
    };

    const size_t capacity_;
    const size_t mask_;
    const std::unique_ptr<Slot[]> slots_;
    alignas(CACHE_LINE_SIZE) uint64_t head_ticket_local_;

    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> tail_ticket_;
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> head_ticket_published_;
};

// Folly 风格的无锁 ticketed MPMC 有界队列。
// 实现方式：生产者和消费者两侧都通过原子 ticket 抢占位置；每个槽位用 turn
// 计数区分“可写”和“可读”阶段，从而处理环形回绕。
// 容量必须是 2 的幂。
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

    size_t size_approx() const noexcept {
        size_t head = head_ticket_.load(std::memory_order_relaxed);
        size_t tail = tail_ticket_.load(std::memory_order_relaxed);
        return tail >= head ? (tail - head) : 0;
    }

    bool push(T&& item) noexcept {
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

    bool pop(T& item) noexcept {
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

// HFT 场景下的多生产者多消费者（MPMC）有界环形缓冲区。
// 实现方式：Vyukov 风格的固定容量 ring buffer；每个槽位维护 sequence，
// 入队和出队位置通过 CAS 抢占。
// 容量必须在构造时提供，且为 2 的幂。
template <typename T>
class MPMCRingBuffer {
    struct Cell {
        std::atomic<size_t> sequence;
        T data;
    };

public:
    explicit MPMCRingBuffer(size_t capacity)
        : capacity_(capacity),
          mask_(capacity - 1),
          buffer_(new Cell[capacity]) {
        if (capacity < 2 || (capacity & (capacity - 1)) != 0) {
            throw std::invalid_argument("Capacity must be power of 2");
        }

        for (size_t i = 0; i < capacity_; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
        enqueue_pos_.store(0, std::memory_order_relaxed);
        dequeue_pos_.store(0, std::memory_order_relaxed);
    }

    bool push(const T& data) {
        Cell* cell;
        size_t pos = enqueue_pos_.load(std::memory_order_relaxed);

        for (;;) {
            cell = &buffer_[pos & mask_];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

            if (dif == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (dif < 0) {
                return false;
            } else {
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
            cell = &buffer_[pos & mask_];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

            if (dif == 0) {
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (dif < 0) {
                return false;
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }

        data = cell->data;
        cell->sequence.store(pos + capacity_, std::memory_order_release);
        return true;
    }

private:
    const size_t capacity_;
    const size_t mask_;
    const std::unique_ptr<Cell[]> buffer_;
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> enqueue_pos_;
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> dequeue_pos_;
};
