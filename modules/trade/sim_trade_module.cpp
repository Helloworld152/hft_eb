#include "framework.h"
#include "core_state.h"
#include "symbol_manager.h"
#include "tick_matching_engine.h"

#include <atomic>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

// ==========================================================================
// SimTrade — 限价订单簿撮合 + 模拟账户
//
// 【撮合】订阅 EVENT_ORDER_SEND / EVENT_MARKET_DATA / EVENT_CANCEL_SEND
//        订单到达时扫内部对手队列 → 扫行情盘口 → 挂单排队
//        tick 到达时扫挂单队列 → 行情盘口撮合
// 【账户】订阅 EVENT_RTN_TRADE / EVENT_RTN_ORDER（自己发的）
//        按开平仓计算已实现盈亏，更新 balance 并发布 EVENT_ACC_UPDATE
//        开仓价记录用 FIFO 成本队列，与 PositionService 的持仓数量对应
// ==========================================================================

class SimTradeModule : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config, ITimerService* timer_svc = nullptr) override {
        (void)timer_svc;
        bus_ = bus;

        // 撮合配置
        if (config.count("debug")) debug_ = (config.at("debug") == "true");
        if (config.count("max_orders")) max_orders_ = std::stoul(config.at("max_orders"));

        // 账户配置
        if (config.count("initial_balance")) initial_balance_ = std::stod(config.at("initial_balance"));
        if (config.count("account_id")) account_id_ = config.at("account_id");
        if (config.count("broker_id")) broker_id_ = config.at("broker_id");
        balance_ = initial_balance_;
        available_ = initial_balance_;

        if (debug_) {
            LOG_INFO("[SimTrade] max_orders={} balance={}", max_orders_, balance_);
        }

        // 撮合事件
        bus_->subscribe(EVENT_ORDER_SEND,
                        StaticDelegate<void(void*)>::bind<SimTradeModule, &SimTradeModule::on_order_send>(this));
        bus_->subscribe(EVENT_CANCEL_SEND,
                        StaticDelegate<void(void*)>::bind<SimTradeModule, &SimTradeModule::on_cancel_send>(this));
        bus_->subscribe(EVENT_MARKET_DATA,
                        StaticDelegate<void(void*)>::bind<SimTradeModule, &SimTradeModule::on_market_data>(this));

        // 查询事件
        bus_->subscribe(EVENT_QRY_ACC,
                        StaticDelegate<void(void*)>::bind<SimTradeModule, &SimTradeModule::on_query_account>(this));
        bus_->subscribe(EVENT_QRY_POS,
                        StaticDelegate<void(void*)>::bind<SimTradeModule, &SimTradeModule::on_query_position>(this));
    }

