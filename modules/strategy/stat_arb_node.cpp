#include "../../include/framework.h"
#include <deque>
#include <numeric>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <string>

/**
 * StatArbNode: 统计套利策略节点 (均值回归)
 * 职责：计算 Z-Score，当偏离超过 N 个标准差时开仓，回归时平仓。
 */
class StatArbNode : public IStrategyNode {
public:
    void init(StrategyContext* ctx, const ConfigMap& config) override {
        ctx_ = ctx;
        
        // 参数读取
        if (config.find("symbol") != config.end()) symbol_ = config.at("symbol");
        if (config.find("window_size") != config.end()) window_size_ = std::stoul(config.at("window_size"));
        if (config.find("sigma") != config.end()) sigma_threshold_ = std::stod(config.at("sigma"));
        if (config.find("debug") != config.end()) debug_ = (config.at("debug") == "true");

        if (debug_) {
            std::string msg = "StatArbNode 初始化: 合约=" + symbol_ + 
                            ", 窗口=" + std::to_string(window_size_) + 
                            ", 阈值=" + std::to_string(sigma_threshold_) + " sigma";
            ctx_->log(msg.c_str());
        }
    }

    void onTick(const TickRecord* tick) override {
        // 过滤合约
        if (symbol_ != tick->symbol) return;

        // 更新价格序列
        prices_.push_back(tick->last_price);
        if (prices_.size() > window_size_) {
            prices_.pop_front();
        }

        // 窗口未满不执行
        if (prices_.size() < window_size_) return;

        // 计算均值和标准差
        double sum = std::accumulate(prices_.begin(), prices_.end(), 0.0);
        double mean = sum / prices_.size();

        double sq_sum = 0;
        for (double p : prices_) {
            sq_sum += (p - mean) * (p - mean);
        }
        double stdev = std::sqrt(sq_sum / prices_.size());

        if (stdev < 0.00001) return; // 防止除零

        // 计算 Z-Score
        double z_score = (tick->last_price - mean) / stdev;

        if (debug_) {
            static int log_cnt = 0;
            if (++log_cnt % 100 == 0) { // 减少日志频率
                std::string msg = "Symbol: " + symbol_ + " | Last: " + std::to_string(tick->last_price) + 
                                " | Mean: " + std::to_string(mean) + " | Z: " + std::to_string(z_score);
                ctx_->log(msg.c_str());
            }
        }

        // 交易逻辑：4个标准差均值回归
        executeLogic(tick, z_score);
    }

    void executeLogic(const TickRecord* tick, double z_score) {
        // pos_ 状态：1=持多, -1=持空, 0=空仓
        if (pos_ == 0) {
            if (z_score > sigma_threshold_) {
                // 价格过高，卖出开空
                ctx_->log(">>> Z-Score 突破上轨，开空");
                sendOrder(tick->symbol, 'S', 'O', tick->last_price);
                pos_ = -1;
            } else if (z_score < -sigma_threshold_) {
                // 价格过低，买入开多
                ctx_->log(">>> Z-Score 突破下轨，开多");
                sendOrder(tick->symbol, 'B', 'O', tick->last_price);
                pos_ = 1;
            }
        } 
        else if (pos_ == 1) {
            // 持有多头，等待回归均值 (Z-Score 回到 0 附近，比如 0.5)
            if (z_score >= -0.5) {
                ctx_->log(">>> Z-Score 回归，平多");
                sendOrder(tick->symbol, 'S', 'C', tick->last_price);
                pos_ = 0;
            }
        } 
        else if (pos_ == -1) {
            // 持有空头，等待回归均值
            if (z_score <= 0.5) {
                ctx_->log(">>> Z-Score 回归，平空");
                sendOrder(tick->symbol, 'B', 'C', tick->last_price);
                pos_ = 0;
            }
        }
    }

    void sendOrder(const char* symbol, char dir, char offset, double price) {
        OrderReq req;
        strncpy(req.symbol, symbol, 31);
        req.direction = dir;
        req.offset_flag = offset;
        req.price = price;
        req.volume = 1;
        ctx_->send_order(req);
    }

    void onKline(const KlineRecord* kline) override {}
    void onSignal(const SignalRecord* signal) override {}
    void onOrderUpdate(const OrderRtn* rtn) override {
        // 这里可以根据成交回报精确维护 pos_
        // 简化版暂不处理，假设发单即成交（在 TradeSim 环境下）
    }

private:
    StrategyContext* ctx_;
    std::string symbol_ = "au2606";
    size_t window_size_ = 60;
    double sigma_threshold_ = 4.0;
    bool debug_ = false;

    std::deque<double> prices_;
    int pos_ = 0; 
};

EXPORT_STRATEGY(StatArbNode)
