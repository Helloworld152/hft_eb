#include "../include/market_snapshot.h"
#include <immintrin.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>
#include <iostream>

// ==========================================
// 单例管理
// ==========================================
static MarketSnapshot* g_instance = nullptr;

MarketSnapshot& MarketSnapshot::instance() {
    if (g_instance == nullptr) {
        // 兜底：如果引擎没初始化，默认使用本地内存版本
        static LocalMarketSnapshot default_inst;
        return default_inst;
    }
    return *g_instance;
}

void MarketSnapshot::set_instance(MarketSnapshot* inst) {
    g_instance = inst;
}

// ==========================================
// LocalMarketSnapshot 实现
// ==========================================
LocalMarketSnapshot::LocalMarketSnapshot() {
    std::memset(slots_, 0, sizeof(slots_));
}

void LocalMarketSnapshot::update(const TickRecord& rec) {
    uint64_t id = rec.symbol_id;
    if (id >= MARKET_SNAPSHOT_MAX_SYMBOLS) return;
    
    MarketSnapshotSlot& slot = slots_[id];
    uint32_t s = slot.seq.load(std::memory_order_relaxed);
    slot.seq.store(s + 1, std::memory_order_release);
    
    std::atomic_thread_fence(std::memory_order_release);
    slot.tick = rec;
    
    slot.seq.store(s + 2, std::memory_order_release);
}

bool LocalMarketSnapshot::get(uint64_t symbol_id, TickRecord& out) const {
    if (symbol_id >= MARKET_SNAPSHOT_MAX_SYMBOLS) return false;
    
    const MarketSnapshotSlot& slot = slots_[symbol_id];
    uint32_t s1, s2;
    int retries = 0;
    
    do {
        s1 = slot.seq.load(std::memory_order_acquire);
        if (s1 & 1) {
            _mm_pause();
            continue;
        }
        
        out = slot.tick;
        std::atomic_thread_fence(std::memory_order_acquire);
        
        s2 = slot.seq.load(std::memory_order_acquire);
        if (s1 == s2) return s1 != 0;
        
        _mm_pause();
    } while (++retries < 16);
    
    return false;
}

void LocalMarketSnapshot::clear() {
    for (auto& slot : slots_) {
        slot.seq.store(0, std::memory_order_release);
    }
}

// ==========================================
// ShmMarketSnapshot 实现
// ==========================================
constexpr uint64_t SHM_MAGIC = 0x534E415053484F54; // "SNAPSHOT"

ShmMarketSnapshot::ShmMarketSnapshot(const std::string& shm_name, bool is_writer) 
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
    }

    void* ptr = mmap(nullptr, shm_size_, PROT_READ | (is_writer ? PROT_WRITE : 0), MAP_SHARED, fd, 0);
    close(fd);

    if (ptr == MAP_FAILED) {
        throw std::runtime_error("Failed to mmap SHM");
    }

    layout_ = static_cast<ShmLayout*>(ptr);

    if (is_writer && layout_->magic != SHM_MAGIC) {
        std::memset(layout_, 0, shm_size_);
        for (size_t i = 0; i < SYMBOL_INDEX_SIZE; ++i) {
            layout_->symbol_index[i] = -1;
        }
        layout_->magic = SHM_MAGIC;
    }
}

ShmMarketSnapshot::~ShmMarketSnapshot() {
    if (layout_) {
        munmap(layout_, shm_size_);
    }
    if (is_writer_) {
        shm_unlink(shm_name_.c_str());
    }
}

void ShmMarketSnapshot::update(const TickRecord& rec) {
    if (!is_writer_) return;
    uint64_t id = rec.symbol_id;
    if (id < SYMBOL_ID_BASE || id >= SYMBOL_ID_BASE + SYMBOL_INDEX_SIZE) return;

    uint32_t idx = static_cast<uint32_t>(id - SYMBOL_ID_BASE);
    int32_t target_idx = layout_->symbol_index[idx];
    
    if (target_idx == -1) {
        // 分配新槽位
        target_idx = layout_->slot_count.fetch_add(1, std::memory_order_relaxed);
        if (target_idx >= MARKET_SNAPSHOT_MAX_SYMBOLS) {
            layout_->slot_count.fetch_sub(1, std::memory_order_relaxed);
            return; // 满了
        }
        layout_->symbol_index[idx] = target_idx;
    }

    MarketSnapshotSlot& slot = layout_->slots[target_idx];
    uint32_t s = slot.seq.load(std::memory_order_relaxed);
    slot.seq.store(s + 1, std::memory_order_release);
    
    std::atomic_thread_fence(std::memory_order_release);
    slot.tick = rec;
    
    slot.seq.store(s + 2, std::memory_order_release);
}

bool ShmMarketSnapshot::get(uint64_t symbol_id, TickRecord& out) const {
    if (symbol_id < SYMBOL_ID_BASE || symbol_id >= SYMBOL_ID_BASE + SYMBOL_INDEX_SIZE) return false;

    uint32_t idx = static_cast<uint32_t>(symbol_id - SYMBOL_ID_BASE);
    int32_t target_idx = layout_->symbol_index[idx];

    if (target_idx == -1 || target_idx >= MARKET_SNAPSHOT_MAX_SYMBOLS) return false;
    
    const MarketSnapshotSlot& slot = layout_->slots[target_idx];
    uint32_t s1, s2;
    int retries = 0;
    
    do {
        s1 = slot.seq.load(std::memory_order_acquire);
        if (s1 & 1) {
            _mm_pause();
            continue;
        }
        
        out = slot.tick;
        std::atomic_thread_fence(std::memory_order_acquire);
        
        s2 = slot.seq.load(std::memory_order_acquire);
        if (s1 == s2) return s1 != 0;
        
        _mm_pause();
    } while (++retries < 16);
    
    return false;
}

void ShmMarketSnapshot::clear() {
    if (!is_writer_) return;
    for (size_t i = 0; i < SYMBOL_INDEX_SIZE; ++i) {
        layout_->symbol_index[i] = -1;
    }
    layout_->slot_count.store(0, std::memory_order_release);
    for (int i = 0; i < MARKET_SNAPSHOT_MAX_SYMBOLS; ++i) {
        layout_->slots[i].seq.store(0, std::memory_order_release);
    }
}
