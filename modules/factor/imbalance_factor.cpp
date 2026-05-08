#include "../../include/factor/factor_node.h"

class ImbalanceFactor : public IFactorNode {
public:
    void init(const ConfigMap& config) override {
        auto it = config.find("debug");
        if (it != config.end()) {
            debug_ = (it->second == "true");
        }
    }

    double compute(const FactorContext& ctx, const std::vector<double>& /*inputs*/) override {
        if (ctx.tick) {
            double bid_vol = static_cast<double>(ctx.tick->bid_volume[0]);
            double ask_vol = static_cast<double>(ctx.tick->ask_volume[0]);
            double denom = bid_vol + ask_vol;
            if (denom > 0.0) {
                last_value_ = (bid_vol - ask_vol) / denom;
            } else {
                last_value_ = 0.0;
            }
        }

        if (debug_) {
            LOG_DEBUG("[ImbalanceFactor] value={}", last_value_);
        }
        return last_value_;
    }

private:
    double last_value_ = 0.0;
    bool debug_ = false;
};

EXPORT_FACTOR(ImbalanceFactor)
