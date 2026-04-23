#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>

template <typename T>
class IntrusivePool {
public:
    static_assert(std::is_trivially_destructible<T>::value,
                  "IntrusivePool is intended for POD-like hot-path nodes");

    using index_type = uint32_t;
    static constexpr index_type npos = std::numeric_limits<index_type>::max();

    explicit IntrusivePool(size_t capacity)
        : capacity_(capacity),
          slots_(capacity ? new Slot[capacity] : nullptr),
          free_head_(capacity ? 0 : npos) {
        for (index_type i = 0; i < capacity_; ++i) {
            slots_[i].free.next_free = i + 1 < capacity_ ? i + 1 : npos;
        }
    }

    IntrusivePool(const IntrusivePool&) = delete;
    IntrusivePool& operator=(const IntrusivePool&) = delete;
    IntrusivePool(IntrusivePool&&) = delete;
    IntrusivePool& operator=(IntrusivePool&&) = delete;

    index_type allocate() noexcept {
        if (free_head_ == npos) return npos;

        const index_type idx = free_head_;
        Slot& slot = slots_[idx];
        free_head_ = slot.free.next_free;

        new (&slot.data) T;
        slot.used = true;
        ++size_;
        return idx;
    }

    bool release(index_type idx) noexcept {
        if (idx >= capacity_ || !slots_[idx].used) return false;

        release_unchecked(idx);
        return true;
    }

    void release_unchecked(index_type idx) noexcept {
        new (&slots_[idx].free) FreeSlot{free_head_};
        slots_[idx].used = false;
        free_head_ = idx;
        --size_;
    }

    T* get(index_type idx) noexcept {
        return idx < capacity_ && slots_[idx].used ? &slots_[idx].data : nullptr;
    }

    const T* get(index_type idx) const noexcept {
        return idx < capacity_ && slots_[idx].used ? &slots_[idx].data : nullptr;
    }

    T& operator[](index_type idx) noexcept {
        return slots_[idx].data;
    }

    const T& operator[](index_type idx) const noexcept {
        return slots_[idx].data;
    }

    size_t capacity() const noexcept {
        return capacity_;
    }

    size_t size() const noexcept {
        return size_;
    }

    size_t available() const noexcept {
        return capacity_ - size_;
    }

private:
    struct FreeSlot {
        index_type next_free = npos;
    };

    struct Slot {
        union {
            FreeSlot free;
            T data;
        };
        bool used = false;

        Slot() : free{} {}
        ~Slot() {}
    };

    const size_t capacity_;
    std::unique_ptr<Slot[]> slots_;
    index_type free_head_ = npos;
    size_t size_ = 0;
};
