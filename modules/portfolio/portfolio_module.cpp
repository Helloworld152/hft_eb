#include "../../include/framework.h"
#include "../../core/include/core_state.h"
#include "../../core/include/symbol_manager.h"
#include "../../core/include/market_snapshot.h"
#include <yaml-cpp/yaml.h>
#include <string>
#include <mutex>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}
}

struct SignalState {
    double value = 0.0;
    uint64_t ts = 0;
};

class PortfolioModule : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config, ITimerService* /*timer_svc*/ = nullptr) override {
        bus_ = bus;
        parse_config(config);

        LOG_INFO("[Portfolio] Initialized. default_account={} signal_scale={} min_signal_threshold={} max_abs_pos={} max_order_size={} max_notional={} prefer_close_first={} signal_ttl_ms={} margin_rate={}",
                 default_account_,
                 signal_scale_,
                 min_signal_threshold_,
                 max_abs_pos_,
                 max_order_size_,
                 max_notional_,
                 prefer_close_first_ ? "true" : "false",
                 signal_ttl_ms_,
                 margin_rate_);

        bus_->subscribe(EVENT_SIGNAL, [this](void* d) {
            this->onSignal(static_cast<SignalRecord*>(d));
        });
    }

private:
    void parse_config(const ConfigMap& config) {
        if (config.count("default_account")) {
            default_account_ = config.at("default_account");
        }
        if (config.count("signal_scale")) {
            signal_scale_ = std::stod(config.at("signal_scale"));
        }
        if (config.count("min_signal_threshold")) {
            min_signal_threshold_ = std::stod(config.at("min_signal_threshold"));
        }
        if (config.count("max_abs_pos")) {
            max_abs_pos_ = std::stoi(config.at("max_abs_pos"));
        }
        if (config.count("max_order_size")) {
            max_order_size_ = std::stoi(config.at("max_order_size"));
        }
        if (config.count("max_notional")) {
            max_notional_ = std::stod(config.at("max_notional"));
        }
        if (config.count("prefer_close_first")) {
            prefer_close_first_ = (config.at("prefer_close_first") == "true");
        }
        if (config.count("signal_ttl_ms")) {
            signal_ttl_ms_ = std::stoull(config.at("signal_ttl_ms"));
        }
        if (config.count("margin_rate")) {
            margin_rate_ = std::stod(config.at("margin_rate"));
        }
        if (config.count("debug")) {
            debug_ = (config.at("debug") == "true");
        }

        auto it_yaml = config.find("_yaml");
        if (it_yaml == config.end()) return;

        try {
            YAML::Node node = YAML::Load(it_yaml->second);
            if (node["strategy_weights"] && node["strategy_weights"].IsMap()) {
                strategy_weights_.clear();
                for (YAML::const_iterator it = node["strategy_weights"].begin();
                     it != node["strategy_weights"].end(); ++it) {
                    std::string key = it->first.as<std::string>();
                    double w = it->second.as<double>();
                    strategy_weights_[key] = w;
                }
            }
        } catch (const YAML::Exception& e) {
            LOG_ERROR("[Portfolio] YAML parse error: {}", e.what());
        }
    }

    void onSignal(SignalRecord* sig) {
        if (!sig) return;
        uint64_t symbol_id = SymbolManager::instance().get_id(sig->symbol);
        if (symbol_id == 0) {
            if (debug_) {
                LOG_WARN("[Portfolio] Unknown symbol: {}", sig->symbol);
            }
            return;
        }

        const std::string source_id(sig->source_id);
        const uint64_t ts = sig->timestamp;

        int target_delta = 0;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            auto& per_symbol = signal_cache_[symbol_id];
            SignalState& st = per_symbol[source_id];
            st.value = sig->value;
            st.ts = ts;

            prune_expired_locked(per_symbol);

            double raw_sum = 0.0;
            for (const auto& kv : per_symbol) {
                const std::string& sid = kv.first;
                const SignalState& s = kv.second;

                double w = 1.0;
                auto it_w = strategy_weights_.find(sid);
                if (it_w != strategy_weights_.end()) w = it_w->second;
                raw_sum += s.value * w;
            }

            if (std::abs(raw_sum) < min_signal_threshold_) {
                target_delta = 0;
            } else {
                target_delta = static_cast<int>(std::round(raw_sum * signal_scale_));
            }
        }

        if (target_delta == 0) return;

        PositionDetail pos{};
        bool has_pos = false;
        AccountDetail acc{};
        bool has_acc = false;
        const auto& core = core::CoreServicesRegistry::get();
        if (core.account_store) {
            has_acc = core.account_store->get_account(default_account_.c_str(), &acc);
        }
        if (core.position_store) {
            has_pos = core.position_store->get_position(default_account_.c_str(), symbol_id, &pos);
        }

        int long_total = has_pos ? (pos.long_td + pos.long_yd) : 0;
        int short_total = has_pos ? (pos.short_td + pos.short_yd) : 0;
        int current_net = long_total - short_total;

        char direction = 0;
        char offset_flag = 'O';
        bool open_intent = true;
        bool close_intent = false;

        if (target_delta > 0) {
            direction = 'B';
            offset_flag = 'O';
            open_intent = true;
        } else {
            direction = 'S';
            if (prefer_close_first_ && long_total > 0) {
                offset_flag = 'C';
                open_intent = false;
                close_intent = true;
            } else {
                offset_flag = 'O';
                open_intent = true;
            }
        }

        // 1) 账户约束 (仅对开仓意图)
        if (open_intent && has_acc && max_order_size_ > 0) {
            double price = 0.0;
            double multiplier = 0.0;
            TickRecord tick;
            if (MarketSnapshot::instance().get(symbol_id, tick)) {
                price = tick.last_price;
            }
            multiplier = SymbolManager::instance().get_multiplier(symbol_id);

            if (price > 0.0 && multiplier > 0.0 && margin_rate_ > 0.0) {
                double margin_per_lot = price * multiplier * margin_rate_;
                if (margin_per_lot > 0.0) {
                    int max_open = static_cast<int>(acc.available / margin_per_lot);
                    if (max_open < 0) max_open = 0;
                    if (std::abs(target_delta) > max_open) {
                        target_delta = (target_delta > 0) ? max_open : -max_open;
                    }
                }
            }
        }

        // 2) 持仓约束 (净仓限制)
        if (max_abs_pos_ > 0) {
            int new_net = current_net + target_delta;
            if (std::abs(new_net) > max_abs_pos_) {
                int lo = -max_abs_pos_ - current_net;
                int hi = max_abs_pos_ - current_net;
                target_delta = clamp_int(target_delta, lo, hi);
            }
        }

        // 3) 风控约束
        if (max_order_size_ > 0 && std::abs(target_delta) > max_order_size_) {
            target_delta = (target_delta > 0) ? max_order_size_ : -max_order_size_;
        }

        if (max_notional_ > 0.0) {
            TickRecord tick;
            if (MarketSnapshot::instance().get(symbol_id, tick)) {
                double price = tick.last_price;
                double multiplier = SymbolManager::instance().get_multiplier(symbol_id);
                if (price > 0.0 && multiplier > 0.0) {
                    double per_lot = price * multiplier;
                    int max_by_notional = static_cast<int>(max_notional_ / per_lot);
                    if (max_by_notional < 0) max_by_notional = 0;
                    if (std::abs(target_delta) > max_by_notional) {
                        target_delta = (target_delta > 0) ? max_by_notional : -max_by_notional;
                    }
                }
            }
        }

        // Close-only guard: avoid closing more than available
        if (close_intent) {
            int closable = long_total;
            if (closable < 0) closable = 0;
            if (std::abs(target_delta) > closable) {
                target_delta = (target_delta > 0) ? closable : -closable;
            }
        }

        if (target_delta == 0) return;

        OrderReq req{};
        req.symbol_id = symbol_id;
        std::strncpy(req.symbol, sig->symbol, 31);
        req.symbol[31] = '\0';
        std::strncpy(req.account_id, default_account_.c_str(), 15);
        req.account_id[15] = '\0';
        req.direction = direction;
        req.offset_flag = offset_flag;
        req.price = 0.0;
        req.volume = std::abs(target_delta);

        if (debug_) {
            LOG_DEBUG("[Portfolio] OrderReq symbol={} dir={} off={} vol={} acc={}",
                      req.symbol,
                      req.direction,
                      req.offset_flag,
                      req.volume,
                      req.account_id);
        }

        bus_->publish(EVENT_ORDER_REQ, &req);
    }

    void prune_expired_locked(FastHashMap<std::string, SignalState>& per_symbol) {
        if (signal_ttl_ms_ == 0) return;
        uint64_t now = now_ms();
        for (auto it = per_symbol.begin(); it != per_symbol.end(); ) {
            const auto& st = it->second;
            if (st.ts > 0 && now > st.ts && (now - st.ts) > signal_ttl_ms_) {
                per_symbol.erase(it++);
            } else {
                ++it;
            }
        }
    }

private:
    EventBus* bus_ = nullptr;

    FastHashMap<std::string, double> strategy_weights_;
    FastHashMap<uint64_t, FastHashMap<std::string, SignalState>> signal_cache_;
    std::mutex mtx_;

    std::string default_account_ = "default";
    double signal_scale_ = 1.0;
    double min_signal_threshold_ = 0.0;
    int max_abs_pos_ = std::numeric_limits<int>::max();
    int max_order_size_ = std::numeric_limits<int>::max();
    double max_notional_ = 0.0;
    bool prefer_close_first_ = true;
    uint64_t signal_ttl_ms_ = 2000;
    double margin_rate_ = 1.0;
    bool debug_ = false;
};

EXPORT_MODULE(PortfolioModule)
