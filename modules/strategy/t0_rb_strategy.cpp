#include "../../include/framework.h"
#include "../../core/include/core_state.h"
#include "../../core/include/symbol_manager.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>

namespace {

uint64_t hhmmssmmm_to_ms(uint64_t hhmmssmmm) {
    uint64_t ms = hhmmssmmm % 1000ULL;
    uint64_t total_sec = hhmmssmmm / 1000ULL;
    uint64_t sec = total_sec % 100ULL;
    uint64_t total_min = total_sec / 100ULL;
    uint64_t min = total_min % 100ULL;
    uint64_t hour = total_min / 100ULL;
    return ((hour * 60ULL + min) * 60ULL + sec) * 1000ULL + ms;
}

double best_buy_price(const TickRecord& tick) {
    if (tick.ask_price[0] > 0.0) return tick.ask_price[0];
    if (tick.last_price > 0.0) return tick.last_price;
    return tick.bid_price[0];
}

double best_sell_price(const TickRecord& tick) {
    if (tick.bid_price[0] > 0.0) return tick.bid_price[0];
    if (tick.last_price > 0.0) return tick.last_price;
    return tick.ask_price[0];
}

}

class T0RbStrategyModule : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config, ITimerService* timer_svc = nullptr) override {
        (void)timer_svc;
        bus_ = bus;

        if (config.count("symbol")) {
            symbol_ = config.at("symbol");
        }
        symbol_id_ = SymbolManager::instance().get_id(symbol_.c_str());

        if (config.count("signal_name")) signal_name_ = config.at("signal_name");
        if (config.count("default_account")) default_account_ = config.at("default_account");
        if (config.count("decision_interval_ms")) decision_interval_ms_ = std::stoull(config.at("decision_interval_ms"));
        if (config.count("entry_threshold_long")) entry_threshold_long_ = std::stod(config.at("entry_threshold_long"));
        if (config.count("entry_threshold_short")) entry_threshold_short_ = std::stod(config.at("entry_threshold_short"));
        if (config.count("exit_threshold_abs")) exit_threshold_abs_ = std::stod(config.at("exit_threshold_abs"));
        if (config.count("max_abs_pos")) max_abs_pos_ = std::max(1, std::stoi(config.at("max_abs_pos")));
        if (config.count("order_volume")) order_volume_ = std::max(1, std::stoi(config.at("order_volume")));
        if (config.count("max_hold_ms")) max_hold_ms_ = std::stoull(config.at("max_hold_ms"));
        if (config.count("min_reentry_ms")) min_reentry_ms_ = std::stoull(config.at("min_reentry_ms"));
        if (config.count("trade_side")) trade_side_ = config.at("trade_side");
        if (config.count("debug")) debug_ = (config.at("debug") == "true");

        bus_->subscribe(EVENT_MARKET_DATA,
                        StaticDelegate<void(void*)>::bind<T0RbStrategyModule, &T0RbStrategyModule::on_tick_event>(this));
        bus_->subscribe(EVENT_SIGNAL,
                        StaticDelegate<void(void*)>::bind<T0RbStrategyModule, &T0RbStrategyModule::on_signal_event>(this));

        std::cout << "[T0RbStrategy] Initialized. symbol=" << symbol_
                  << " signal_name=" << signal_name_
                  << " decision_interval_ms=" << decision_interval_ms_
                  << " trade_side=" << trade_side_
                  << " max_abs_pos=" << max_abs_pos_ << std::endl;
    }

