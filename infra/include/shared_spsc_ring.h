#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace trade_gateway {

namespace detail {

inline void pause_cpu() {
    __builtin_ia32_pause();
}

struct SharedRingHeader {
    uint64_t magic = 0x4854464757545950ULL;  // "HFTGWTYP"
    uint32_t version = 1;
    uint32_t capacity = 0;
    uint32_t slot_size = 0;
    uint32_t reserved = 0;
    alignas(64) std::atomic<uint64_t> producer_ticket{0};
    alignas(64) std::atomic<uint64_t> consumer_ticket{0};
    alignas(64) std::atomic<uint64_t> producer_heartbeat_ns{0};
    alignas(64) std::atomic<uint64_t> consumer_heartbeat_ns{0};
};

template <typename T>
struct SharedRingSlot {
    alignas(64) std::atomic<uint64_t> sequence{0};
    T data{};
};

template <typename T>
struct SharedRingLayout {
    SharedRingHeader header;
    SharedRingSlot<T> slots[1];
};

inline size_t round_capacity(uint32_t capacity) {
    if (capacity < 2) return 2;
    size_t out = 1;
    while (out < capacity) out <<= 1;
    return out;
}

}  // namespace detail

template <typename T>
class SharedSpscRing {
public:
    SharedSpscRing() = default;
    ~SharedSpscRing() { close(); }

    SharedSpscRing(const SharedSpscRing&) = delete;
    SharedSpscRing& operator=(const SharedSpscRing&) = delete;

    void open(const std::string& name, uint32_t capacity, bool create, bool unlink_on_close = false) {
        close();

        name_ = name;
        unlink_on_close_ = unlink_on_close;
        owner_ = create;
        capacity_ = static_cast<uint32_t>(detail::round_capacity(capacity));
        const size_t map_size = sizeof(detail::SharedRingHeader) +
                                sizeof(detail::SharedRingSlot<T>) * capacity_;

        int flags = O_RDWR;
        if (create) flags |= O_CREAT;

        fd_ = shm_open(name.c_str(), flags, 0666);
        if (fd_ < 0) {
            throw std::runtime_error("shm_open failed for " + name + ": " + std::strerror(errno));
        }
        if (create && ftruncate(fd_, static_cast<off_t>(map_size)) != 0) {
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("ftruncate failed for " + name + ": " + std::strerror(errno));
        }
        if (!create) {
            struct stat st {};
            if (fstat(fd_, &st) != 0) {
                ::close(fd_);
                fd_ = -1;
                throw std::runtime_error("fstat failed for " + name + ": " + std::strerror(errno));
            }
            mapped_size_ = static_cast<size_t>(st.st_size);
        } else {
            mapped_size_ = map_size;
        }

        base_ = ::mmap(nullptr, mapped_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (base_ == MAP_FAILED) {
            ::close(fd_);
            fd_ = -1;
            base_ = nullptr;
            throw std::runtime_error("mmap failed for " + name + ": " + std::strerror(errno));
        }

        header_ = reinterpret_cast<detail::SharedRingHeader*>(base_);
        slots_ = reinterpret_cast<detail::SharedRingSlot<T>*>(
            static_cast<char*>(base_) + sizeof(detail::SharedRingHeader));

        if (create || header_->magic != 0x4854464757545950ULL) {
            header_->magic = 0x4854464757545950ULL;
            header_->version = 1;
            header_->capacity = capacity_;
            header_->slot_size = sizeof(T);
            header_->reserved = 0;
            header_->producer_ticket.store(0, std::memory_order_relaxed);
            header_->consumer_ticket.store(0, std::memory_order_relaxed);
            header_->producer_heartbeat_ns.store(0, std::memory_order_relaxed);
            header_->consumer_heartbeat_ns.store(0, std::memory_order_relaxed);
            for (uint32_t i = 0; i < capacity_; ++i) {
                slots_[i].sequence.store(i, std::memory_order_relaxed);
            }
        } else {
            capacity_ = header_->capacity;
            if (capacity_ == 0) {
                close();
                throw std::runtime_error("invalid capacity for shared ring " + name);
            }
            if (header_->slot_size != sizeof(T)) {
                close();
                throw std::runtime_error("slot size mismatch for shared ring " + name);
            }
        }
    }

    void close() {
        if (base_ && base_ != MAP_FAILED) {
            ::munmap(base_, mapped_size_);
        }
        if (fd_ >= 0) {
            ::close(fd_);
        }
        if (unlink_on_close_ && owner_ && !name_.empty()) {
            shm_unlink(name_.c_str());
        }
        fd_ = -1;
        base_ = nullptr;
        header_ = nullptr;
        slots_ = nullptr;
        mapped_size_ = 0;
        capacity_ = 0;
        owner_ = false;
        unlink_on_close_ = false;
        name_.clear();
    }

    bool try_push(const T& item) {
        if (!header_) return false;
        const uint64_t ticket = header_->producer_ticket.load(std::memory_order_relaxed);
        auto& slot = slots_[ticket & (capacity_ - 1)];
        if (slot.sequence.load(std::memory_order_acquire) != ticket) {
            return false;
        }
        slot.data = item;
        slot.sequence.store(ticket + 1, std::memory_order_release);
        header_->producer_ticket.store(ticket + 1, std::memory_order_relaxed);
        return true;
    }

    bool try_pop(T& item) {
        if (!header_) return false;
        const uint64_t ticket = header_->consumer_ticket.load(std::memory_order_relaxed);
        auto& slot = slots_[ticket & (capacity_ - 1)];
        if (slot.sequence.load(std::memory_order_acquire) != ticket + 1) {
            return false;
        }
        item = slot.data;
        slot.sequence.store(ticket + capacity_, std::memory_order_release);
        header_->consumer_ticket.store(ticket + 1, std::memory_order_relaxed);
        return true;
    }

    void producer_heartbeat(uint64_t now_ns) {
        if (header_) header_->producer_heartbeat_ns.store(now_ns, std::memory_order_relaxed);
    }

    void consumer_heartbeat(uint64_t now_ns) {
        if (header_) header_->consumer_heartbeat_ns.store(now_ns, std::memory_order_relaxed);
    }

    uint32_t capacity() const noexcept { return capacity_; }
    bool is_open() const noexcept { return header_ != nullptr; }

private:
    int fd_ = -1;
    void* base_ = nullptr;
    size_t mapped_size_ = 0;
    uint32_t capacity_ = 0;
    bool owner_ = false;
    bool unlink_on_close_ = false;
    std::string name_;
    detail::SharedRingHeader* header_ = nullptr;
    detail::SharedRingSlot<T>* slots_ = nullptr;
};

}  // namespace trade_gateway
