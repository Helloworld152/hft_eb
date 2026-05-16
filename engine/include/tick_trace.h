#pragma once

#include <atomic>
#include <cstdint>

#include "latency_stats.h"
#include "protocol.h"

#ifndef HFT_TICK_TRACE_ENABLED
#define HFT_TICK_TRACE_ENABLED 1
#endif

struct TickTraceContext {
    uint64_t trace_id = 0;
    uint64_t start_tsc = 0;
};

struct TickTraceItem {
    TickRecord tick;
    TickTraceContext trace;
    uint64_t enqueue_tsc = 0;
};

inline uint64_t next_tick_trace_id() {
    static std::atomic<uint64_t> next_id{1};
    return next_id.fetch_add(1, std::memory_order_relaxed);
}

inline TickTraceContext begin_tick_trace() {
    TickTraceContext ctx;
#if HFT_TICK_TRACE_ENABLED
    ctx.trace_id = next_tick_trace_id();
    ctx.start_tsc = latency_now_ticks();
#endif
    return ctx;
}

inline uint64_t tick_trace_elapsed_ns(const TickTraceContext& trace) {
#if HFT_TICK_TRACE_ENABLED
    if (trace.trace_id == 0 || trace.start_tsc == 0) {
        return 0;
    }
    return latency_ticks_to_ns(latency_now_ticks() - trace.start_tsc);
#else
    (void)trace;
    return 0;
#endif
}

inline uint64_t tick_trace_queue_ns(const TickTraceItem& item) {
#if HFT_TICK_TRACE_ENABLED
    if (item.enqueue_tsc == 0) {
        return 0;
    }
    return latency_ticks_to_ns(latency_now_ticks() - item.enqueue_tsc);
#else
    (void)item;
    return 0;
#endif
}
