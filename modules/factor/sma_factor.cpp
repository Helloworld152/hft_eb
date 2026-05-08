#include "../../include/factor/factor_node.h"
#include <vector>
#include <cstdlib>

class SmaFactor : public IFactorNode {
public:
    void init(const ConfigMap& config) override {
        auto it = config.find("window");
        if (it == config.end()) it = config.find("window_size");
        if (it != config.end()) {
            int v = std::atoi(it->second.c_str());
            if (v > 0) window_ = v;
        }
        buffer_.assign(window_, 0.0);

        auto it_dbg = config.find("debug");
        if (it_dbg != config.end()) {
            debug_ = (it_dbg->second == "true");
        }
    }

    double compute(const FactorContext& ctx, const std::vector<double>& inputs) override {
        double value = 0.0;
        bool has_input = false;
        if (!inputs.empty()) {
            value = inputs[0];
            has_input = true;
        } else if (ctx.tick) {
            value = ctx.tick->last_price;
            has_input = true;
        } else if (ctx.kline) {
            value = ctx.kline->close;
            has_input = true;
        }

        if (has_input) {
            sum_ -= buffer_[index_];
            buffer_[index_] = value;
            sum_ += value;
            index_ = (index_ + 1) % window_;
            if (count_ < window_) count_++;
            last_value_ = sum_ / static_cast<double>(count_);
        }

        if (debug_) {
            LOG_DEBUG("[SmaFactor] value={} window={}", last_value_, window_);
        }
        return last_value_;
    }

private:
    int window_ = 5;
    std::vector<double> buffer_;
    int index_ = 0;
    int count_ = 0;
    double sum_ = 0.0;
    double last_value_ = 0.0;
    bool debug_ = false;
};

EXPORT_FACTOR(SmaFactor)
