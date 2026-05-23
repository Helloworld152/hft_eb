#include "framework.h"
#include "core_state.h"
#include "symbol_manager.h"
#include "tick_matching_engine.h"

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

    TickMatchingEngine& get_engine(const std::string& symbol) {
        auto it = engines_.find(symbol);
        if (it != engines_.end()) return *it->second;
        auto eng = std::make_unique<TickMatchingEngine>(max_orders_);
        TickMatchingEngine* ptr = eng.get();
        engines_[symbol] = std::move(eng);
        return *ptr;
    }

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

        std::string sym(req->symbol);
        auto& engine = get_engine(sym);
        const auto side = (req->direction == 'B') ? TickMatchingEngine::BUY
                                                   : TickMatchingEngine::SELL;
        const bool is_market = (req->price <= 0.0);

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
        tracked.is_market = is_market;

        tracked_orders_[sys_id] = tracked;
        client_to_sys_[req->client_id] = sys_id;

        TickMatchingEngine::Output output;
        bool ok = engine.submit(sys_id, side, req->price, req->volume, is_market, output);

        if (!ok) {
            publish_order_rtn(tracked_orders_[sys_id], '5', is_market ? "无对手方" : "引擎拒绝");
            client_to_sys_.erase(req->client_id);
            tracked_orders_.erase(sys_id);
            return;
        }

        int taker_filled = 0;
        for (const auto& trade : output.trades)
            taker_filled += process_trade(trade, sys_id);

        tracked_orders_[sys_id].volume_traded = taker_filled;

        if (output.resting) {
            publish_order_rtn(tracked_orders_[sys_id], taker_filled > 0 ? '1' : '3');
        } else {
            publish_order_rtn(tracked_orders_[sys_id], '0');
            client_to_sys_.erase(req->client_id);
            tracked_orders_.erase(sys_id);
        }
    }

    void onTick(TickRecord* tick) {
        if (!tick || tick->symbol[0] == '\0') return;
        std::string sym(tick->symbol);
        auto it = engines_.find(sym);
        if (it == engines_.end()) return;

        TickMatchingEngine::Output output;
        it->second->apply_tick(tick->bid_price, tick->bid_volume, 5,
                               tick->ask_price, tick->ask_volume, 5, output);
        for (const auto& trade : output.trades)
            process_trade(trade, 0);
    }

    void onCancel(CancelReq* req) {
        if (!req || req->client_id == 0) return;

        auto cit = client_to_sys_.find(req->client_id);
        if (cit == client_to_sys_.end()) return;
        uint64_t sys_id = cit->second;

        auto it = tracked_orders_.find(sys_id);
        if (it == tracked_orders_.end()) return;

        std::string sym(req->symbol);
        auto eng_it = engines_.find(sym);
        if (eng_it == engines_.end()) return;

        TickMatchingEngine::Output output;
        if (!eng_it->second->cancel(sys_id, output)) return;

        publish_order_rtn(it->second, '5', "已撤单");
        client_to_sys_.erase(req->client_id);
        tracked_orders_.erase(it);
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

    FastHashMap<std::string, std::unique_ptr<TickMatchingEngine>> engines_;
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
