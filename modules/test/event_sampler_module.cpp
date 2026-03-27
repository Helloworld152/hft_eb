#include "../../include/framework.h"
#include "../../core/include/protocol.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using json = nlohmann::json;

namespace {

std::unordered_map<std::string, EventType> build_event_map() {
    return {
        {"EVENT_MARKET_DATA", EVENT_MARKET_DATA},
        {"EVENT_ORDER_REQ", EVENT_ORDER_REQ},
        {"EVENT_ORDER_SEND", EVENT_ORDER_SEND},
        {"EVENT_RTN_ORDER", EVENT_RTN_ORDER},
        {"EVENT_RTN_TRADE", EVENT_RTN_TRADE},
        {"EVENT_RTN_RAW_ORDER", EVENT_RTN_RAW_ORDER},
        {"EVENT_RTN_RAW_TRADE", EVENT_RTN_RAW_TRADE},
        {"EVENT_POS_UPDATE", EVENT_POS_UPDATE},
        {"EVENT_RSP_POS", EVENT_RSP_POS},
        {"EVENT_KLINE", EVENT_KLINE},
        {"EVENT_SIGNAL", EVENT_SIGNAL},
        {"EVENT_QRY_POS", EVENT_QRY_POS},
        {"EVENT_QRY_ACC", EVENT_QRY_ACC},
        {"EVENT_CANCEL_REQ", EVENT_CANCEL_REQ},
        {"EVENT_CANCEL_SEND", EVENT_CANCEL_SEND},
        {"EVENT_ACC_UPDATE", EVENT_ACC_UPDATE},
        {"EVENT_CONN_STATUS", EVENT_CONN_STATUS},
        {"EVENT_LOG", EVENT_LOG},
        {"EVENT_CACHE_RESET", EVENT_CACHE_RESET}
    };
}

std::unordered_map<EventType, std::string> build_event_name_map() {
    return {
        {EVENT_MARKET_DATA, "EVENT_MARKET_DATA"},
        {EVENT_ORDER_REQ, "EVENT_ORDER_REQ"},
        {EVENT_ORDER_SEND, "EVENT_ORDER_SEND"},
        {EVENT_RTN_ORDER, "EVENT_RTN_ORDER"},
        {EVENT_RTN_TRADE, "EVENT_RTN_TRADE"},
        {EVENT_RTN_RAW_ORDER, "EVENT_RTN_RAW_ORDER"},
        {EVENT_RTN_RAW_TRADE, "EVENT_RTN_RAW_TRADE"},
        {EVENT_POS_UPDATE, "EVENT_POS_UPDATE"},
        {EVENT_RSP_POS, "EVENT_RSP_POS"},
        {EVENT_KLINE, "EVENT_KLINE"},
        {EVENT_SIGNAL, "EVENT_SIGNAL"},
        {EVENT_QRY_POS, "EVENT_QRY_POS"},
        {EVENT_QRY_ACC, "EVENT_QRY_ACC"},
        {EVENT_CANCEL_REQ, "EVENT_CANCEL_REQ"},
        {EVENT_CANCEL_SEND, "EVENT_CANCEL_SEND"},
        {EVENT_ACC_UPDATE, "EVENT_ACC_UPDATE"},
        {EVENT_CONN_STATUS, "EVENT_CONN_STATUS"},
        {EVENT_LOG, "EVENT_LOG"},
        {EVENT_CACHE_RESET, "EVENT_CACHE_RESET"}
    };
}

std::string trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) start++;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) end--;
    return s.substr(start, end - start);
}

std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        if (ch == ',') {
            out.push_back(trim(cur));
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    if (!cur.empty() || !out.empty()) out.push_back(trim(cur));
    return out;
}

json tick_to_json(const TickRecord& t) {
    json j;
    j["symbol"] = t.symbol;
    j["symbol_id"] = t.symbol_id;
    j["trading_day"] = t.trading_day;
    j["update_time"] = t.update_time;
    j["last_price"] = t.last_price;
    j["volume"] = t.volume;
    j["turnover"] = t.turnover;
    j["open_interest"] = t.open_interest;
    j["upper_limit"] = t.upper_limit;
    j["lower_limit"] = t.lower_limit;
    j["open_price"] = t.open_price;
    j["highest_price"] = t.highest_price;
    j["lowest_price"] = t.lowest_price;
    j["pre_close_price"] = t.pre_close_price;
    j["bid_price"] = {t.bid_price[0], t.bid_price[1], t.bid_price[2], t.bid_price[3], t.bid_price[4]};
    j["bid_volume"] = {t.bid_volume[0], t.bid_volume[1], t.bid_volume[2], t.bid_volume[3], t.bid_volume[4]};
    j["ask_price"] = {t.ask_price[0], t.ask_price[1], t.ask_price[2], t.ask_price[3], t.ask_price[4]};
    j["ask_volume"] = {t.ask_volume[0], t.ask_volume[1], t.ask_volume[2], t.ask_volume[3], t.ask_volume[4]};
    return j;
}