private:
    // ======================================================================
    //  数据定义
    // ======================================================================

    struct TrackedOrder {
        uint64_t client_id;
        char order_ref[13];
        char order_sys_id[21];
        char account_id[16];
        char symbol[32];
        uint64_t symbol_id;
        char direction;
        char offset_flag;
        double limit_price;
        int volume_total;
        int volume_traded;
        bool is_market;
    };

    // ======================================================================
    //  撮合引擎 — ID 映射
    // ======================================================================

    uint64_t alloc_engine_id() {
        if (!free_engine_ids_.empty()) {
            uint64_t id = free_engine_ids_.back();
            free_engine_ids_.pop_back();
            return id;
        }
        return next_engine_id_++;
    }

    uint64_t to_engine_id(uint64_t client_id) {
        auto it = client_to_engine_.find(client_id);
        if (it != client_to_engine_.end()) return it->second;
        uint64_t eid = alloc_engine_id();
        client_to_engine_[client_id] = eid;
        engine_to_client_[eid] = client_id;
        return eid;
    }

    uint64_t to_client_id(uint64_t engine_id) {
        auto it = engine_to_client_.find(engine_id);
        return it != engine_to_client_.end() ? it->second : 0;
    }

    void cleanup_id_map(uint64_t client_id) {
        auto it = client_to_engine_.find(client_id);
        if (it != client_to_engine_.end()) {
            free_engine_ids_.push_back(it->second);
            engine_to_client_.erase(it->second);
            client_to_engine_.erase(it);
        }
    }

    TickMatchingEngine& get_engine(const std::string& symbol) {
        auto it = engines_.find(symbol);
        if (it != engines_.end()) return *it->second;
        auto eng = std::make_unique<TickMatchingEngine>(max_orders_);
        TickMatchingEngine* ptr = eng.get();
        engines_[symbol] = std::move(eng);
        return *ptr;
    }

    // ======================================================================
    //  撮合引擎 — 事件入口
    // ======================================================================

    void on_order_send(void* d)    { onOrder(static_cast<OrderReq*>(d)); }
    void on_cancel_send(void* d)   { onCancel(static_cast<CancelReq*>(d)); }
    void on_market_data(void* d)   { onTick(static_cast<TickRecord*>(d)); }
    void on_query_account(void*)   { publishAccount(); }
    void on_query_position(void*)  {}

    void onOrder(OrderReq* req) {
        if (!req || req->symbol[0] == '\0' || req->volume <= 0) return;

        std::string sym(req->symbol);
        auto& engine = get_engine(sym);
        const auto side = (req->direction == 'B') ? TickMatchingEngine::BUY
                                                   : TickMatchingEngine::SELL;
        const bool is_market = (req->price <= 0.0);

        const uint64_t client_id = req->client_id;

        TrackedOrder tracked{};
        tracked.client_id = client_id;
        std::strncpy(tracked.order_ref, req->order_ref, sizeof(tracked.order_ref) - 1);
        std::strncpy(tracked.account_id, req->account_id, sizeof(tracked.account_id) - 1);
        std::strncpy(tracked.symbol, req->symbol, sizeof(tracked.symbol) - 1);
        tracked.symbol_id = req->symbol_id;
        tracked.direction = req->direction;
        tracked.offset_flag = req->offset_flag;
        tracked.limit_price = req->price;
        tracked.volume_total = req->volume;
        tracked.is_market = is_market;
        format_sys_id(tracked.order_sys_id, sizeof(tracked.order_sys_id));
        tracked_orders_[client_id] = tracked;

        const uint64_t engine_id = to_engine_id(client_id);
        TickMatchingEngine::Output output;
        bool ok = engine.submit(engine_id, side, req->price, req->volume, is_market,
                                nullptr, nullptr, 0, nullptr, nullptr, 0, output);
        if (!ok) {
            publish_order_rtn(tracked, '5', is_market ? "无对手方" : "引擎拒绝");
            tracked_orders_.erase(client_id);
            cleanup_id_map(client_id);
            return;
        }

        int taker_filled = 0;
        for (const auto& trade : output.trades)
            taker_filled += process_trade(trade, to_engine_id(client_id));

        tracked_orders_[client_id].volume_traded = taker_filled;
        if (output.resting)
            publish_order_rtn(tracked_orders_[client_id], taker_filled > 0 ? '1' : '3');
        else {
            publish_order_rtn(tracked_orders_[client_id], '0');
            tracked_orders_.erase(client_id);
            cleanup_id_map(client_id);
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
        if (!req) return;
        const uint64_t client_id = req->client_id;
        auto it = tracked_orders_.find(client_id);
        if (it == tracked_orders_.end()) return;
        auto eit = client_to_engine_.find(client_id);
        if (eit == client_to_engine_.end()) return;
        std::string sym(req->symbol);
        auto eng_it = engines_.find(sym);
        if (eng_it == engines_.end()) return;

        TickMatchingEngine::Output output;
        if (!eng_it->second->cancel(eit->second, output)) return;
        publish_order_rtn(it->second, '5', "已撤单");
        tracked_orders_.erase(it);
        cleanup_id_map(client_id);
    }

    // ---- 成交处理 ----
    int process_trade(const TickMatchingEngine::Trade& trade, uint64_t taker_eid) {
        int related_qty = 0;
        auto process_side = [&](uint64_t eid) {
            if (eid == 0) return;
            uint64_t cl = to_client_id(eid);
            auto it = tracked_orders_.find(cl);
            if (it == tracked_orders_.end()) return;
            auto& o = it->second;
            o.volume_traded += trade.qty;
            publish_trade_rtn(o, trade.price, trade.qty);

            if (o.volume_traded >= o.volume_total) {
                publish_order_rtn(o, '0');
                tracked_orders_.erase(it);
                cleanup_id_map(cl);
            } else {
                publish_order_rtn(o, '1');
            }
            if (eid == taker_eid) related_qty += trade.qty;
        };
        process_side(trade.maker_id);
        process_side(trade.taker_id);
        return related_qty;
    }

    // ======================================================================
    //  撮合引擎 — 回报发布
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
        std::strncpy(rtn.order_ref, order.order_ref, sizeof(rtn.order_ref) - 1);
        std::strncpy(rtn.order_sys_id, order.order_sys_id, sizeof(rtn.order_sys_id) - 1);
        if (msg) {
            std::strncpy(rtn.status_msg, msg, sizeof(rtn.status_msg) - 1);
        } else {
            const char* default_msg =
                (status == '0') ? "全部成交" :
                (status == '1') ? "部分成交" :
                (status == '3') ? "已报" :
                (status == '5') ? "已撤单" : "";
            std::strncpy(rtn.status_msg, default_msg, sizeof(rtn.status_msg) - 1);
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
        std::strncpy(rtn.order_ref, order.order_ref, sizeof(rtn.order_ref) - 1);
        std::strncpy(rtn.order_sys_id, order.order_sys_id, sizeof(rtn.order_sys_id) - 1);
        format_trade_id(rtn.trade_id, sizeof(rtn.trade_id));

        // → 通知 core services（PositionService 更新持仓数量）
        const auto& core = core::CoreServicesRegistry::get();
        if (core.order_service) core.order_service->enqueue_trade_rtn(rtn);
        if (core.position_service) core.position_service->enqueue_trade(rtn);
        bus_->publish(EVENT_RTN_TRADE, &rtn);

        // → 更新模拟账户
        onTradeFill(order, price, qty);
    }

    // ======================================================================
    //  模拟账户 — 权益计算
    //  开仓: 记录成本，balance 不变
    //  平仓: balance += (平仓价 - 开仓价) * multiplier * qty
    // ======================================================================

    void onTradeFill(const TrackedOrder& order, double fill_price, int qty) {
        double multiplier = SymbolManager::instance().get_multiplier(order.symbol);
        bool is_open = (order.offset_flag == 'O');

        if (is_open) {
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

    // ======================================================================
    //  工具
    // ======================================================================

    void format_sys_id(char* buf, size_t len) {
        uint64_t id = next_order_sys_id_.fetch_add(1);
        std::snprintf(buf, len, "SIM%llu", static_cast<unsigned long long>(id));
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

    // 撮合
    size_t max_orders_ = 10000;
    FastHashMap<std::string, std::unique_ptr<TickMatchingEngine>> engines_;
    FastHashMap<uint64_t, TrackedOrder> tracked_orders_;
    FastHashMap<uint64_t, uint64_t> client_to_engine_;
    FastHashMap<uint64_t, uint64_t> engine_to_client_;
    std::vector<uint64_t> free_engine_ids_;
    uint64_t next_engine_id_ = 1;

    // 账户
    double initial_balance_ = 1000000.0;
    std::string account_id_ = "SIM";
    std::string broker_id_ = "SIM";
    double balance_ = 0.0;
    double available_ = 0.0;
    FastHashMap<uint64_t, std::deque<double>> cost_map_;  // symbol_id → 开仓价 FIFO

    // ID 生成
    std::atomic<uint64_t> next_order_sys_id_{1};
    std::atomic<uint64_t> next_trade_id_{1};
};

EXPORT_MODULE(SimTradeModule)
