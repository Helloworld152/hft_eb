#pragma once

#include "protocol.h"
#include <atomic>
#include <cstdint>

#ifndef MARKET_SNAPSHOT_MAX_SYMBOLS
#define MARKET_SNAPSHOT_MAX_SYMBOLS 2048
#endif

struct alignas(64) MarketSnapshotSlot {
    std::atomic<uint32_t> seq{0};  // even=stable, odd=writing
    TickRecord tick;
};

// 进程内行情截面，单写多读无锁（SeqLock per slot）。不依赖具体模块。
// 实现与单例均在 hft_core 中，保证所有 so 观测到同一实例。
class MarketSnapshot {
public:
    static MarketSnapshot& instance();

    void update(const TickRecord& rec);
    bool get(uint64_t symbol_id, TickRecord& out) const;
    void clear();

private:
    MarketSnapshot() = default;
    MarketSnapshotSlot slots_[MARKET_SNAPSHOT_MAX_SYMBOLS];
};
