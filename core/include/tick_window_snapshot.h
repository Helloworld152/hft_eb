#pragma once

#include "protocol.h"
#include <atomic>
#include <cstdint>
#include <string>

// 近 N 条 Tick 的共享内存快照
class ShmTickWindowSnapshot {
public:
    static constexpr size_t TICK_WINDOW_SIZE = 128;
    static constexpr size_t MAX_SYMBOLS = 2048;

    /**
     * @param shm_name 共享内存名称
     * @param is_writer 是否拥有写权限
     */
    ShmTickWindowSnapshot(const std::string& shm_name, bool is_writer);
    ~ShmTickWindowSnapshot();

    void update(const TickRecord& rec);
    size_t read_recent(uint64_t symbol_id, TickRecord* out, size_t max_k) const;
    void clear();

private:
    static constexpr uint64_t SHM_MAGIC = 0x5449434B57494E44ULL; // "TICKWIND"
    static constexpr uint64_t SYMBOL_ID_BASE = 10000000;
    static constexpr size_t SYMBOL_INDEX_SIZE = 65536;

    struct alignas(512) TickWindowSlot {
        std::atomic<uint32_t> seq{0};
        uint32_t padding{0};
        TickRecord tick;
    };

    struct alignas(64) SymbolTickWindow {
        std::atomic<uint64_t> write_seq{0};
        TickWindowSlot slots[TICK_WINDOW_SIZE];
    };

    struct ShmLayout {
        uint64_t magic;
        std::atomic<int32_t> symbol_index[SYMBOL_INDEX_SIZE]; // symbol_id - BASE -> slot_idx
        SymbolTickWindow symbols[MAX_SYMBOLS];
        std::atomic<int32_t> slot_count;
    };

    bool read_slot(const TickWindowSlot& slot, TickRecord& out) const;
    int32_t get_or_alloc_slot(uint64_t symbol_id);

    ShmLayout* layout_ = nullptr;
    bool is_writer_ = false;
    size_t shm_size_ = 0;
    std::string shm_name_;
};
