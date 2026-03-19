#include "../../include/framework.h"
#include "../../core/include/protocol.h"
#include <nlohmann/json.hpp>
#include <zmq.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

namespace {

struct ExpectedEvent {
    EventType type;
    json match;
    bool matched = false;
    json sample;
    uint64_t elapsed_ms = 0;
};

struct PendingRequest {
    std::string req_id;
    std::vector<ExpectedEvent> expects;
    std::chrono::steady_clock::time_point start;
    int timeout_ms = 1000;
    bool done = false;
};

bool json_subset_match(const json& expect, const json& actual) {
    if (expect.is_object()) {
        if (!actual.is_object()) return false;
        for (auto it = expect.begin(); it != expect.end(); ++it) {
            const auto& key = it.key();
            if (!actual.contains(key)) return false;
            if (!json_subset_match(it.value(), actual.at(key))) return false;
        }
        return true;
    }

    if (expect.is_array()) {
        if (!actual.is_array()) return false;
        if (expect.size() != actual.size()) return false;
        for (size_t i = 0; i < expect.size(); ++i) {
            if (!json_subset_match(expect[i], actual[i])) return false;
        }
        return true;
    }

    return expect == actual;
}

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

void fill_order_req_from_json(const json& payload, OrderReq* req) {
    std::memset(req, 0, sizeof(*req));
    req->client_id = payload.value("client_id", 0);
    std::string order_ref = payload.value("order_ref", "");
    std::string account_id = payload.value("account_id", "");
    std::string symbol = payload.value("symbol", "");
    std::string direction = payload.value("direction", "B");
    std::string offset = payload.value("offset", "O");
    std::strncpy(req->order_ref, order_ref.c_str(), sizeof(req->order_ref) - 1);
    std::strncpy(req->account_id, account_id.c_str(), sizeof(req->account_id) - 1);
    std::strncpy(req->symbol, symbol.c_str(), sizeof(req->symbol) - 1);
    req->symbol_id = payload.value("symbol_id", 0);
    req->direction = direction.empty() ? 'B' : direction[0];
    req->offset_flag = offset.empty() ? 'O' : offset[0];
    req->price = payload.value("price", 0.0);
    req->volume = payload.value("volume", 1);
}

void fill_cancel_req_from_json(const json& payload, CancelReq* req) {
    std::memset(req, 0, sizeof(*req));
    req->client_id = payload.value("client_id", 0);
    std::string account_id = payload.value("account_id", "");
    std::string symbol = payload.value("symbol", "");
    std::string order_ref = payload.value("order_ref", "");
    std::string order_sys_id = payload.value("order_sys_id", "");
    std::strncpy(req->account_id, account_id.c_str(), sizeof(req->account_id) - 1);
    std::strncpy(req->symbol, symbol.c_str(), sizeof(req->symbol) - 1);
    std::strncpy(req->order_ref, order_ref.c_str(), sizeof(req->order_ref) - 1);
    std::strncpy(req->order_sys_id, order_sys_id.c_str(), sizeof(req->order_sys_id) - 1);
}

void fill_tick_from_json(const json& payload, TickRecord* t) {
    std::memset(t, 0, sizeof(*t));
    std::string symbol = payload.value("symbol", "");
    std::strncpy(t->symbol, symbol.c_str(), sizeof(t->symbol) - 1);
    t->symbol_id = payload.value("symbol_id", 0);
    t->trading_day = payload.value("trading_day", 0);
    t->update_time = payload.value("update_time", 0);
    t->last_price = payload.value("last_price", 0.0);
    t->volume = payload.value("volume", 0);
    t->turnover = payload.value("turnover", 0.0);
    t->open_interest = payload.value("open_interest", 0.0);
    t->upper_limit = payload.value("upper_limit", 0.0);
    t->lower_limit = payload.value("lower_limit", 0.0);
    t->open_price = payload.value("open_price", 0.0);
    t->highest_price = payload.value("highest_price", 0.0);
    t->lowest_price = payload.value("lowest_price", 0.0);
    t->pre_close_price = payload.value("pre_close_price", 0.0);
    if (payload.contains("bid_price") && payload["bid_price"].is_array()) {
        for (size_t i = 0; i < 5 && i < payload["bid_price"].size(); ++i) {
            t->bid_price[i] = payload["bid_price"][i].get<double>();
        }
    }
    if (payload.contains("bid_volume") && payload["bid_volume"].is_array()) {
        for (size_t i = 0; i < 5 && i < payload["bid_volume"].size(); ++i) {
            t->bid_volume[i] = payload["bid_volume"][i].get<int>();
        }
    }
    if (payload.contains("ask_price") && payload["ask_price"].is_array()) {
        for (size_t i = 0; i < 5 && i < payload["ask_price"].size(); ++i) {
            t->ask_price[i] = payload["ask_price"][i].get<double>();
        }
    }
    if (payload.contains("ask_volume") && payload["ask_volume"].is_array()) {
        for (size_t i = 0; i < 5 && i < payload["ask_volume"].size(); ++i) {
            t->ask_volume[i] = payload["ask_volume"][i].get<int>();
        }
    }
}

void fill_kline_from_json(const json& payload, KlineRecord* k) {
    std::memset(k, 0, sizeof(*k));
    std::string symbol = payload.value("symbol", "");
    std::strncpy(k->symbol, symbol.c_str(), sizeof(k->symbol) - 1);
    k->symbol_id = payload.value("symbol_id", 0);
    k->trading_day = payload.value("trading_day", 0);
    k->start_time = payload.value("start_time", 0);
    k->open = payload.value("open", 0.0);
    k->high = payload.value("high", 0.0);
    k->low = payload.value("low", 0.0);
    k->close = payload.value("close", 0.0);
    k->volume = payload.value("volume", 0);
    k->turnover = payload.value("turnover", 0.0);
    k->open_interest = payload.value("open_interest", 0.0);
    k->interval = static_cast<KlineInterval>(payload.value("interval", static_cast<int>(K_1M)));
}

void fill_signal_from_json(const json& payload, SignalRecord* s) {
    std::memset(s, 0, sizeof(*s));
    std::string source_id = payload.value("source_id", "");
    std::string symbol = payload.value("symbol", "");
    std::string factor_name = payload.value("factor_name", "");
    std::strncpy(s->source_id, source_id.c_str(), sizeof(s->source_id) - 1);
    std::strncpy(s->symbol, symbol.c_str(), sizeof(s->symbol) - 1);
    std::strncpy(s->factor_name, factor_name.c_str(), sizeof(s->factor_name) - 1);
    s->value = payload.value("value", 0.0);
    s->timestamp = payload.value("timestamp", 0);
}

}