json kline_to_json(const KlineRecord& k) {
    json j;
    j["symbol"] = k.symbol;
    j["symbol_id"] = k.symbol_id;
    j["trading_day"] = k.trading_day;
    j["start_time"] = k.start_time;
    j["open"] = k.open;
    j["high"] = k.high;
    j["low"] = k.low;
    j["close"] = k.close;
    j["volume"] = k.volume;
    j["turnover"] = k.turnover;
    j["open_interest"] = k.open_interest;
    j["interval"] = static_cast<int>(k.interval);
    return j;
}

json order_req_to_json(const OrderReq& r) {
    json j;
    j["client_id"] = r.client_id;
    j["order_ref"] = r.order_ref;
    j["account_id"] = r.account_id;
    j["symbol"] = r.symbol;
    j["symbol_id"] = r.symbol_id;
    j["direction"] = std::string(1, r.direction);
    j["offset"] = std::string(1, r.offset_flag);
    j["price"] = r.price;
    j["volume"] = r.volume;
    return j;
}

json cancel_req_to_json(const CancelReq& r) {
    json j;
    j["client_id"] = r.client_id;
    j["account_id"] = r.account_id;
    j["symbol"] = r.symbol;
    j["order_ref"] = r.order_ref;
    j["order_sys_id"] = r.order_sys_id;
    return j;
}

json order_rtn_to_json(const OrderRtn& r) {
    json j;
    j["client_id"] = r.client_id;
    j["account_id"] = r.account_id;
    j["order_ref"] = r.order_ref;
    j["order_sys_id"] = r.order_sys_id;
    j["exchange_id"] = r.exchange_id;
    j["symbol"] = r.symbol;
    j["symbol_id"] = r.symbol_id;
    j["direction"] = std::string(1, r.direction);
    j["offset"] = std::string(1, r.offset_flag);
    j["price"] = r.limit_price;
    j["vol_total"] = r.volume_total;
    j["vol_traded"] = r.volume_traded;
    j["status"] = std::string(1, r.status);
    j["status_msg"] = r.status_msg;
    return j;
}

json trade_rtn_to_json(const TradeRtn& r) {
    json j;
    j["client_id"] = r.client_id;
    j["account_id"] = r.account_id;
    j["exchange_id"] = r.exchange_id;
    j["symbol"] = r.symbol;
    j["symbol_id"] = r.symbol_id;
    j["direction"] = std::string(1, r.direction);
    j["offset"] = std::string(1, r.offset_flag);
    j["price"] = r.price;
    j["volume"] = r.volume;
    j["trade_id"] = r.trade_id;
    j["order_ref"] = r.order_ref;
    j["order_sys_id"] = r.order_sys_id;
    return j;
}

json account_to_json(const AccountDetail& a) {
    json j;
    j["broker_id"] = a.broker_id;
    j["account_id"] = a.account_id;
    j["balance"] = a.balance;
    j["available"] = a.available;
    j["margin"] = a.margin;
    j["close_pnl"] = a.close_pnl;
    j["position_pnl"] = a.position_pnl;
    return j;
}

json position_to_json(const PositionDetail& p) {
    json j;
    j["account_id"] = p.account_id;
    j["symbol"] = p.symbol;
    j["exchange_id"] = p.exchange_id;
    j["symbol_id"] = p.symbol_id;
    j["direction"] = std::string(1, p.direction);
    j["position_date"] = std::string(1, p.position_date);
    j["long_td"] = p.long_td;
    j["long_yd"] = p.long_yd;
    j["long_avg_price"] = p.long_avg_price;
    j["long_pnl"] = p.long_pnl;
    j["short_td"] = p.short_td;
    j["short_yd"] = p.short_yd;
    j["short_avg_price"] = p.short_avg_price;
    j["short_pnl"] = p.short_pnl;
    j["net_pnl"] = p.net_pnl;
    return j;
}

json signal_to_json(const SignalRecord& s) {
    json j;
    j["source_id"] = s.source_id;
    j["symbol"] = s.symbol;
    j["factor_name"] = s.factor_name;
    j["value"] = s.value;
    j["timestamp"] = s.timestamp;
    return j;
}

