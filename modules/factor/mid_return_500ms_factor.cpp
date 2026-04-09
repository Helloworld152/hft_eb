#include "../../include/factor/factor_node.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

uint64_t hhmmssmmm_to_ms(uint64_t hhmmssmmm) {
    uint64_t ms = hhmmssmmm % 1000ULL;
    uint64_t total_sec = hhmmssmmm / 1000ULL;
    uint64_t sec = total_sec % 100ULL;
    uint64_t total_min = total_sec / 100ULL;
    uint64_t min = total_min % 100ULL;
    uint64_t hour = total_min / 100ULL;
    return ((hour * 60ULL + min) * 60ULL + sec) * 1000ULL + ms;
}

double safe_mid(const TickRecord* tick) {
    if (!tick) return 0.0;
    double bid = tick->bid_price[0];
    double ask = tick->ask_price[0];
    if (bid > 0.0 && ask > 0.0) return 0.5 * (bid + ask);
    if (tick->last_price > 0.0) return tick->last_price;
    return 0.0;
}

}  // namespace

class MidReturn500msFactor : public IFactorNode {
public:
    void init(const ConfigMap& config) override {
        auto it = config.find("window_ms");
        if (it != config.end()) {
            long long v = std::atoll(it->second.c_str());
            if (v > 0) window_ms_ = static_cast<uint64_t>(v);
        }

        it = config.find("tick_size");
        if (it != config.end()) {
            tick_size_ = std::atof(it->second.c_str());
        }

        it = config.find("debug");
        if (it != config.end()) {
            debug_ = (it->second == "true");
        }
    }

    double compute(const FactorContext& ctx, const std::vector<double>& /*inputs*/) override {
        if (!ctx.tick) return last_value_;

        double mid = safe_mid(ctx.tick);
        if (mid <= 0.0) return last_value_;

        uint64_t now_ms = hhmmssmmm_to_ms(ctx.tick->update_time);
        if (!initialized_) {
            initialized_ = true;
            window_start_ms_ = now_ms;
            window_start_mid_ = mid;
            return last_value_;
        }

        if (now_ms < window_start_ms_) {
            window_start_ms_ = now_ms;
            window_start_mid_ = mid;
            return last_value_;
        }

        if (now_ms - window_start_ms_ >= window_ms_) {
            double diff = mid - window_start_mid_;
            if (tick_size_ > 0.0) {
                last_value_ = diff / tick_size_;
            } else if (std::abs(window_start_mid_) > std::numeric_limits<double>::epsilon()) {
                last_value_ = diff / window_start_mid_;
            } else {
                last_value_ = 0.0;
            }
            window_start_ms_ = now_ms;
            window_start_mid_ = mid;
        }

        if (debug_) {
            std::cout << "[MidReturn500msFactor] value=" << last_value_
                      << " mid=" << mid << std::endl;
        }
        return last_value_;
    }

private:
    uint64_t window_ms_ = 500;
    uint64_t window_start_ms_ = 0;
    double window_start_mid_ = 0.0;
    double tick_size_ = 1.0;
    double last_value_ = 0.0;
    bool initialized_ = false;
    bool debug_ = false;
};

EXPORT_FACTOR(MidReturn500msFactor)
