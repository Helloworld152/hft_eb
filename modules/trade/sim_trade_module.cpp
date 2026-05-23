#include "framework.h"
#include "core_state.h"
#include "symbol_manager.h"
#include "tick_matching_engine.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

class SimTradeModule : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config, ITimerService* timer_svc = nullptr) override {
        (void)timer_svc;
        bus_ = bus;

        if (config.count("debug")) debug_ = (config.at("debug") == "true");
        if (config.count("max_orders")) max_orders_ = std::stoul(config.at("max_orders"));
        if (config.count("initial_balance")) initial_balance_ = std::stod(config.at("initial_balance"));
        if (config.count("account_id")) account_id_ = config.at("account_id");
        if (config.count("broker_id")) broker_id_ = config.at("broker_id");
        balance_ = initial_balance_;
        available_ = initial_balance_;

        if (debug_) LOG_INFO("[SimTrade] max_orders={} balance={}", max_orders_, balance_);

        bus_->subscribe(EVENT_ORDER_SEND,
                        StaticDelegate<void(void*)>::bind<SimTradeModule, &SimTradeModule::on_order_send>(this));
        bus_->subscribe(EVENT_CANCEL_SEND,
                        StaticDelegate<void(void*)>::bind<SimTradeModule, &SimTradeModule::on_cancel_send>(this));
        bus_->subscribe(EVENT_MARKET_DATA,
                        StaticDelegate<void(void*)>::bind<SimTradeModule, &SimTradeModule::on_market_data>(this));
        bus_->subscribe(EVENT_QRY_ACC,
                        StaticDelegate<void(void*)>::bind<SimTradeModule, &SimTradeModule::on_query_account>(this));
        bus_->subscribe(EVENT_QRY_POS,
                        StaticDelegate<void(void*)>::bind<SimTradeModule, &SimTradeModule::on_query_position>(this));
    }