private:
    void on_tick_event(void* d) {
        auto* tick = static_cast<TickRecord*>(d);
        if (!tick || tick->symbol_id != symbol_id_) return;
        last_tick_ = *tick;
        has_tick_ = true;
        maybe_decide();
    }

    void on_signal_event(void* d) {
        auto* sig = static_cast<SignalRecord*>(d);
        if (!sig) return;
        if (std::strncmp(sig->symbol, symbol_.c_str(), sizeof(sig->symbol)) != 0) return;
        if (signal_name_ != sig->factor_name) return;
        latest_alpha_ = sig->value;
        latest_alpha_ts_ms_ = hhmmssmmm_to_ms(sig->timestamp);
    }

    void maybe_decide() {
        if (!has_tick_ || symbol_id_ == 0) return;

        uint64_t now_ms = hhmmssmmm_to_ms(last_tick_.update_time);
        if (now_ms == 0 && last_tick_.update_time != 0) return;

        uint64_t bucket = now_ms / decision_interval_ms_;
        if (bucket == last_decision_bucket_) return;
        last_decision_bucket_ = bucket;

        PositionDetail pos{};
        int long_total = 0;
        int short_total = 0;
        if (const auto& core = core::CoreServicesRegistry::get(); core.position_store) {
            if (core.position_store->get_position(default_account_.c_str(), symbol_id_, &pos)) {
                long_total = pos.long_td + pos.long_yd;
                short_total = pos.short_td + pos.short_yd;
            }
        }

        int current_net = long_total - short_total;
        if (current_net != 0 && position_entry_ms_ == 0) {
            position_entry_ms_ = now_ms;
        } else if (current_net == 0) {
            position_entry_ms_ = 0;
        }

        if (current_net != 0 && max_hold_ms_ > 0 && position_entry_ms_ > 0 &&
            now_ms >= position_entry_ms_ && now_ms - position_entry_ms_ >= max_hold_ms_) {
            close_position(pos, long_total, short_total, "max_hold");
            return;
        }

        if (current_net > 0) {
            if (latest_alpha_ <= entry_threshold_short_ || std::abs(latest_alpha_) <= exit_threshold_abs_) {
                close_position(pos, long_total, short_total,
                               latest_alpha_ <= entry_threshold_short_ ? "reverse_to_short" : "alpha_exit_long");
            }
            return;
        }

        if (current_net < 0) {
            if (latest_alpha_ >= entry_threshold_long_ || std::abs(latest_alpha_) <= exit_threshold_abs_) {
                close_position(pos, long_total, short_total,
                               latest_alpha_ >= entry_threshold_long_ ? "reverse_to_long" : "alpha_exit_short");
            }
            return;
        }

        if (last_exit_ms_ > 0 && now_ms >= last_exit_ms_ && now_ms - last_exit_ms_ < min_reentry_ms_) {
            return;
        }

        if (allows_long() && latest_alpha_ >= entry_threshold_long_) {
            open_position('B', std::min(order_volume_, max_abs_pos_), "long_entry");
        } else if (allows_short() && latest_alpha_ <= entry_threshold_short_) {
            open_position('S', std::min(order_volume_, max_abs_pos_), "short_entry");
        } else if (debug_) {
            log_state("idle");
        }
    }

    void close_position(const PositionDetail& pos, int long_total, int short_total, const char* reason) {
        if (!has_tick_) return;

        if (long_total > 0) {
            int vol = std::min(order_volume_, long_total);
            char offset = pos.long_td > 0 ? 'T' : 'C';
            send_order('S', offset, vol, best_sell_price(last_tick_), reason);
            if (long_total - vol <= 0) {
                last_exit_ms_ = hhmmssmmm_to_ms(last_tick_.update_time);
            }
            return;
        }

        if (short_total > 0) {
            int vol = std::min(order_volume_, short_total);
            char offset = pos.short_td > 0 ? 'T' : 'C';
            send_order('B', offset, vol, best_buy_price(last_tick_), reason);
            if (short_total - vol <= 0) {
                last_exit_ms_ = hhmmssmmm_to_ms(last_tick_.update_time);
            }
        }
    }

    void open_position(char direction, int volume, const char* reason) {
        if (!has_tick_ || volume <= 0) return;
        double price = (direction == 'B') ? best_buy_price(last_tick_) : best_sell_price(last_tick_);
        send_order(direction, 'O', volume, price, reason);
        position_entry_ms_ = hhmmssmmm_to_ms(last_tick_.update_time);
    }

    void send_order(char direction, char offset_flag, int volume, double price, const char* reason) {
        if (price <= 0.0 || volume <= 0) return;

        OrderReq req{};
        req.symbol_id = symbol_id_;
        std::strncpy(req.account_id, default_account_.c_str(), sizeof(req.account_id) - 1);
        std::strncpy(req.symbol, symbol_.c_str(), sizeof(req.symbol) - 1);
        req.direction = direction;
        req.offset_flag = offset_flag;
        req.price = price;
        req.volume = volume;
        bus_->publish(EVENT_ORDER_REQ, &req);

        if (debug_) {
            std::cout << "[T0RbStrategy] reason=" << reason
                      << " alpha=" << latest_alpha_
                      << " symbol=" << symbol_
                      << " direction=" << direction
                      << " offset=" << offset_flag
                      << " volume=" << volume
                      << " price=" << price
                      << " ts=" << last_tick_.update_time << std::endl;
        }
    }

    void log_state(const char* reason) const {
        std::cout << "[T0RbStrategy] reason=" << reason
                  << " alpha=" << latest_alpha_
                  << " symbol=" << symbol_
                  << " ts=" << last_tick_.update_time << std::endl;
    }

    bool allows_long() const {
        return trade_side_ == "both" || trade_side_ == "long_only";
    }

    bool allows_short() const {
        return trade_side_ == "both" || trade_side_ == "short_only";
    }

private:
    EventBus* bus_ = nullptr;
    std::string symbol_ = "rb2605";
    uint64_t symbol_id_ = 0;
    std::string signal_name_ = "ALPHA_SCORE";
    std::string default_account_ = "SIM";
    std::string trade_side_ = "both";

    uint64_t decision_interval_ms_ = 500;
    uint64_t max_hold_ms_ = 5000;
    uint64_t min_reentry_ms_ = 1000;
    double entry_threshold_long_ = 0.8;
    double entry_threshold_short_ = -0.8;
    double exit_threshold_abs_ = 0.2;
    int max_abs_pos_ = 3;
    int order_volume_ = 1;
    bool debug_ = false;

    TickRecord last_tick_{};
    bool has_tick_ = false;
    double latest_alpha_ = 0.0;
    uint64_t latest_alpha_ts_ms_ = 0;
    uint64_t last_decision_bucket_ = std::numeric_limits<uint64_t>::max();
    uint64_t position_entry_ms_ = 0;
    uint64_t last_exit_ms_ = 0;
};

EXPORT_MODULE(T0RbStrategyModule)
