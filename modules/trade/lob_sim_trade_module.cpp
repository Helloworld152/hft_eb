#include "framework.h"
#include "core_state.h"
#include "symbol_manager.h"
#include "simple_matching_engine.h"

#include <atomic>
#include <climits>
#include <cstring>
#include <string>
#include <vector>

class LobSimTradeModule : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config, ITimerService* timer_svc = nullptr) override {
        (void)timer_svc;
        bus_ = bus;

        if (config.count("debug")) debug_ = (config.at("debug") == "true");
        if (config.count("initial_balance")) initial_balance_ = std::stod(config.at("initial_balance"));
        if (config.count("account_id")) account_id_ = config.at("account_id");
        if (config.count("broker_id")) broker_id_ = config.at("broker_id");
        if (config.count("max_orders")) max_orders_ = std::stoul(config.at("max_orders"));
        if (config.count("max_levels")) max_levels_ = std::stoul(config.at("max_levels"));

        balance_ = initial_balance_;
        available_ = initial_balance_;

        if (debug_) {
            LOG_INFO("[LobSimTrade] LOB mode. balance={} max_orders={} max_levels={}",
                     balance_, max_orders_, max_levels_);
        }

        bus_->subscribe(EVENT_ORDER_SEND,
                        StaticDelegate<void(void*)>::bind<LobSimTradeModule, &LobSimTradeModule::on_order_send_event>(this));
        bus_->subscribe(EVENT_CANCEL_SEND,
                        StaticDelegate<void(void*)>::bind<LobSimTradeModule, &LobSimTradeModule::on_cancel_send_event>(this));
        bus_->subscribe(EVENT_QRY_ACC,
                        StaticDelegate<void(void*)>::bind<LobSimTradeModule, &LobSimTradeModule::on_query_account_event>(this));
        bus_->subscribe(EVENT_QRY_POS,
                        StaticDelegate<void(void*)>::bind<LobSimTradeModule, &LobSimTradeModule::on_query_position_event>(this));
    }

