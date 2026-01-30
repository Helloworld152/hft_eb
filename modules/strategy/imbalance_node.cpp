#include "../../include/framework.h"
#include <iostream>
#include <cstring>

/**
 * ImbalanceNode: 挂单失衡因子 (Orderbook Imbalance)
 * 职责：计算买一卖一的挂单量差异，反映瞬时买卖压力
 */
class ImbalanceNode : public IStrategyNode {
public:
    void init(StrategyContext* ctx, const ConfigMap& config) override {
        ctx_ = ctx;
        if (config.find("debug") != config.end()) {
            debug_ = (config.at("debug") == "true");
        }
        if (debug_) ctx_->log("挂单失衡因子节点初始化完成。");
    }

    void onTick(const TickRecord* tick) override {
        double bid_vol = tick->bid_volume[0];
        double ask_vol = tick->ask_volume[0];
        
        if (bid_vol + ask_vol == 0) return;

        // 计算失衡度: (bid - ask) / (bid + ask)
        double imbalance = (bid_vol - ask_vol) / (bid_vol + ask_vol);

        SignalRecord sig;
        std::strncpy(sig.symbol, tick->symbol, sizeof(sig.symbol)-1);
        std::strncpy(sig.factor_name, "Imbalance", sizeof(sig.factor_name)-1);
        sig.value = imbalance;
        sig.timestamp = tick->update_time;

        ctx_->send_signal(sig);
    }

    void onKline(const KlineRecord* kline) override {}
    void onSignal(const SignalRecord* signal) override {}
    void onOrderUpdate(const OrderRtn* rtn) override {}

private:
    StrategyContext* ctx_;
    bool debug_ = false;
};

EXPORT_STRATEGY(ImbalanceNode)
