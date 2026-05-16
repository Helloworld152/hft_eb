#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

#ifndef HFT_LATENCY_TSC_ENABLED
#define HFT_LATENCY_TSC_ENABLED 1
#endif

#if HFT_LATENCY_TSC_ENABLED && (defined(__x86_64__) || defined(__i386__))
#include <x86intrin.h>
#define HFT_USE_TSC 1
#else
#define HFT_USE_TSC 0
#endif

struct LatencySnapshot {
    uint64_t count = 0;
    uint64_t p50_ns = 0;
    uint64_t p99_ns = 0;
    uint64_t p999_ns = 0;
    uint64_t max_ns = 0;
};

class LatencyStats {
public:
    explicit LatencyStats(size_t capacity = 1024)
        : capacity_(capacity) {
        samples_.reserve(capacity_);
    }

    void record_ns(uint64_t ns) {
        max_ns_ = std::max(max_ns_, ns);
        count_++;
        if (samples_.size() < capacity_) {
            samples_.push_back(ns);
        } else {
            samples_[write_idx_] = ns;
            write_idx_ = (write_idx_ + 1) % capacity_;
        }
    }

    LatencySnapshot snapshot() const {
        LatencySnapshot snap;
        snap.count = count_;
        snap.max_ns = max_ns_;
        if (samples_.empty()) {
            return snap;
        }
        std::vector<uint64_t> tmp = samples_;
        snap.p50_ns = percentile_ns(tmp, 0.50);
        snap.p99_ns = percentile_ns(tmp, 0.99);
        snap.p999_ns = percentile_ns(tmp, 0.999);
        return snap;
    }

private:
    static uint64_t percentile_ns(std::vector<uint64_t>& v, double p) {
        if (v.empty()) return 0;
        size_t n = v.size();
        double rank = p * static_cast<double>(n);
        size_t idx = (rank <= 1.0) ? 0 : static_cast<size_t>(rank) - 1;
        if (idx >= n) idx = n - 1;
        std::nth_element(v.begin(), v.begin() + idx, v.end());
        return v[idx];
    }

    size_t capacity_;
    std::vector<uint64_t> samples_;
    size_t write_idx_ = 0;
    uint64_t count_ = 0;
    uint64_t max_ns_ = 0;
};

inline uint64_t latency_rdtsc() {
#if HFT_USE_TSC
    return __rdtsc();
#else
    return 0;
#endif
}

inline double latency_ticks_per_ns() {
#if HFT_USE_TSC
    static double value = []() {
        using clock = std::chrono::steady_clock;
        auto t0 = clock::now();
        uint64_t c0 = latency_rdtsc();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        auto t1 = clock::now();
        uint64_t c1 = latency_rdtsc();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        if (ns <= 0) return 0.0;
        return static_cast<double>(c1 - c0) / static_cast<double>(ns);
    }();
    return value;
#else
    return 0.0;
#endif
}

inline uint64_t latency_now_ticks() {
#if HFT_USE_TSC
    return latency_rdtsc();
#else
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
#endif
}

inline uint64_t latency_ticks_to_ns(uint64_t ticks) {
#if HFT_USE_TSC
    double tpn = latency_ticks_per_ns();
    return tpn > 0.0 ? static_cast<uint64_t>(static_cast<double>(ticks) / tpn) : 0;
#else
    return ticks;
#endif
}

inline void latency_maybe_log(const char* module_name,
                              int event_type,
                              LatencyStats& stats,
                              uint64_t& last_log_ticks,
                              uint64_t interval_ms) {
    uint64_t now = latency_now_ticks();
    if (last_log_ticks == 0) {
        last_log_ticks = now;
        return;
    }
    uint64_t elapsed_ns = latency_ticks_to_ns(now - last_log_ticks);
    if (elapsed_ns < interval_ms * 1000000ULL) {
        return;
    }
    last_log_ticks = now;
    LatencySnapshot snap = stats.snapshot();
    std::cout << "[Latency] module=" << module_name
              << " event=" << event_type
              << " count=" << snap.count
              << " p50_ns=" << snap.p50_ns
              << " p99_ns=" << snap.p99_ns
              << " p999_ns=" << snap.p999_ns
              << " max_ns=" << snap.max_ns
              << std::endl;
}

class LatencyScope {
public:
    LatencyScope(LatencyStats& stats,
                 const char* module_name,
                 int event_type,
                 uint64_t* last_log,
                 uint64_t interval_ms)
        : stats_(stats),
          module_name_(module_name),
          event_type_(event_type),
          last_log_(last_log),
          interval_ms_(interval_ms),
          start_ns_(0),
          start_tsc_(0) {
#if HFT_USE_TSC
        start_tsc_ = latency_rdtsc();
#else
        start_ns_ = now_ns_();
#endif
    }

    ~LatencyScope() noexcept {
#if HFT_USE_TSC
        uint64_t end_tsc = latency_rdtsc();
        uint64_t ns = latency_ticks_to_ns(end_tsc - start_tsc_);
        stats_.record_ns(ns);
#else
        uint64_t end_ns = now_ns_();
        stats_.record_ns(end_ns - start_ns_);
#endif
        if (last_log_) {
            latency_maybe_log(module_name_, event_type_, stats_, *last_log_, interval_ms_);
        }
    }

private:
    static inline uint64_t now_ns_() {
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) == 0) {
            return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
        }
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    LatencyStats& stats_;
    const char* module_name_;
    int event_type_;
    uint64_t* last_log_;
    uint64_t interval_ms_;
    uint64_t start_ns_;
    uint64_t start_tsc_;
};

#define LATENCY_SCOPE(module_name, event_type, stats, last_log, interval_ms) \
    LatencyScope latency_scope_##__LINE__(stats, module_name, event_type, &(last_log), interval_ms)

#define LATENCY_SCOPE_NOLOG(module_name, event_type, stats) \
    LatencyScope latency_scope_##__LINE__(stats, module_name, event_type, nullptr, 0)
