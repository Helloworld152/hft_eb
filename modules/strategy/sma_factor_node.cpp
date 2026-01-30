#include "../../include/framework.h"
#include <iostream>
#include <vector>
#include <numeric>
#include <deque>
#include <cstring>
#include <unordered_map>
#include <string>

/**
 * SmaFactorNode: 简单移动平均因子
 * 职责：计算价格的 SMA，并输出 (当前价 - SMA) 作为信号
 */
class SmaFactorNode : public IStrategyNode {
public:
    void init(StrategyContext* ctx, const ConfigMap& config) override {
        ctx_ = ctx;
        if (config.find("window_size") != config.end()) {
            window_size_ = std::stoi(config.at("window_size"));
        }
        if (config.find("multiplier") != config.end()) {
            multiplier_ = std::stod(config.at("multiplier"));
        }
        if (config.find("debug") != config.end()) {
            debug_ = (config.at("debug") == "true");
        }
        
        // 预分配内存，防止运行时 resize
        price_history_.reserve(16); 
    }

    void onTick(const TickRecord* tick) override {
        std::string symbol = tick->symbol;
        
        // 获取或创建该品种的 RingBuffer (首次创建会分配，之后复用)
        // 注意：这里仍然有一次 map 查找，但在 HFT 中通常会对 Symbol 预先建立索引
        // 为了演示方便，我们先保留 map，但优化内部结构
        if (price_history_.find(symbol) == price_history_.end()) {
            price_history_.emplace(symbol, RingBuffer(window_size_));
        }
        
        RingBuffer& buf = price_history_.at(symbol);
        
        // O(1) 增量更新
        buf.add(tick->last_price);

        if (!buf.is_full()) return;

        double sma = buf.sum() / window_size_;
        
        double raw_diff = (tick->last_price - sma) / sma; 
        double normalized_sig = raw_diff * multiplier_;

        if (normalized_sig > 1.0) normalized_sig = 1.0;
        if (normalized_sig < -1.0) normalized_sig = -1.0;

        SignalRecord sig;
        std::strncpy(sig.symbol, tick->symbol, sizeof(sig.symbol)-1);
        std::strncpy(sig.factor_name, "SMA_Diff", sizeof(sig.factor_name)-1);
        sig.value = normalized_sig; 
        sig.timestamp = tick->update_time;

        ctx_->send_signal(sig);
    }

    void onKline(const KlineRecord* kline) override {}
    void onSignal(const SignalRecord* signal) override {}
    void onOrderUpdate(const OrderRtn* rtn) override {}

private:
    // 高性能环形缓冲区
    struct RingBuffer {
        std::vector<double> data;
        size_t size = 0;
        size_t capacity = 0;
        size_t cursor = 0; // 指向下一个写入位置
        double current_sum = 0.0;

        RingBuffer(size_t cap) : capacity(cap) {
            data.resize(cap, 0.0);
        }

        void add(double val) {
            // 减去将被覆盖的旧值
            double old_val = data[cursor];
            
            // 写入新值
            data[cursor] = val;
            
            // 更新和 (O(1))
            // 注意：浮点数累加可能有精度误差，但在 HFT 短周期内可接受
            // 严谨做法是每隔 N 次重算一次，这里追求极致速度忽略之
            current_sum = current_sum - old_val + val;

            // 移动游标
            cursor++;
            if (cursor >= capacity) cursor = 0;
            if (size < capacity) size++;
        }

        bool is_full() const { return size >= capacity; }
        double sum() const { return current_sum; }
    };

    StrategyContext* ctx_;
    size_t window_size_ = 20;
    double multiplier_ = 1000.0;
    std::unordered_map<std::string, RingBuffer> price_history_;
    bool debug_ = false;
};

EXPORT_STRATEGY(SmaFactorNode)
