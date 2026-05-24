#include "tick_window_snapshot.h"
#include "symbol_manager.h"
#include <immintrin.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace {

uint32_t tick_window_slot_index(uint64_t symbol_id) {
    const uint32_t index = SymbolManager::instance().get_index(symbol_id);
    if (index == std::numeric_limits<uint32_t>::max() ||
        index >= ShmTickWindowSnapshot::MAX_SYMBOLS) {
        return std::numeric_limits<uint32_t>::max();
    }
    return index;
}

}  // namespace

ShmTickWindowSnapshot::ShmTickWindowSnapshot(const std::string& shm_name, bool is_writer)
    : is_writer_(is_writer), shm_name_(shm_name) {
    shm_size_ = sizeof(ShmLayout);
    int flags = is_writer ? (O_RDWR | O_CREAT) : (O_RDWR);
    int fd = shm_open(shm_name.c_str(), flags, 0666);
    if (fd < 0) {
        throw std::runtime_error("Failed to shm_open: " + shm_name);
    }

    if (is_writer) {
        if (ftruncate(fd, shm_size_) != 0) {
            close(fd);
            throw std::runtime_error("Failed to ftruncate SHM");
        }
    } else {
        struct stat s;
        if (fstat(fd, &s) != 0 || static_cast<size_t>(s.st_size) < shm_size_) {
            close(fd);
            throw std::runtime_error("SHM size mismatch or not initialized");
        }
    }

    void* ptr = mmap(nullptr, shm_size_, PROT_READ | (is_writer ? PROT_WRITE : 0), MAP_SHARED, fd, 0);
    close(fd);
    if (ptr == MAP_FAILED) {
        throw std::runtime_error("Failed to mmap SHM");
    }

    layout_ = static_cast<ShmLayout*>(ptr);

    if (is_writer_ && layout_->magic != SHM_MAGIC) {
        std::memset(layout_, 0, shm_size_);
        layout_->magic = SHM_MAGIC;
    } else if (!is_writer_ && layout_->magic != SHM_MAGIC) {
        munmap(layout_, shm_size_);
        layout_ = nullptr;
        throw std::runtime_error("Invalid SHM Magic");
    }
}

ShmTickWindowSnapshot::~ShmTickWindowSnapshot() {
    if (layout_) {
        munmap(layout_, shm_size_);
    }
    if (is_writer_) {
        shm_unlink(shm_name_.c_str());
    }
}

void ShmTickWindowSnapshot::update(const TickRecord& rec) {
    if (!is_writer_ || !layout_) {
        return;
    }
    const uint32_t slot_idx = tick_window_slot_index(rec.symbol_id);
    if (slot_idx == std::numeric_limits<uint32_t>::max()) {
        return;
    }

    SymbolTickWindow& window = layout_->symbols[slot_idx];
    uint64_t seq = window.write_seq.load(std::memory_order_relaxed);
    seq += 1;
    window.write_seq.store(seq, std::memory_order_release);

    size_t pos = static_cast<size_t>((seq - 1) % TICK_WINDOW_SIZE);
    TickWindowSlot& slot = window.slots[pos];

    uint32_t s = slot.seq.load(std::memory_order_relaxed);
    slot.seq.store(s + 1, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);
    slot.tick = rec;
    slot.seq.store(s + 2, std::memory_order_release);
}

bool ShmTickWindowSnapshot::read_slot(const TickWindowSlot& slot, TickRecord& out) const {
    uint32_t s1 = 0;
    uint32_t s2 = 0;
    int retries = 0;
    const int kMaxRetries = 10;

    do {
        s1 = slot.seq.load(std::memory_order_acquire);
        if (s1 & 1) {
            _mm_pause();
            continue;
        }

        out = slot.tick;
        std::atomic_thread_fence(std::memory_order_acquire);
        s2 = slot.seq.load(std::memory_order_acquire);

        if (s1 == s2) {
            return s1 != 0;
        }
        _mm_pause();
    } while (++retries < kMaxRetries);

    return false;
}

size_t ShmTickWindowSnapshot::read_recent(uint64_t symbol_id, TickRecord* out, size_t max_k) const {
    if (!layout_ || out == nullptr || max_k == 0) {
        return 0;
    }
    const uint32_t slot_idx = tick_window_slot_index(symbol_id);
    if (slot_idx == std::numeric_limits<uint32_t>::max()) {
        return 0;
    }

    const SymbolTickWindow& window = layout_->symbols[slot_idx];
    uint64_t seq = window.write_seq.load(std::memory_order_acquire);
    if (seq == 0) {
        return 0;
    }

    size_t available = seq < TICK_WINDOW_SIZE ? static_cast<size_t>(seq) : TICK_WINDOW_SIZE;
    size_t k = max_k < available ? max_k : available;

    size_t read_count = 0;
    for (size_t i = 0; i < k; ++i) {
        uint64_t cur = seq - 1 - i;
        size_t pos = static_cast<size_t>(cur % TICK_WINDOW_SIZE);
        if (!read_slot(window.slots[pos], out[read_count])) {
            break;
        }
        read_count++;
    }

    return read_count;
}

void ShmTickWindowSnapshot::clear() {
    if (!is_writer_ || !layout_) {
        return;
    }
    for (size_t i = 0; i < MAX_SYMBOLS; ++i) {
        layout_->symbols[i].write_seq.store(0, std::memory_order_release);
        for (size_t j = 0; j < TICK_WINDOW_SIZE; ++j) {
            layout_->symbols[i].slots[j].seq.store(0, std::memory_order_release);
        }
    }
}
