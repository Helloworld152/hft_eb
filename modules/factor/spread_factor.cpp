#include "factor/factor_node.h"

class SpreadFactor : public IFactorNode {
public:
    void init(const ConfigMap& config) override {
        auto it = config.find("debug");
        if (it != config.end()) {
            debug_ = (it->second == "true");
        }
    }

    double compute(const FactorContext& ctx, const std::vector<double>& /*inputs*/) override {
        if (ctx.tick) {
            double bid = ctx.tick->bid_price[0];
            double ask = ctx.tick->ask_price[0];
            last_value_ = (ask - bid);
        }

        if (debug_) {
            LOG_DEBUG("[SpreadFactor] value={}", last_value_);
        }
        return last_value_;
    }

private:
    double last_value_ = 0.0;
    bool debug_ = false;
};

EXPORT_FACTOR(SpreadFactor)
