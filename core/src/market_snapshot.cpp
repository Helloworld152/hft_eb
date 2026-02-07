#include "../include/market_snapshot.h"
#include <immintrin.h>

MarketSnapshot& MarketSnapshot::instance() {
    static MarketSnapshot inst;
    return inst;
}

void MarketSnapshot::update(const TickRecord& rec) {
    const uint64_t id = rec.symbol_id;
    if (id >= MARKET_SNAPSHOT_MAX_SYMBOLS) return;
    MarketSnapshotSlot& slot = slots_[id];
    uint32_t s = slot.seq.load(std::memory_order_relaxed);
    slot.seq.store(s + 1, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);
    slot.tick = rec;
    slot.seq.store(s + 2, std::memory_order_release);
}

bool MarketSnapshot::get(uint64_t symbol_id, TickRecord& out) const {
    if (symbol_id >= MARKET_SNAPSHOT_MAX_SYMBOLS) return false;
    const MarketSnapshotSlot& slot = slots_[symbol_id];
    uint32_t s1, s2;
    int retries = 0;
    const int max_retries = 16;
    do {
        s1 = slot.seq.load(std::memory_order_acquire);
        if (s1 & 1) {
            _mm_pause();
            if (++retries > max_retries) return false;
            continue;
        }
        out = slot.tick;
        std::atomic_thread_fence(std::memory_order_acquire);
        s2 = slot.seq.load(std::memory_order_acquire);
        if (s1 == s2) return s1 != 0;
        retries++;
        _mm_pause();
    } while (retries <= max_retries);
    return false;
}

void MarketSnapshot::clear() {
    for (size_t i = 0; i < MARKET_SNAPSHOT_MAX_SYMBOLS; ++i)
        slots_[i].seq.store(0, std::memory_order_release);
}