private:
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
        bool is_market; // 市价单标记
    };

    // ---- ID 映射：任意 client_id → engine 内部小整数 ID ----
    uint64_t alloc_engine_id() {
        if (!free_engine_ids_.empty()) {
            uint64_t id = free_engine_ids_.back();
            free_engine_ids_.pop_back();
            return id;
        }
        return next_engine_id_++;
    }

    void release_engine_id(uint64_t id) {
        free_engine_ids_.push_back(id);
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
            release_engine_id(it->second);
            engine_to_client_.erase(it->second);
            client_to_engine_.erase(it);
        }
    }

    // ---- 获取 per-symbol engine ----
    SimpleMatchingEngine& get_engine(const std::string& symbol) {
        auto it = engines_.find(symbol);
        if (it != engines_.end()) return *it->second;
        auto eng = std::make_unique<SimpleMatchingEngine>(max_orders_, max_levels_);
        SimpleMatchingEngine* ptr = eng.get();
        engines_[symbol] = std::move(eng);
        return *ptr;
    }

    // ---- 事件处理 ----
    void on_order_send_event(void* d) {
        onOrder(static_cast<OrderReq*>(d));
    }

    void on_cancel_send_event(void* d) {
        onCancel(static_cast<CancelReq*>(d));
    }

    void on_query_account_event(void*) {
        publishAccount();
    }

    void on_query_position_event(void*) {
    }

    void onOrder(OrderReq* req) {
        if (!req || req->symbol[0] == '\0' || req->volume <= 0) return;

        std::string sym(req->symbol);
        auto& engine = get_engine(sym);

        const auto side = (req->direction == 'B') ? SimpleMatchingEngine::BUY
                                                   : SimpleMatchingEngine::SELL;

        const bool is_market = (req->price <= 0.0);
        int64_t limit_price;
        if (is_market) {
            limit_price = (side == SimpleMatchingEngine::BUY) ? INT64_MAX : INT64_MIN;
        } else {
            limit_price = static_cast<int64_t>(req->price);
        }

        // 分配 engine 内部 ID
        const uint64_t client_id = req->client_id;
        const uint64_t engine_id = to_engine_id(client_id);

        // 记录订单状态
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
        tracked.volume_traded = 0;
        tracked.is_market = is_market;
        format_sys_id(tracked.order_sys_id, sizeof(tracked.order_sys_id));
        tracked_orders_[client_id] = tracked;

        // 提交到撮合引擎
        SimpleMatchingEngine::Output output;
        bool ok = engine.submit(engine_id, side, limit_price, req->volume, output);

        if (!ok) {
            // 引擎拒绝（重复 ID / pool 满）
            publish_order_rtn(tracked, '5', "EngineRejected");
            tracked_orders_.erase(client_id);
            cleanup_id_map(client_id);
            return;
        }

        // 累计 taker 成交量
        int taker_filled = 0;

        // 处理每笔成交
        for (const auto& trade : output.trades) {
            taker_filled += process_trade(trade, client_id);
        }

        // 市价单：若有剩余且挂入了 book，立即撤单
        if (is_market) {
            bool resting = false;
            for (const auto& entrust : output.entrusts) {
                if (entrust.order_id == engine_id && entrust.type == SimpleMatchingEngine::ADD) {
                    resting = true;
                    break;
                }
            }
            if (resting) {
                SimpleMatchingEngine::Output cancel_out;
                engine.cancel(engine_id, cancel_out);
            }

            tracked_orders_[client_id].volume_traded = taker_filled;
            if (taker_filled > 0) {
                publish_order_rtn(tracked_orders_[client_id], '0');
            } else {
                publish_order_rtn(tracked_orders_[client_id], '5', "NoLiquidity");
            }
            tracked_orders_.erase(client_id);
            cleanup_id_map(client_id);
            return;
        }

        // 限价单：判断最终状态
        bool resting = false;
        for (const auto& entrust : output.entrusts) {
            if (entrust.order_id == engine_id && entrust.type == SimpleMatchingEngine::ADD) {
                resting = true;
                break;
            }
        }

        tracked_orders_[client_id].volume_traded = taker_filled;

        if (resting) {
            if (taker_filled > 0) {
                publish_order_rtn(tracked_orders_[client_id], '1');
            } else {
                publish_order_rtn(tracked_orders_[client_id], '3');
            }
            // 保持在 tracked_orders_ 中（挂单等待撮合）
        } else {
            publish_order_rtn(tracked_orders_[client_id], '0');
            tracked_orders_.erase(client_id);
            cleanup_id_map(client_id);
        }
    }

    // 处理单笔成交，返回 taker (传入 client_id 对应方) 的成交量
    int process_trade(const SimpleMatchingEngine::TradeTick& trade, uint64_t taker_client_id) {
        const uint64_t bid_client = to_client_id(trade.bid_order_id);
        const uint64_t ask_client = to_client_id(trade.ask_order_id);

        auto it_bid = tracked_orders_.find(bid_client);
        auto it_ask = tracked_orders_.find(ask_client);

        if (it_bid == tracked_orders_.end() || it_ask == tracked_orders_.end()) {
            return 0;
        }

        auto& bid_order = it_bid->second;
        auto& ask_order = it_ask->second;
        const double trade_price = static_cast<double>(trade.price);
        const int qty = trade.qty;

        // 更新成交量
        bid_order.volume_traded += qty;
        ask_order.volume_traded += qty;

        // 发布成交回报
        publish_trade_rtn(bid_order, trade_price, qty);
        publish_trade_rtn(ask_order, trade_price, qty);

        // 更新双方报单状态
        auto update_side = [&](TrackedOrder& order, uint64_t client_id) {
            if (order.volume_traded >= order.volume_total) {
                publish_order_rtn(order, '0');
                tracked_orders_.erase(client_id);
                cleanup_id_map(client_id);
            } else {
                publish_order_rtn(order, '1');
            }
        };

        update_side(bid_order, bid_client);
        update_side(ask_order, ask_client);

        // 更新账户（双向）
        updateAccount(bid_order, trade_price, qty);
        updateAccount(ask_order, trade_price, qty);

        // 返回 taker 在这笔成交中的量
        if (bid_client == taker_client_id || ask_client == taker_client_id) {
            return qty;
        }
        return 0;
    }

    void onCancel(CancelReq* req) {
        if (!req) return;

        const uint64_t client_id = req->client_id;
        auto it = tracked_orders_.find(client_id);
        if (it == tracked_orders_.end()) {
            LOG_DEBUG("[LobSimTrade] Cancel unknown order client_id={}", client_id);
            return;
        }

        auto eit = client_to_engine_.find(client_id);
        if (eit == client_to_engine_.end()) {
            LOG_DEBUG("[LobSimTrade] Cancel order not in engine client_id={}", client_id);
            return;
        }

        std::string sym(req->symbol);
        auto eng_it = engines_.find(sym);
        if (eng_it == engines_.end()) {
            return;
        }

        SimpleMatchingEngine::Output output;
        bool ok = eng_it->second->cancel(eit->second, output);

        if (ok) {
            auto& order = it->second;
            publish_order_rtn(order, '5', "Cancelled");
            // 更新账户：撤单返还资金（若为买单，解冻资金）
            if (order.direction == 'B') {
                double multiplier = SymbolManager::instance().get_multiplier(order.symbol);
                double notional = order.limit_price * (order.volume_total - order.volume_traded) * multiplier;
                balance_ += notional;
                available_ = balance_;
            }
            tracked_orders_.erase(it);
            cleanup_id_map(client_id);
        } else {
            LOG_DEBUG("[LobSimTrade] Cancel failed client_id={}", client_id);
        }
    }

    // ---- 回报发布 ----
    void publish_order_rtn(const TrackedOrder& order, char status, const char* msg = nullptr) {
        OrderRtn rtn;
        std::memset(&rtn, 0, sizeof(rtn));
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
        }

        const auto& core = core::CoreServicesRegistry::get();
        if (core.order_service) {
            core.order_service->enqueue_order_rtn(rtn);
        }
        bus_->publish(EVENT_RTN_ORDER, &rtn);
    }

    void publish_trade_rtn(const TrackedOrder& order, double price, int qty) {
        TradeRtn rtn;
        std::memset(&rtn, 0, sizeof(rtn));
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

        const auto& core = core::CoreServicesRegistry::get();
        if (core.order_service) {
            core.order_service->enqueue_trade_rtn(rtn);
        }
        if (core.position_service) {
            core.position_service->enqueue_trade(rtn);
        }
        bus_->publish(EVENT_RTN_TRADE, &rtn);
    }

    // ---- 账户管理 ----
    void updateAccount(const TrackedOrder& order, double fill_price, int qty) {
        double multiplier = SymbolManager::instance().get_multiplier(order.symbol);
        double notional = fill_price * static_cast<double>(qty) * multiplier;

        if (order.direction == 'B') {
            balance_ -= notional;
        } else if (order.direction == 'S') {
            balance_ += notional;
        }
        available_ = balance_;
    }

    void publishAccount() {
        AccountDetail acc;
        std::memset(&acc, 0, sizeof(acc));
        std::strncpy(acc.broker_id, broker_id_.c_str(), sizeof(acc.broker_id) - 1);
        std::strncpy(acc.account_id, account_id_.c_str(), sizeof(acc.account_id) - 1);
        acc.balance = balance_;
        acc.available = available_;
        const auto& core = core::CoreServicesRegistry::get();
        if (core.account_service) {
            core.account_service->enqueue_account(acc);
        }
        bus_->publish(EVENT_ACC_UPDATE, &acc);
    }

    void format_sys_id(char* buf, size_t len) {
        uint64_t id = next_order_sys_id_.fetch_add(1);
        std::snprintf(buf, len, "SIM%llu", static_cast<unsigned long long>(id));
    }

    void format_trade_id(char* buf, size_t len) {
        uint64_t id = next_trade_id_.fetch_add(1);
        std::snprintf(buf, len, "T%llu", static_cast<unsigned long long>(id));
    }

    // ---- 成员变量 ----
    EventBus* bus_ = nullptr;
    bool debug_ = false;
    double initial_balance_ = 1000000.0;
    std::string account_id_ = "SIM";
    std::string broker_id_ = "SIM";
    size_t max_orders_ = 10000;
    size_t max_levels_ = 100;

    double balance_ = 0.0;
    double available_ = 0.0;

    // Per-symbol 撮合引擎
    FastHashMap<std::string, std::unique_ptr<SimpleMatchingEngine>> engines_;

    // 订单状态追踪（key = client_id）
    FastHashMap<uint64_t, TrackedOrder> tracked_orders_;

    // ID 映射
    FastHashMap<uint64_t, uint64_t> client_to_engine_;
    FastHashMap<uint64_t, uint64_t> engine_to_client_;
    std::vector<uint64_t> free_engine_ids_;
    uint64_t next_engine_id_ = 1;

    std::atomic<uint64_t> next_order_sys_id_{1};
    std::atomic<uint64_t> next_trade_id_{1};
};

EXPORT_MODULE(LobSimTradeModule)
