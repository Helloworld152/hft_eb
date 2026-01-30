#include "../../include/framework.h"
#include <iostream>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <yaml-cpp/yaml.h>

/**
 * CombinedStrategyNode: 组合决策节点
 * 功能：接收多个因子的信号 (EVENT_SIGNAL)，进行线性组合，达到阈值后发单
 */
class CombinedStrategyNode : public IStrategyNode {
public:
    void init(StrategyContext* ctx, const ConfigMap& config) override {
        ctx_ = ctx;
        
        if (config.find("debug") != config.end()) debug_ = (config.at("debug") == "true");
        if (config.find("threshold") != config.end()) threshold_ = std::stod(config.at("threshold"));

        // 解析权重配置
        if (config.find("_yaml") != config.end()) {
            try {
                YAML::Node node = YAML::Load(config.at("_yaml"));
                if (node["weights"] && node["weights"].IsMap()) {
                    int idx = 0;
                    for (YAML::const_iterator it = node["weights"].begin(); it != node["weights"].end(); ++it) {
                        std::string fname = it->first.as<std::string>();
                        double w = it->second.as<double>();
                        
                        // 建立映射: Name -> Index
                        // 我们保存 char* 副本以便快速比较 (假设 factor_name 不变且生命周期长，
                        // 但为了安全，我们保存 string 并在 onSignal 中用 c_str() 比较)
                        factor_names_.push_back(fname);
                        weight_values_.push_back(w);
                        
                        if (debug_) ctx_->log(("权重加载: " + fname + " = " + std::to_string(w) + " [Idx:" + std::to_string(idx) + "]").c_str());
                        idx++;
                    }
                }
            } catch(...) {
                ctx_->log("权重解析失败");
            }
        }
        
        // 预分配信号值向量，默认值为 0
        signal_values_.resize(factor_names_.size(), 0.0);
    }

    void onSignal(const SignalRecord* signal) override {
        // [Fast Path] 线性扫描匹配因子名称
        // 对于因子数量少的情况 (< 20)，strcmp 线性扫描比构造 std::string + hash map 快得多
        // 且完全没有内存分配
        int found_idx = -1;
        for (size_t i = 0; i < factor_names_.size(); ++i) {
            // 比较 char*
            if (std::strncmp(signal->factor_name, factor_names_[i].c_str(), 31) == 0) {
                found_idx = (int)i;
                break;
            }
        }

        // 如果不是我们要关注的因子，直接忽略
        if (found_idx == -1) return;

        // 1. 更新信号值 (O(1) 数组访问)
        signal_values_[found_idx] = signal->value;

        // 2. 进行组合计算 (向量点积)
        double total_signal = 0;
        // 编译器可以自动向量化 (SIMD) 这个循环
        for (size_t i = 0; i < signal_values_.size(); ++i) {
            total_signal += signal_values_[i] * weight_values_[i];
        }

        if (debug_) {
            std::string msg = "收到信号 [" + std::string(signal->factor_name) + "]: " 
                            + std::to_string(signal->value) + " | 当前总分: " + std::to_string(total_signal);
            ctx_->log(msg.c_str());
        }

        // 3. 决策逻辑
        if (total_signal >= threshold_) {
            send_order(signal->symbol, 'B');
            // 重置信号？通常组合策略是持续状态，不需要清空，这里保持原逻辑
            std::fill(signal_values_.begin(), signal_values_.end(), 0.0);
        } 
        else if (total_signal <= -threshold_) {
            send_order(signal->symbol, 'S');
            std::fill(signal_values_.begin(), signal_values_.end(), 0.0);
        }
    }

    void send_order(const char* symbol, char dir) {
        if (debug_) ctx_->log(">>> 信号达标，触发请求");
        OrderReq req;
        strncpy(req.symbol, symbol, 31);
        req.direction = dir;
        req.offset_flag = 'O';
        req.price = 0; 
        req.volume = 1;
        ctx_->send_order(req);
    }

    void onTick(const TickRecord* tick) override {}
    void onKline(const KlineRecord* kline) override {}
    void onOrderUpdate(const OrderRtn* rtn) override {
        if (debug_) ctx_->log("收到报单回报。");
    }

private:
    StrategyContext* ctx_;
    bool debug_ = false;
    double threshold_ = 1.0;
    
    // 平铺的数据结构 (SoA - Structure of Arrays)
    std::vector<std::string> factor_names_; // 用于查找
    std::vector<double> weight_values_;     // 权重
    std::vector<double> signal_values_;     // 当前值
};

EXPORT_STRATEGY(CombinedStrategyNode)