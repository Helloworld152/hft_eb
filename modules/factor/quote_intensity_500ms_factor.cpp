#include "factor/factor_node.h"

#include <cmath>
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

}  // namespace

class QuoteIntensity500msFactor : public IFactorNode {
public:
    void init(const ConfigMap& config) override {
        auto it = config.find("window_ms");
        if (it != config.end()) {
            long long v = std::atoll(it->second.c_str());
            if (v > 0) window_ms_ = static_cast<uint64_t>(v);
        }

        it = config.find("normalize");
        if (it != config.end()) {
            normalize_ = (it->second == "true");
        }

        it = config.find("debug");
        if (it != config.end()) {
            debug_ = (it->second == "true");
        }
    }

    double compute(const FactorContext& ctx, const std::vector<double>& /*inputs*/) override {
        if (!ctx.tick) return last_value_;

        uint64_t now_ms = hhmmssmmm_to_ms(ctx.tick->update_time);
        if (!initialized_) {
            initialized_ = true;
            window_start_ms_ = now_ms;
            last_bid_price_ = ctx.tick->bid_price[0];
            last_ask_price_ = ctx.tick->ask_price[0];
            last_bid_volume_ = ctx.tick->bid_volume[0];
            last_ask_volume_ = ctx.tick->ask_volume[0];
            return last_value_;
        }

        if (ctx.tick->bid_price[0] != last_bid_price_ || ctx.tick->ask_price[0] != last_ask_price_ ||
            ctx.tick->bid_volume[0] != last_bid_volume_ || ctx.tick->ask_volume[0] != last_ask_volume_) {
            quote_updates_++;
            last_bid_price_ = ctx.tick->bid_price[0];
            last_ask_price_ = ctx.tick->ask_price[0];
            last_bid_volume_ = ctx.tick->bid_volume[0];
            last_ask_volume_ = ctx.tick->ask_volume[0];
        }

        if (now_ms < window_start_ms_) {
            window_start_ms_ = now_ms;
            quote_updates_ = 0;
            return last_value_;
        }

        uint64_t elapsed = now_ms - window_start_ms_;
        if (elapsed >= window_ms_) {
            last_value_ = static_cast<double>(quote_updates_);
            if (normalize_ && elapsed > 0) {
                last_value_ = std::log1p(last_value_);
            }
            window_start_ms_ = now_ms;
            quote_updates_ = 0;
        }

        if (debug_) {
            LOG_DEBUG("[QuoteIntensity500msFactor] value={} updates={}", last_value_, quote_updates_);
        }
        return last_value_;
    }

private:
    uint64_t window_ms_ = 500;
    uint64_t window_start_ms_ = 0;
    double last_bid_price_ = 0.0;
    double last_ask_price_ = 0.0;
    int last_bid_volume_ = 0;
    int last_ask_volume_ = 0;
    uint32_t quote_updates_ = 0;
    double last_value_ = 0.0;
    bool normalize_ = true;
    bool initialized_ = false;
    bool debug_ = false;
};

EXPORT_FACTOR(QuoteIntensity500msFactor)
