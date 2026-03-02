#include "../../include/framework.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <yaml-cpp/yaml.h>

namespace {
constexpr double kEps = 1e-9;

struct SymbolState {
    std::vector<double> values;
    std::vector<unsigned char> seen;
};

bool parse_symbols_file(const std::string& path, std::vector<std::string>* symbols) {
    std::ifstream in(path);
    if (!in.is_open()) return false;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream iss(line);
        std::string token;
        // Format: id:symbol:mult
        if (!std::getline(iss, token, ':')) continue;
        if (!std::getline(iss, token, ':')) continue;
        if (!token.empty()) symbols->push_back(token);
    }
    return !symbols->empty();
}

} // namespace

/**
 * CrossSectionCombinerNode: 多因子截面组合节点
 * - 接收因子信号 (SignalRecord)
 * - 按品种聚合为因子矩阵
 * - 在 1 分钟 K线触发时做截面 z-score + 线性组合
 * - 根据排序输出目标仓位 (净暴露 0)
 */
class CrossSectionCombinerNode : public IStrategyNode {
public:
    void init(StrategyContext* ctx, const ConfigMap& config) override {
        ctx_ = ctx;
        if (config.find("debug") != config.end()) debug_ = (config.at("debug") == "true");
        if (config.find("top_pct") != config.end()) top_pct_ = std::stod(config.at("top_pct"));
        if (config.find("bottom_pct") != config.end()) bottom_pct_ = std::stod(config.at("bottom_pct"));
        if (config.find("top_n") != config.end()) top_n_ = std::stoi(config.at("top_n"));
        if (config.find("bottom_n") != config.end()) bottom_n_ = std::stoi(config.at("bottom_n"));
        if (config.find("base_volume") != config.end()) base_volume_ = std::stoi(config.at("base_volume"));
        if (config.find("symbols_file") != config.end()) symbols_file_ = config.at("symbols_file");

        load_weights_from_yaml(config);
        load_universe();
    }

    void onTick(const TickRecord* tick) override {
        (void)tick;
    }

    void onKline(const KlineRecord* kline) override {
        if (kline->interval != K_1M) return;
        rebalance();
    }

    void onSignal(const SignalRecord* signal) override {
        auto it = factor_index_.find(signal->factor_name);
        if (it == factor_index_.end()) return;

        const int idx = it->second;
        SymbolState& st = symbol_state_[signal->symbol];
        if (st.values.empty()) {
            st.values.assign(factor_names_.size(), 0.0);
            st.seen.assign(factor_names_.size(), 0);
        }
        st.values[idx] = signal->value;
        st.seen[idx] = 1;
    }

    void onOrderUpdate(const OrderRtn* rtn) override {
        // Track executed volume to approximate net position.
        if (rtn->order_ref[0] == '\0') return;
        const std::string order_ref(rtn->order_ref);
        int last = 0;
        auto it = order_traded_.find(order_ref);
        if (it != order_traded_.end()) last = it->second;
        int delta = rtn->volume_traded - last;
        if (delta <= 0) return;

        order_traded_[order_ref] = rtn->volume_traded;
        int sign = (rtn->direction == 'B') ? 1 : -1;
        position_[rtn->symbol] += sign * delta;
    }

private:
    void load_weights_from_yaml(const ConfigMap& config) {
        if (config.find("_yaml") == config.end()) return;
        try {
            YAML::Node node = YAML::Load(config.at("_yaml"));
            if (!node["weights"] || !node["weights"].IsMap()) return;

            int idx = 0;
            for (auto it = node["weights"].begin(); it != node["weights"].end(); ++it) {
                std::string fname = it->first.as<std::string>();
                double w = it->second.as<double>();
                factor_names_.push_back(fname);
                factor_weights_.push_back(w);
                factor_index_[fname] = idx++;
                if (debug_) ctx_->log(("加载权重: " + fname + "=" + std::to_string(w)).c_str());
            }
        } catch (...) {
            if (debug_) ctx_->log("权重解析失败");
        }
    }

    void load_universe() {
        if (symbols_file_.empty()) return;
        parse_symbols_file(symbols_file_, &universe_);
        if (debug_ && !universe_.empty()) {
            ctx_->log(("加载品种池: " + std::to_string(universe_.size())).c_str());
        }
    }

