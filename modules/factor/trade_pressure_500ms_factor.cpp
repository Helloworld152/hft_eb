#include "factor/factor_node.h"

#include <cstdlib>

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

double mid_price(const TickRecord* tick) {
    if (!tick) return 0.0;
    double bid = tick->bid_price[0];
    double ask = tick->ask_price[0];
    if (bid > 0.0 && ask > 0.0) return 0.5 * (bid + ask);
    return tick->last_price;
}

}  // namespace

class TradePressure500msFactor : public IFactorNode {
public:
    void init(const ConfigMap& config) override {
        auto it = config.find("window_ms");
        if (it != config.end()) {
            long long v = std::atoll(it->second.c_str());
            if (v > 0) window_ms_ = static_cast<uint64_t>(v);
        }

        it = config.find("debug");
        if (it != config.end()) {
            debug_ = (it->second == "true");
        }
    }

    double compute(const FactorContext& ctx, const std::vector<double>& /*inputs*/) override {
        if (!ctx.tick) return last_value_;

        uint64_t now_ms = hhmmssmmm_to_ms(ctx.tick->update_time);
        int delta_volume = 0;
        if (initialized_ && ctx.tick->volume >= last_cum_volume_) {
            delta_volume = ctx.tick->volume - last_cum_volume_;
        }

        double current_mid = mid_price(ctx.tick);
        double delta_sign = 0.0;
        if (ctx.tick->last_price >= ctx.tick->ask_price[0] && ctx.tick->ask_price[0] > 0.0) {
            delta_sign = 1.0;
        } else if (ctx.tick->last_price <= ctx.tick->bid_price[0] && ctx.tick->bid_price[0] > 0.0) {
            delta_sign = -1.0;
        } else if (initialized_) {
            double diff = current_mid - last_mid_;
            if (diff > 0.0) delta_sign = 1.0;
            else if (diff < 0.0) delta_sign = -1.0;
        }

        signed_volume_ += delta_sign * static_cast<double>(delta_volume);
        total_volume_ += static_cast<double>(delta_volume);

        if (!initialized_) {
            initialized_ = true;
            window_start_ms_ = now_ms;
        } else if (now_ms < window_start_ms_) {
            window_start_ms_ = now_ms;
            signed_volume_ = 0.0;
            total_volume_ = 0.0;
        } else if (now_ms - window_start_ms_ >= window_ms_) {
            if (total_volume_ > 0.0) {
                last_value_ = signed_volume_ / total_volume_;
            } else {
                last_value_ = 0.0;
            }
            window_start_ms_ = now_ms;
            signed_volume_ = 0.0;
            total_volume_ = 0.0;
        }

        last_cum_volume_ = ctx.tick->volume;
        last_mid_ = current_mid;

        if (debug_) {
            LOG_DEBUG("[TradePressure500msFactor] value={} signed_volume={}", last_value_, signed_volume_);
        }
        return last_value_;
    }

private:
    uint64_t window_ms_ = 500;
    uint64_t window_start_ms_ = 0;
    int last_cum_volume_ = 0;
    double last_mid_ = 0.0;
    double signed_volume_ = 0.0;
    double total_volume_ = 0.0;
    double last_value_ = 0.0;
    bool initialized_ = false;
    bool debug_ = false;
};

EXPORT_FACTOR(TradePressure500msFactor)
