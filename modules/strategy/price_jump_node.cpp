#include "../../include/framework.h"
#include <iostream>
#include <cstring>
#include <cmath>

/**
 * PriceJumpNode: 计算因子节点
 * 职责：监听 Tick，产生价格跳变信号 (EVENT_SIGNAL)
 */
class PriceJumpNode : public IStrategyNode {
public:
    void init(StrategyContext* ctx, const ConfigMap& config) override {
        ctx_ = ctx;
        if (config.find("threshold") != config.end()) threshold_ = std::stod(config.at("threshold"));
        if (config.find("debug") != config.end()) debug_ = (config.at("debug") == "true");

        if (debug_) ctx_->log("价格跳变因子节点初始化完成。");
    }

    void onTick(const TickRecord* tick) override {
        static double last_price = 0;
        if (last_price == 0) {
            last_price = tick->last_price;
            return;
        }

        double diff = tick->last_price - last_price;
        if (std::abs(diff) >= threshold_) {
            SignalRecord sig;
            strncpy(sig.symbol, tick->symbol, 31);
            strncpy(sig.factor_name, "PriceJump", 31);
            sig.value = (diff > 0) ? 1.0 : -1.0;
            sig.timestamp = tick->update_time;
            
            if (debug_) {
                std::string msg = "产生信号 [PriceJump]: " + std::to_string(sig.value);
                ctx_->log(msg.c_str());
            }
            
            // 发出信号
            ctx_->send_signal(sig);
            last_price = tick->last_price;
        }
    }

    void onKline(const KlineRecord* kline) override {}
    void onSignal(const SignalRecord* signal) override {}
    void onOrderUpdate(const OrderRtn* rtn) override {}

private:
    StrategyContext* ctx_;
    double threshold_ = 0.2;
    bool debug_ = false;
};

EXPORT_STRATEGY(PriceJumpNode)
