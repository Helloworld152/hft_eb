#pragma once

#include "protocol.h"
#include <atomic>
#include <cstdint>
#include <immintrin.h> // For _mm_pause
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdexcept>
#include <string>
#include <cstring>

// Maximum number of symbols supported in the snapshot SHM
// Adjusted to cover the range in symbols.txt (approx 1000) with room for growth
constexpr size_t MAX_SNAPSHOT_SYMBOLS = 2048;
constexpr uint64_t SNAPSHOT_SHM_MAGIC = 0x534E415053484F54; // "SNAPSHOT"

// Slot structure for a single symbol
// Aligned to 128 bytes to prevent false sharing between adjacent slots
// (TickRecord is ~200-300 bytes, so maybe 512 bytes is safer to cover it entirely + align)
// Let's check TickRecord size. 
// 32(sym)+8(id)+4(day)+8(time) = 52.
// + 8*double(64) + int(4) + 8*double(64) + ...
// roughly: 52 + 8 + 4 + 8 + 8 + 6*8 + 4*5*8 + 4*5*4 = ~400 bytes.
// So 512 bytes alignment is good.
struct alignas(512) SnapshotSlot {
    std::atomic<uint32_t> seq{0}; // Sequence number (Even = Stable, Odd = Writing)
    uint32_t padding;             // Explicit padding
    TickRecord tick;              // The market data
    
    // Total size will be padded to 512 by compiler due to alignas
};

// The layout of the Shared Memory file
struct SnapshotShmLayout {
    uint64_t magic;
    uint64_t symbol_count;
    SnapshotSlot slots[MAX_SNAPSHOT_SYMBOLS];
};

class SnapshotShm {
public:
    // is_writer: true if this process will write data (hft_md), false for readers
    SnapshotShm(const std::string& shm_name, bool is_writer) 
        : is_writer_(is_writer), shm_name_(shm_name) {
        
        // Open SHM
        int flags = is_writer ? (O_RDWR | O_CREAT) : (O_RDWR);
        int fd = shm_open(shm_name.c_str(), flags, 0666);
        if (fd < 0) {
            throw std::runtime_error("Failed to shm_open: " + shm_name);
        }

        size_t size = sizeof(SnapshotShmLayout);
        
        if (is_writer) {
            if (ftruncate(fd, size) != 0) {
                close(fd);
                throw std::runtime_error("Failed to ftruncate SHM");
            }
        } else {
            // Check size for readers
            struct stat s;
            if (fstat(fd, &s) != 0 || (size_t)s.st_size < size) {
                close(fd);
                throw std::runtime_error("SHM size mismatch or not initialized");
            }
        }

        void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (ptr == MAP_FAILED) {
            close(fd);
            throw std::runtime_error("Failed to mmap SHM");
        }
        close(fd); // fd no longer needed

        layout_ = static_cast<SnapshotShmLayout*>(ptr);

        if (is_writer) {
            // Initialize if new
            if (layout_->magic != SNAPSHOT_SHM_MAGIC) {
                std::memset(layout_, 0, size);
                layout_->magic = SNAPSHOT_SHM_MAGIC;
            }
        } else {
            if (layout_->magic != SNAPSHOT_SHM_MAGIC) {
                munmap(layout_, size);
                throw std::runtime_error("Invalid SHM Magic");
            }
        }
    }

    ~SnapshotShm() {
        if (layout_) {
            munmap(layout_, sizeof(SnapshotShmLayout));
        }
        // Note: We do not shm_unlink here. 
        // Writer might want to unlink on exit, but usually we keep it for inspection.
    }

    // ========================================================================
    // Writer API
    // ========================================================================
    void update(int index, const TickRecord& tick) {
        if (index < 0 || index >= MAX_SNAPSHOT_SYMBOLS) return;
        
        SnapshotSlot& slot = layout_->slots[index];
        
        // 1. Increment Seq (Odd = Writing)
        uint32_t seq = slot.seq.load(std::memory_order_relaxed);
        slot.seq.store(seq + 1, std::memory_order_release);

        // 2. Copy Data
        slot.tick = tick;

        // 3. Increment Seq (Even = Done)
        slot.seq.store(seq + 2, std::memory_order_release);
    }

    // ========================================================================
    // Reader API
    // ========================================================================
    bool read(int index, TickRecord& out_tick) const {
        if (index < 0 || index >= MAX_SNAPSHOT_SYMBOLS) return false;
        
        const SnapshotSlot& slot = layout_->slots[index];
        uint32_t seq1, seq2;
        int retries = 0;
        const int MAX_RETRIES = 10; // Avoid infinite loops if writer dies mid-write

        do {
            seq1 = slot.seq.load(std::memory_order_acquire);
            
            // If writing (Odd), wait
            if (seq1 & 1) {
                _mm_pause();
                continue;
            }

            out_tick = slot.tick;
            
            std::atomic_thread_fence(std::memory_order_acquire);
            
            seq2 = slot.seq.load(std::memory_order_acquire);

            if (seq1 == seq2) return true;

            retries++;
            if (retries > MAX_RETRIES) return false; // Fail fast or yield
             _mm_pause();

        } while (true);
    }
    
    // Unsafe read (for debugging/monitoring where tearing is acceptable)
    const TickRecord& read_unsafe(int index) const {
         return layout_->slots[index].tick;
    }

private:
    bool is_writer_;
    std::string shm_name_;
    SnapshotShmLayout* layout_ = nullptr;
};