class TestHarnessModule : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config, ITimerService* timer_svc = nullptr) override {
        (void)timer_svc;
        bus_ = bus;
        if (config.count("control_addr")) control_addr_ = config.at("control_addr");
        if (config.count("default_timeout_ms")) default_timeout_ms_ = std::stoi(config.at("default_timeout_ms"));
        if (config.count("debug")) {
            std::string val = config.at("debug");
            debug_ = (val == "true" || val == "1");
        }

        event_map_ = build_event_map();
        event_name_map_ = build_event_name_map();

        subscribe_all();

        std::cout << "[TestHarness] 初始化. 控制地址: " << control_addr_
                  << ", 默认超时: " << default_timeout_ms_ << "ms" << std::endl;
    }

    void start() override {
        running_ = true;
        worker_ = std::thread(&TestHarnessModule::control_loop, this);
    }

    void stop() override {
        running_ = false;
        if (worker_.joinable()) worker_.join();
    }

private:
    void subscribe_all() {
        auto handler = [this](EventType type, void* data) {
            json payload = event_to_json(type, data);
            if (payload.is_null() || payload.empty()) return;

            std::unique_lock<std::mutex> lock(mtx_);
            for (auto& pending : pending_) {
                for (auto& exp : pending->expects) {
                    if (exp.matched || exp.type != type) continue;
                    if (json_subset_match(exp.match, payload)) {
                        exp.matched = true;
                        exp.sample = payload;
                        auto now = std::chrono::steady_clock::now();
                        exp.elapsed_ms = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(now - pending->start).count());
                        cv_.notify_all();
                    }
                }
            }
        };

        for (int t = 0; t < MAX_EVENTS; ++t) {
            bus_->subscribe(static_cast<EventType>(t), [handler, t](void* d) { handler(static_cast<EventType>(t), d); });
        }
    }

    void control_loop() {
        void* context = zmq_ctx_new();
        void* rep = zmq_socket(context, ZMQ_REP);
        int timeout = 200;
        zmq_setsockopt(rep, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
        zmq_bind(rep, control_addr_.c_str());

        while (running_) {
            char buffer[8192];
            int recv_size = zmq_recv(rep, buffer, sizeof(buffer) - 1, 0);
            if (recv_size < 0) continue;
            buffer[recv_size] = '\0';

            std::string req_str(buffer, recv_size);
            json resp = handle_request(req_str);
            std::string resp_str = resp.dump();
            zmq_send(rep, resp_str.c_str(), resp_str.size(), 0);
        }

        zmq_close(rep);
        zmq_ctx_destroy(context);
    }

    json handle_request(const std::string& req_str) {
        json resp;
        resp["ok"] = false;
        resp["error"] = "";
        resp["results"] = json::array();

        try {
            json req = json::parse(req_str);
            std::string action = req.value("action", "");
            std::string req_id = req.value("req_id", "");
            resp["req_id"] = req_id;

            if (action == "ping") {
                resp["ok"] = true;
                return resp;
            }

            if (action == "publish" || action == "publish_and_wait") {
                std::string event_name = req.value("event", "");
                if (!event_map_.count(event_name)) {
                    resp["error"] = "unknown event";
                    return resp;
                }

                EventType evt = event_map_[event_name];
                json payload = req.value("payload", json::object());

                PendingRequest* pending = nullptr;
                std::shared_ptr<PendingRequest> holder;

                if (action == "publish_and_wait") {
                    holder = std::make_shared<PendingRequest>();
                    holder->req_id = req_id;
                    holder->start = std::chrono::steady_clock::now();
                    holder->timeout_ms = req.value("timeout_ms", default_timeout_ms_);

                    if (req.contains("expect") && req["expect"].is_array()) {
                        for (const auto& item : req["expect"]) {
                            std::string exp_event = item.value("event", "");
                            if (!event_map_.count(exp_event)) continue;
                            ExpectedEvent exp;
                            exp.type = event_map_[exp_event];
                            exp.match = item.value("match", json::object());
                            holder->expects.push_back(std::move(exp));
                        }
                    }

                    {
                        std::lock_guard<std::mutex> lock(mtx_);
                        pending_.push_back(holder);
                    }
                    pending = holder.get();
                }

                publish_event(evt, payload);

                if (action == "publish") {
                    resp["ok"] = true;
                    return resp;
                }

                wait_for_expectations(pending, resp);

                if (pending) {
                    std::lock_guard<std::mutex> lock(mtx_);
                    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                        [pending](const std::shared_ptr<PendingRequest>& p) { return p.get() == pending; }),
                        pending_.end());
                }

                return resp;
            }

            resp["error"] = "unknown action";
            return resp;
        } catch (const std::exception& e) {
            resp["error"] = e.what();
            return resp;
        }
    }

    void publish_event(EventType evt, const json& payload) {
        if (!bus_) return;
        switch (evt) {
            case EVENT_MARKET_DATA: {
                TickRecord t;
                fill_tick_from_json(payload, &t);
                bus_->publish(evt, &t);
                break;
            }
            case EVENT_ORDER_REQ:
            case EVENT_ORDER_SEND: {
                OrderReq r;
                fill_order_req_from_json(payload, &r);
                bus_->publish(evt, &r);
                break;
            }
            case EVENT_CANCEL_REQ:
            case EVENT_CANCEL_SEND: {
                CancelReq r;
                fill_cancel_req_from_json(payload, &r);
                bus_->publish(evt, &r);
                break;
            }
            case EVENT_SIGNAL: {
                SignalRecord s;
                fill_signal_from_json(payload, &s);
                bus_->publish(evt, &s);
                break;
            }
            case EVENT_KLINE: {
                KlineRecord k;
                fill_kline_from_json(payload, &k);
                bus_->publish(evt, &k);
                break;
            }
            default:
                if (debug_) {
                    std::cout << "[TestHarness] Unsupported publish event: " << static_cast<int>(evt) << std::endl;
                }
                break;
        }
    }

    void wait_for_expectations(PendingRequest* pending, json& resp) {
        if (!pending) {
            resp["error"] = "no pending request";
            return;
        }

        auto deadline = pending->start + std::chrono::milliseconds(pending->timeout_ms);

        std::unique_lock<std::mutex> lock(mtx_);
        while (running_ && std::chrono::steady_clock::now() < deadline) {
            bool all_matched = true;
            for (const auto& exp : pending->expects) {
                if (!exp.matched) { all_matched = false; break; }
            }
            if (all_matched) break;
            cv_.wait_until(lock, deadline);
        }

        resp["ok"] = true;
        for (const auto& exp : pending->expects) {
            json r;
            r["event"] = event_name_map_[exp.type];
            r["matched"] = exp.matched;
            r["elapsed_ms"] = exp.elapsed_ms;
            if (exp.matched) r["sample"] = exp.sample;
            resp["results"].push_back(r);
            if (!exp.matched) resp["ok"] = false;
        }
    }

    EventBus* bus_ = nullptr;
    std::string control_addr_ = "tcp://*:5560";
    int default_timeout_ms_ = 1000;
    bool debug_ = false;

    std::unordered_map<std::string, EventType> event_map_;
    std::unordered_map<EventType, std::string> event_name_map_;

    std::atomic<bool> running_{false};
    std::thread worker_;

    std::mutex mtx_;
    std::condition_variable cv_;
    std::vector<std::shared_ptr<PendingRequest>> pending_;
};

EXPORT_MODULE(TestHarnessModule)
