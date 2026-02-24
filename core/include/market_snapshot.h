#pragma once

#include "protocol.h"
#include <atomic>
#include <cstdint>
#include <string>

#ifndef MARKET_SNAPSHOT_MAX_SYMBOLS
#define MARKET_SNAPSHOT_MAX_SYMBOLS 2048
#endif

struct alignas(64) MarketSnapshotSlot {
    std::atomic<uint32_t> seq{0};  // even=stable, odd=writing
    TickRecord tick;
};

/**
 * MarketSnapshot (基类接口)
 * 进程内与进程间行情截面的抽象基类。
 */
class MarketSnapshot {
public:
    virtual ~MarketSnapshot() = default;

    // 单例访问点
    static MarketSnapshot& instance();
    
    // 设置实现类 (由引擎在启动时调用)
    static void set_instance(MarketSnapshot* inst);

    virtual void update(const TickRecord& rec) = 0;
    virtual bool get(uint64_t symbol_id, TickRecord& out) const = 0;
    virtual void clear() = 0;

protected:
    MarketSnapshot() = default;
};

/**
 * 进程内版本 (LOCAL)
 */
class LocalMarketSnapshot : public MarketSnapshot {
public:
    LocalMarketSnapshot();
    void update(const TickRecord& rec) override;
    bool get(uint64_t symbol_id, TickRecord& out) const override;
    void clear() override;

private:
    MarketSnapshotSlot slots_[MARKET_SNAPSHOT_MAX_SYMBOLS];
};

/**
 * 共享内存版本 (SHM)
 */
class ShmMarketSnapshot : public MarketSnapshot {
public:
    /**
     * @param shm_name 共享内存名称
     * @param is_writer 是否拥有写权限
     */
    ShmMarketSnapshot(const std::string& shm_name, bool is_writer);
    ~ShmMarketSnapshot() override;

    void update(const TickRecord& rec) override;
    bool get(uint64_t symbol_id, TickRecord& out) const override;
    void clear() override;

private:
    static constexpr uint64_t SYMBOL_ID_BASE = 10000000;
    static constexpr size_t SYMBOL_INDEX_SIZE = 65536;

    struct ShmLayout {
        uint64_t magic;
        int32_t symbol_index[SYMBOL_INDEX_SIZE]; // symbol_id - BASE -> slot_idx
        MarketSnapshotSlot slots[MARKET_SNAPSHOT_MAX_SYMBOLS];
        std::atomic<int32_t> slot_count;
    };
    ShmLayout* layout_ = nullptr;
    bool is_writer_ = false;
    size_t shm_size_ = 0;
    std::string shm_name_;
};