private:
    struct TrackedOrder {
        uint64_t order_sys_id = 0;   // 交易所单号 (1,2,3...)
        uint64_t client_id = 0;      // 策略发单 ID
        char account_id[16] = {};
        char symbol[32] = {};
        uint64_t symbol_id = 0;
        char direction = 0;
        char offset_flag = 0;
        double limit_price = 0.0;
        int volume_total = 0;
        int volume_traded = 0;
        bool is_market = false;
    };

    struct SimpleFill {
        double price = 0.0;
        int qty = 0;
    };

    // ======================================================================
    //  事件入口
    // ======================================================================

    void on_order_send(void* d)  { onOrder(static_cast<OrderReq*>(d)); }
    void on_cancel_send(void* d) { onCancel(static_cast<CancelReq*>(d)); }
    void on_market_data(void* d) { onTick(static_cast<TickRecord*>(d)); }
    void on_query_account(void*) { publishAccount(); }
    void on_query_position(void*) {}

    void onOrder(OrderReq* req) {
        if (!req || req->symbol[0] == '\0' || req->volume <= 0) return;
        if (req->direction != 'B' && req->direction != 'S') return;

        uint64_t sys_id = next_order_sys_id_++;
        TrackedOrder tracked{};
        tracked.order_sys_id = sys_id;
        tracked.client_id = req->client_id;
        std::strncpy(tracked.account_id, req->account_id, sizeof(tracked.account_id) - 1);
        std::strncpy(tracked.symbol, req->symbol, sizeof(tracked.symbol) - 1);
        tracked.symbol_id = req->symbol_id;
        tracked.direction = req->direction;
        tracked.offset_flag = req->offset_flag;
        tracked.limit_price = req->price;
        tracked.volume_total = req->volume;
        tracked.is_market = (req->price <= 0.0);

        tracked_orders_[sys_id] = tracked;
        client_to_sys_[req->client_id] = sys_id;
        pending_order_ids_.push_back(sys_id);

        // 保守回测：订单先进入 pending，不允许在产生信号的同一个 tick 成交。
        publish_order_rtn(tracked_orders_[sys_id], '3');
    }

    void onTick(TickRecord* tick) {
        if (!tick || tick->symbol[0] == '\0') return;

        int bid_remain[5] = {};
        int ask_remain[5] = {};
        for (int i = 0; i < 5; ++i) {
            bid_remain[i] = tick->bid_volume[i];
            ask_remain[i] = tick->ask_volume[i];
        }

        std::vector<uint64_t> still_working;
        still_working.reserve(working_order_ids_.size() + pending_order_ids_.size());

        for (uint64_t sys_id : working_order_ids_) {
            auto it = tracked_orders_.find(sys_id);
            if (it == tracked_orders_.end()) continue;

            TrackedOrder& order = it->second;
            if (std::strncmp(order.symbol, tick->symbol, sizeof(order.symbol)) != 0) {
                still_working.push_back(sys_id);
                continue;
            }

            SimpleFill fill{};
            if (try_simple_fill(order, *tick, bid_remain, ask_remain, fill)) {
                TickMatchingEngine::Trade trade{};
                trade.taker_id = 0;            // 0 表示外部 tick 流动性
                trade.maker_id = order.order_sys_id;
                trade.price = fill.price;
                trade.qty = fill.qty;
                process_trade(trade, 0);
            }

            if (tracked_orders_.find(sys_id) != tracked_orders_.end()) {
                still_working.push_back(sys_id);
            }
        }

        // 当前 tick 撮合结束后，再让本 tick 期间产生的新订单进入 working。
        // 因此策略基于 tick[i] 发出的订单最早只能在 tick[i+1] 成交。
        still_working.insert(still_working.end(), pending_order_ids_.begin(), pending_order_ids_.end());
        pending_order_ids_.clear();
        working_order_ids_.swap(still_working);
    }

    void onCancel(CancelReq* req) {
        if (!req || req->client_id == 0) return;

        auto cit = client_to_sys_.find(req->client_id);
        if (cit == client_to_sys_.end()) return;
        uint64_t sys_id = cit->second;

        auto it = tracked_orders_.find(sys_id);
        if (it == tracked_orders_.end()) return;

        erase_order_id(pending_order_ids_, sys_id);
        erase_order_id(working_order_ids_, sys_id);

        publish_order_rtn(it->second, '5', "已撤单");
        client_to_sys_.erase(req->client_id);
        tracked_orders_.erase(it);
    }

    bool try_simple_fill(const TrackedOrder& order, const TickRecord& tick,
                         int* bid_remain, int* ask_remain, SimpleFill& fill) {
        const int remain = order.volume_total - order.volume_traded;
        if (remain <= 0) return false;

        if (order.direction == 'B') {
            for (int i = 0; i < 5; ++i) {
                const double ask_px = tick.ask_price[i];
                int& ask_qty = ask_remain[i];
                if (ask_px <= 0.0 || ask_qty <= 0) continue;
                if (!order.is_market && order.limit_price < ask_px) break;

                fill.price = ask_px;
                fill.qty = std::min(remain, ask_qty);
                ask_qty -= fill.qty;
                return fill.qty > 0;
            }
            return false;
        }

        if (order.direction == 'S') {
            for (int i = 0; i < 5; ++i) {
                const double bid_px = tick.bid_price[i];
                int& bid_qty = bid_remain[i];
                if (bid_px <= 0.0 || bid_qty <= 0) continue;
                if (!order.is_market && order.limit_price > bid_px) break;

                fill.price = bid_px;
                fill.qty = std::min(remain, bid_qty);
                bid_qty -= fill.qty;
                return fill.qty > 0;
            }
        }
        return false;
    }

    static void erase_order_id(std::vector<uint64_t>& ids, uint64_t id) {
        ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
    }

    int process_trade(const TickMatchingEngine::Trade& trade, uint64_t taker_sys_id) {
        int related_qty = 0;
        auto process_side = [&](uint64_t sid) {
            if (sid == 0) return;
            auto it = tracked_orders_.find(sid);
            if (it == tracked_orders_.end()) return;
            auto& o = it->second;
            o.volume_traded += trade.qty;
            publish_trade_rtn(o, trade.price, trade.qty);

            if (o.volume_traded >= o.volume_total) {
                publish_order_rtn(o, '0');
                client_to_sys_.erase(o.client_id);
                tracked_orders_.erase(it);
            } else {
                publish_order_rtn(o, '1');
            }
            if (sid == taker_sys_id) related_qty += trade.qty;
        };
        process_side(trade.maker_id);
        process_side(trade.taker_id);
        return related_qty;
    }

    // ======================================================================
    //  回报发布
    // ======================================================================

    void publish_order_rtn(const TrackedOrder& order, char status, const char* msg = nullptr) {
        OrderRtn rtn{};
        rtn.client_id = order.client_id;
        std::strncpy(rtn.account_id, order.account_id, sizeof(rtn.account_id) - 1);
        std::strncpy(rtn.exchange_id, "SIM", sizeof(rtn.exchange_id) - 1);
        std::strncpy(rtn.symbol, order.symbol, sizeof(rtn.symbol) - 1);
        rtn.symbol_id = order.symbol_id;
        rtn.direction = order.direction;
        rtn.offset_flag = order.offset_flag;
        rtn.limit_price = order.limit_price;
        rtn.volume_total = order.volume_total;
        rtn.volume_traded = order.volume_traded;
        rtn.status = status;
        format_sys_id_str(order.order_sys_id, rtn.order_sys_id, sizeof(rtn.order_sys_id));
        if (msg) std::strncpy(rtn.status_msg, msg, sizeof(rtn.status_msg) - 1);
        else {
            const char* d = (status == '0') ? "全部成交" : (status == '1') ? "部分成交"
                          : (status == '3') ? "已报" : (status == '5') ? "已撤单" : "";
            std::strncpy(rtn.status_msg, d, sizeof(rtn.status_msg) - 1);
        }

        const auto& core = core::CoreServicesRegistry::get();
        if (core.order_service) core.order_service->enqueue_order_rtn(rtn);
        bus_->publish(EVENT_RTN_ORDER, &rtn);
    }

    void publish_trade_rtn(const TrackedOrder& order, double price, int qty) {
        TradeRtn rtn{};
        rtn.client_id = order.client_id;
        std::strncpy(rtn.account_id, order.account_id, sizeof(rtn.account_id) - 1);
        std::strncpy(rtn.exchange_id, "SIM", sizeof(rtn.exchange_id) - 1);
        std::strncpy(rtn.symbol, order.symbol, sizeof(rtn.symbol) - 1);
        rtn.symbol_id = order.symbol_id;
        rtn.direction = order.direction;
        rtn.offset_flag = order.offset_flag;
        rtn.price = price;
        rtn.volume = qty;
        format_sys_id_str(order.order_sys_id, rtn.order_sys_id, sizeof(rtn.order_sys_id));
        format_trade_id(rtn.trade_id, sizeof(rtn.trade_id));

        const auto& core = core::CoreServicesRegistry::get();
        if (core.order_service) core.order_service->enqueue_trade_rtn(rtn);
        if (core.position_service) core.position_service->enqueue_trade(rtn);
        bus_->publish(EVENT_RTN_TRADE, &rtn);

        onTradeFill(order, price, qty);
    }

    static void format_sys_id_str(uint64_t id, char* buf, size_t len) {
        std::snprintf(buf, len, "SIM%llu", static_cast<unsigned long long>(id));
    }

    // ======================================================================
    //  模拟账户
    // ======================================================================

    void onTradeFill(const TrackedOrder& order, double fill_price, int qty) {
        double multiplier = SymbolManager::instance().get_multiplier(order.symbol);
        if (order.offset_flag == 'O') {
            cost_map_[order.symbol_id].push_back(fill_price);
        } else {
            auto it = cost_map_.find(order.symbol_id);
            if (it != cost_map_.end() && !it->second.empty()) {
                double open_price = it->second.front();
                it->second.pop_front();
                double pnl = (order.direction == 'S')
                    ? (fill_price - open_price) * qty * multiplier
                    : (open_price - fill_price) * qty * multiplier;
                balance_ += pnl;
            }
        }
        available_ = balance_;
        publishAccount();
    }

    void publishAccount() {
        AccountDetail acc{};
        std::strncpy(acc.broker_id, broker_id_.c_str(), sizeof(acc.broker_id) - 1);
        std::strncpy(acc.account_id, account_id_.c_str(), sizeof(acc.account_id) - 1);
        acc.balance = balance_;
        acc.available = available_;
        const auto& core = core::CoreServicesRegistry::get();
        if (core.account_service) core.account_service->enqueue_account(acc);
        bus_->publish(EVENT_ACC_UPDATE, &acc);
    }

    void format_trade_id(char* buf, size_t len) {
        uint64_t id = next_trade_id_.fetch_add(1);
        std::snprintf(buf, len, "T%llu", static_cast<unsigned long long>(id));
    }

    // ======================================================================
    //  成员变量
    // ======================================================================

    EventBus* bus_ = nullptr;
    bool debug_ = false;
    size_t max_orders_ = 10000;

    std::vector<uint64_t> pending_order_ids_;
    std::vector<uint64_t> working_order_ids_;
    FastHashMap<uint64_t, TrackedOrder> tracked_orders_;    // order_sys_id → order
    FastHashMap<uint64_t, uint64_t> client_to_sys_;          // client_id → order_sys_id

    double initial_balance_ = 1000000.0;
    std::string account_id_ = "SIM";
    std::string broker_id_ = "SIM";
    double balance_ = 0.0;
    double available_ = 0.0;
    FastHashMap<uint64_t, std::deque<double>> cost_map_;

    std::atomic<uint64_t> next_order_sys_id_{1};
    std::atomic<uint64_t> next_trade_id_{1};
};

EXPORT_MODULE(SimTradeModule)