json conn_to_json(const ConnectionStatus& c) {
    json j;
    j["account_id"] = c.account_id;
    j["source"] = c.source;
    j["code"] = std::string(1, c.status);
    j["msg"] = c.msg;
    return j;
}

json cache_reset_to_json(const CacheReset& c) {
    json j;
    j["account_id"] = c.account_id;
    j["trading_day"] = c.trading_day;
    j["reset_type"] = c.reset_type;
    j["reason"] = c.reason;
    return j;
}

json event_to_json(EventType type, void* data) {
    switch (type) {
        case EVENT_MARKET_DATA:
            return tick_to_json(*static_cast<TickRecord*>(data));
        case EVENT_KLINE:
            return kline_to_json(*static_cast<KlineRecord*>(data));
        case EVENT_ORDER_REQ:
        case EVENT_ORDER_SEND:
            return order_req_to_json(*static_cast<OrderReq*>(data));
        case EVENT_CANCEL_REQ:
        case EVENT_CANCEL_SEND:
            return cancel_req_to_json(*static_cast<CancelReq*>(data));
        case EVENT_RTN_ORDER:
        case EVENT_RTN_RAW_ORDER:
            return order_rtn_to_json(*static_cast<OrderRtn*>(data));
        case EVENT_RTN_TRADE:
        case EVENT_RTN_RAW_TRADE:
            return trade_rtn_to_json(*static_cast<TradeRtn*>(data));
        case EVENT_ACC_UPDATE:
            return account_to_json(*static_cast<AccountDetail*>(data));
        case EVENT_POS_UPDATE:
        case EVENT_RSP_POS:
            return position_to_json(*static_cast<PositionDetail*>(data));
        case EVENT_SIGNAL:
            return signal_to_json(*static_cast<SignalRecord*>(data));
        case EVENT_CONN_STATUS:
            return conn_to_json(*static_cast<ConnectionStatus*>(data));
        case EVENT_CACHE_RESET:
            return cache_reset_to_json(*static_cast<CacheReset*>(data));
        default:
            return json::object();
    }
}

} // namespace

class EventSamplerModule : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config, ITimerService* timer_svc = nullptr) override {
        bus_ = bus;
        (void)timer_svc;

        if (config.find("events") != config.end()) {
            events_raw_ = config.at("events");
        }
        if (config.find("head_n") != config.end()) {
            head_n_ = std::stoull(config.at("head_n"));
        }
        if (config.find("sample_every") != config.end()) {
            sample_every_ = std::stoull(config.at("sample_every"));
        }
        if (config.find("max_print") != config.end()) {
            max_print_ = std::stoull(config.at("max_print"));
        }

        subscribe_events();

        if (event_types_.empty()) {
            std::cerr << "[EventSampler] No events subscribed. Set config 'events'.\n";
        } else {
            std::cout << "[EventSampler] Subscribed events: " << events_raw_ << "\n";
        }
    }

private:
    void subscribe_events() {
        auto event_map = build_event_map();
        auto names = split_csv(events_raw_);

        std::unordered_set<EventType> dedup;
        for (const auto& name : names) {
            if (name.empty()) continue;
            auto it = event_map.find(name);
            if (it == event_map.end()) {
                std::cerr << "[EventSampler] Unknown event: " << name << "\n";
                continue;
            }
            if (dedup.insert(it->second).second) {
                event_types_.push_back(it->second);
            }
        }

        for (auto type : event_types_) {
            bus_->subscribe(type, [this, type](void* data) {
                on_event(type, data);
            });
        }
    }

    void on_event(EventType type, void* data) {
        uint64_t& count = counters_[type];
        bool should_print = false;
        if (count < head_n_) {
            should_print = true;
        } else if (sample_every_ > 0 && (count % sample_every_ == 0)) {
            should_print = true;
        }

        if (should_print) {
            if (max_print_ > 0 && printed_ >= max_print_) {
                ++count;
                return;
            }
            json payload = event_to_json(type, data);
            payload["event_type"] = event_name_map_[type];
            payload["seq"] = count;
            std::cout << payload.dump() << "\n";
            ++printed_;
        }

        ++count;
    }

    EventBus* bus_ = nullptr;
    std::string events_raw_;
    uint64_t head_n_ = 5;
    uint64_t sample_every_ = 1000;
    uint64_t max_print_ = 0;
    uint64_t printed_ = 0;

    std::vector<EventType> event_types_;
    std::unordered_map<EventType, uint64_t> counters_;
    std::unordered_map<EventType, std::string> event_name_map_ = build_event_name_map();
};

EXPORT_MODULE(EventSamplerModule)