    bool symbol_ready(const SymbolState& st) const {
        if (st.seen.size() != factor_names_.size()) return false;
        for (size_t i = 0; i < st.seen.size(); ++i) {
            if (st.seen[i] == 0) return false;
        }
        return true;
    }

    void rebalance() {
        if (factor_names_.empty()) return;

        // Collect symbols that are ready.
        std::vector<std::string> symbols;
        symbols.reserve(symbol_state_.size());
        if (!universe_.empty()) {
            for (const auto& sym : universe_) {
                auto it = symbol_state_.find(sym);
                if (it != symbol_state_.end() && symbol_ready(it->second)) symbols.push_back(sym);
            }
        } else {
            for (const auto& kv : symbol_state_) {
                if (symbol_ready(kv.second)) symbols.push_back(kv.first);
            }
        }
        if (symbols.size() < 2) return;

        const size_t n = symbols.size();
        const size_t k = factor_names_.size();

        // Precompute mean/std for each factor across symbols.
        std::vector<double> mean(k, 0.0);
        std::vector<double> var(k, 0.0);
        for (size_t i = 0; i < k; ++i) {
            for (const auto& sym : symbols) {
                mean[i] += symbol_state_[sym].values[i];
            }
            mean[i] /= static_cast<double>(n);
            for (const auto& sym : symbols) {
                double diff = symbol_state_[sym].values[i] - mean[i];
                var[i] += diff * diff;
            }
            var[i] /= static_cast<double>(n);
        }

        std::vector<std::pair<std::string, double>> scores;
        scores.reserve(n);
        for (const auto& sym : symbols) {
            const auto& vals = symbol_state_[sym].values;
            double score = 0.0;
            for (size_t i = 0; i < k; ++i) {
                double stdv = std::sqrt(var[i]);
                double z = (stdv > kEps) ? (vals[i] - mean[i]) / stdv : 0.0;
                score += z * factor_weights_[i];
            }
            scores.emplace_back(sym, score);
        }

        std::sort(scores.begin(), scores.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });

        int top_n = top_n_;
        int bottom_n = bottom_n_;
        if (top_n <= 0) top_n = static_cast<int>(std::round(top_pct_ * static_cast<double>(n)));
        if (bottom_n <= 0) bottom_n = static_cast<int>(std::round(bottom_pct_ * static_cast<double>(n)));
        if (top_n <= 0 || bottom_n <= 0) return;
        int kside = std::min(top_n, bottom_n);
        if (kside <= 0) return;

        std::unordered_map<std::string, int> targets;
        targets.reserve(n);
        for (int i = 0; i < kside; ++i) {
            targets[scores[i].first] = base_volume_; // long
            const auto& sym = scores[n - 1 - i].first;
            targets[sym] = -base_volume_; // short
        }

        for (const auto& sym : symbols) {
            int target = 0;
            auto it = targets.find(sym);
            if (it != targets.end()) target = it->second;
            int current = 0;
            auto itp = position_.find(sym);
            if (itp != position_.end()) current = itp->second;
            int delta = target - current;
            if (delta == 0) continue;
            send_order(sym.c_str(), delta);
        }

        if (debug_) {
            ctx_->log(("截面重平完成, N=" + std::to_string(n) + ", kside=" + std::to_string(kside)).c_str());
        }
    }

    void send_order(const char* symbol, int delta) {
        OrderReq req;
        std::memset(&req, 0, sizeof(req));
        std::strncpy(req.symbol, symbol, sizeof(req.symbol) - 1);
        req.direction = (delta > 0) ? 'B' : 'S';
        req.offset_flag = 'O';
        req.price = 0;
        req.volume = std::abs(delta);
        ctx_->send_order(req);
    }

private:
    StrategyContext* ctx_ = nullptr;
    bool debug_ = false;
    double top_pct_ = 0.2;
    double bottom_pct_ = 0.2;
    int top_n_ = 0;
    int bottom_n_ = 0;
    int base_volume_ = 1;
    std::string symbols_file_ = "../conf/symbols.txt";

    std::vector<std::string> factor_names_;
    std::vector<double> factor_weights_;
    std::unordered_map<std::string, int> factor_index_;

    std::unordered_map<std::string, SymbolState> symbol_state_;
    std::vector<std::string> universe_;

    std::unordered_map<std::string, int> position_;
    std::unordered_map<std::string, int> order_traded_;
};

EXPORT_STRATEGY(CrossSectionCombinerNode)
