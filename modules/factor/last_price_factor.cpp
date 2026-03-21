#include "../../include/factor/factor_node.h"
#include <iostream>

class LastPriceFactor : public IFactorNode {
public:
    void init(const ConfigMap& config) override {
        auto it = config.find("debug");
        if (it != config.end()) {
            debug_ = (it->second == "true");
        }
    }

    double compute(const FactorContext& ctx, const std::vector<double>& /*inputs*/) override {
        if (ctx.tick) {
            last_value_ = ctx.tick->last_price;
        } else if (ctx.kline) {
            last_value_ = ctx.kline->close;
        }
        if (debug_) {
            std::cout << "[LastPriceFactor] value=" << last_value_ << std::endl;
        }
        return last_value_;
    }

private:
    double last_value_ = 0.0;
    bool debug_ = false;
};

EXPORT_FACTOR(LastPriceFactor)
