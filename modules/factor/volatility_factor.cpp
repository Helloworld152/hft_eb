#include "factor/factor_node.h"
#include <vector>
#include <cstdlib>
#include <cmath>

class VolatilityFactor : public IFactorNode {
public:
    void init(const ConfigMap& config) override {
        auto it = config.find("window");
        if (it == config.end()) it = config.find("window_size");
        if (it != config.end()) {
            int v = std::atoi(it->second.c_str());
            if (v > 0) window_ = v;
        }
        returns_.assign(window_, 0.0);

        auto it_dbg = config.find("debug");
        if (it_dbg != config.end()) {
            debug_ = (it_dbg->second == "true");
        }
    }

    double compute(const FactorContext& ctx, const std::vector<double>& inputs) override {
        double price = 0.0;
        bool has_price = false;
        if (!inputs.empty()) {
            price = inputs[0];
            has_price = true;
        } else if (ctx.tick) {
            price = ctx.tick->last_price;
            has_price = true;
        } else if (ctx.kline) {
            price = ctx.kline->close;
            has_price = true;
        }

        if (has_price) {
            if (has_prev_) {
                double ret = (price - prev_price_) / prev_price_;
                sum_ -= returns_[index_];
                sum_sq_ -= returns_[index_] * returns_[index_];
                returns_[index_] = ret;
                sum_ += ret;
                sum_sq_ += ret * ret;
                index_ = (index_ + 1) % window_;
                if (count_ < window_) count_++;
            } else {
                has_prev_ = true;
            }
            prev_price_ = price;
        }

        if (count_ > 1) {
            double mean = sum_ / static_cast<double>(count_);
            double var = (sum_sq_ / static_cast<double>(count_)) - mean * mean;
            if (var < 0.0) var = 0.0;
            last_value_ = std::sqrt(var);
        }

        if (debug_) {
            LOG_DEBUG("[VolatilityFactor] value={} window={}", last_value_, window_);
        }
        return last_value_;
    }

private:
    int window_ = 20;
    std::vector<double> returns_;
    int index_ = 0;
    int count_ = 0;
    double sum_ = 0.0;
    double sum_sq_ = 0.0;
    double prev_price_ = 0.0;
    bool has_prev_ = false;
    double last_value_ = 0.0;
    bool debug_ = false;
};

EXPORT_FACTOR(VolatilityFactor)
