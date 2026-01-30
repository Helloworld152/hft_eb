#include "../../include/framework.h"
#include <iostream>
#include <cstring>
#include <unordered_map>

/**
 * CombinedStrategyNode: 组合决策节点
 * 功能：接收多个因子的信号 (EVENT_SIGNAL)，进行线性组合，达到阈值后发单
 */
class CombinedStrategyNode : public IStrategyNode {
public:
    void init(StrategyContext* ctx, const ConfigMap& config) override {
        ctx_ = ctx;
        
        // 读取调试开关
        if (config.find("debug") != config.end()) {
            debug_ = (config.at("debug") == "true");
        }
        
        // 阈值配置
        if (config.find("threshold") != config.end()) {
            threshold_ = std::stod(config.at("threshold"));
        }

        if (debug_) ctx_->log("组合决策节点初始化完成。");
    }

    void onSignal(const SignalRecord* signal) override {
        // 1. 记录/更新各因子的最新信号值
        signals_[signal->factor_name] = signal->value;

        // 2. 进行组合计算 (这里简单采用求和)
        double total_signal = 0;
        for (auto const& [name, val] : signals_) {
            total_signal += val;
        }

        if (debug_) {
            std::string msg = "收到信号 [" + std::string(signal->factor_name) + "]: " 
                            + std::to_string(signal->value) + " | 当前总分: " + std::to_string(total_signal);
            ctx_->log(msg.c_str());
        }

        // 3. 决策逻辑：根据组合后的信号发单
        if (total_signal >= threshold_) {
            if (debug_) ctx_->log(">>> 信号达标，触发买入请求");
            
            OrderReq req;
            strncpy(req.symbol, signal->symbol, 31);
            req.direction = 'B';
            req.offset_flag = 'O';
            req.price = 0; 
            req.volume = 1;
            ctx_->send_order(req);
            
            // 发单后重置信号（可选，视策略逻辑而定）
            signals_.clear();
        } 
        else if (total_signal <= -threshold_) {
            if (debug_) ctx_->log(">>> 信号达标，触发卖出请求");
            
            OrderReq req;
            strncpy(req.symbol, signal->symbol, 31);
            req.direction = 'S';
            req.offset_flag = 'O';
            req.price = 0;
            req.volume = 1;
            ctx_->send_order(req);
            
            signals_.clear();
        }
    }

    // 行情数据仅用于参考或风控，不在此计算因子
    void onTick(const TickRecord* tick) override {}
    void onKline(const KlineRecord* kline) override {}
    void onOrderUpdate(const OrderRtn* rtn) override {
        if (debug_) ctx_->log("收到报单回报。");
    }

private:
    StrategyContext* ctx_;
    bool debug_ = false;
    double threshold_ = 1.0;
    
    // 存储各因子的最新状态
    std::unordered_map<std::string, double> signals_;
};

EXPORT_STRATEGY(CombinedStrategyNode)