#include "framework.h"
#include "core_state.h"
#include "symbol_manager.h"
#include <atomic>
#include <cstring>
#include <string>

class SimTradeModule : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config, ITimerService* timer_svc = nullptr) override {
        (void)timer_svc;
        bus_ = bus;

        if (config.count("debug")) debug_ = (config.at("debug") == "true");
        if (config.count("slippage_ticks")) slippage_ticks_ = std::stod(config.at("slippage_ticks"));
        if (config.count("tick_size")) tick_size_ = std::stod(config.at("tick_size"));
        if (config.count("initial_balance")) initial_balance_ = std::stod(config.at("initial_balance"));
        if (config.count("account_id")) account_id_ = config.at("account_id");
        if (config.count("broker_id")) broker_id_ = config.at("broker_id");

        balance_ = initial_balance_;
        available_ = initial_balance_;

        if (debug_) {
            LOG_INFO("[SimTrade] Initialized. balance={} slippage_ticks={} tick_size={}",
                     balance_,
                     slippage_ticks_,
                     tick_size_);
        }


        bus_->subscribe(EVENT_ORDER_SEND,
                        StaticDelegate<void(void*)>::bind<SimTradeModule, &SimTradeModule::on_order_send_event>(this));

        bus_->subscribe(EVENT_CANCEL_SEND,
                        StaticDelegate<void(void*)>::bind<SimTradeModule, &SimTradeModule::on_cancel_send_event>(this));

        bus_->subscribe(EVENT_QRY_ACC,
                        StaticDelegate<void(void*)>::bind<SimTradeModule, &SimTradeModule::on_query_account_event>(this));

        bus_->subscribe(EVENT_QRY_POS,
                        StaticDelegate<void(void*)>::bind<SimTradeModule, &SimTradeModule::on_query_position_event>(this));
    }

private:
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

    void onOrder(const OrderReq* req) {
        if (!req || req->symbol[0] == '\0' || req->volume <= 0) return;

        double fill_price = req->price;
        if (fill_price <= 0.0) {
            fill_price = 0.0;
        }

        if (tick_size_ > 0.0 && slippage_ticks_ != 0.0) {
            double slip = slippage_ticks_ * tick_size_;
            if (req->direction == 'B') fill_price += slip;
            else if (req->direction == 'S') fill_price -= slip;
        }

        if (fill_price < 0.0) fill_price = 0.0;

        OrderRtn order_rtn;
        std::memset(&order_rtn, 0, sizeof(order_rtn));
        fillOrderRtn(req, fill_price, &order_rtn);
        const auto& core = core::CoreServicesRegistry::get();
        if (core.order_service) {
            core.order_service->enqueue_order_rtn(order_rtn);
        }
        bus_->publish(EVENT_RTN_ORDER, &order_rtn);

        TradeRtn trade_rtn;
        std::memset(&trade_rtn, 0, sizeof(trade_rtn));
        fillTradeRtn(req, fill_price, &order_rtn, &trade_rtn);
        if (core.order_service) {
            core.order_service->enqueue_trade_rtn(trade_rtn);
        }
        if (core.position_service) {
            core.position_service->enqueue_trade(trade_rtn);
        }
        bus_->publish(EVENT_RTN_TRADE, &trade_rtn);

        updateAccount(req, fill_price);
    }

    void onCancel(const CancelReq* req) {
        if (!req) return;
        // InstantFill 模式下，撤单默认视为已成交/不可撤
        OrderRtn order_rtn;
        std::memset(&order_rtn, 0, sizeof(order_rtn));
        std::strncpy(order_rtn.account_id, account_id_.c_str(), sizeof(order_rtn.account_id) - 1);
        std::strncpy(order_rtn.exchange_id, "SIM", sizeof(order_rtn.exchange_id) - 1);
        std::memset(order_rtn.order_ref, 0, sizeof(order_rtn.order_ref));
        std::memset(order_rtn.order_sys_id, 0, sizeof(order_rtn.order_sys_id));
        std::strncpy(order_rtn.order_ref, req->order_ref, sizeof(order_rtn.order_ref) - 1);
        std::strncpy(order_rtn.order_sys_id, req->order_sys_id, sizeof(order_rtn.order_sys_id) - 1);
        order_rtn.status = '0';
        std::strncpy(order_rtn.status_msg, "AlreadyFilled", sizeof(order_rtn.status_msg) - 1);
        const auto& core = core::CoreServicesRegistry::get();
        if (core.order_service) {
            core.order_service->enqueue_order_rtn(order_rtn);
        }
        bus_->publish(EVENT_RTN_ORDER, &order_rtn);
    }

    void fillOrderRtn(const OrderReq* req, double fill_price, OrderRtn* rtn) {
        std::strncpy(rtn->account_id, account_id_.c_str(), sizeof(rtn->account_id) - 1);
        std::strncpy(rtn->exchange_id, "SIM", sizeof(rtn->exchange_id) - 1);
        std::strncpy(rtn->symbol, req->symbol, sizeof(rtn->symbol) - 1);
        rtn->symbol_id = req->symbol_id ? req->symbol_id : SymbolManager::instance().get_id(req->symbol);
        rtn->direction = req->direction;
        rtn->offset_flag = req->offset_flag;
        rtn->limit_price = fill_price;
        rtn->volume_total = req->volume;
        rtn->volume_traded = req->volume;
        rtn->status = '0';
        std::strncpy(rtn->status_msg, "Filled", sizeof(rtn->status_msg) - 1);

        std::memset(rtn->order_ref, 0, sizeof(rtn->order_ref));
        std::strncpy(rtn->order_ref, req->order_ref, sizeof(rtn->order_ref) - 1);
        format_sys_id(rtn->order_sys_id, sizeof(rtn->order_sys_id));
    }

    void fillTradeRtn(const OrderReq* req, double fill_price, const OrderRtn* order_rtn, TradeRtn* rtn) {
        std::strncpy(rtn->account_id, account_id_.c_str(), sizeof(rtn->account_id) - 1);
        std::strncpy(rtn->exchange_id, "SIM", sizeof(rtn->exchange_id) - 1);
        std::strncpy(rtn->symbol, req->symbol, sizeof(rtn->symbol) - 1);
        rtn->symbol_id = order_rtn->symbol_id;
        rtn->direction = req->direction;
        rtn->offset_flag = req->offset_flag;
        rtn->price = fill_price;
        rtn->volume = req->volume;
        std::memset(rtn->order_ref, 0, sizeof(rtn->order_ref));
        std::memset(rtn->order_sys_id, 0, sizeof(rtn->order_sys_id));
        std::strncpy(rtn->order_ref, order_rtn->order_ref, sizeof(rtn->order_ref) - 1);
        std::strncpy(rtn->order_sys_id, order_rtn->order_sys_id, sizeof(rtn->order_sys_id) - 1);
        format_trade_id(rtn->trade_id, sizeof(rtn->trade_id));
    }

    void updateAccount(const OrderReq* req, double fill_price) {
        double multiplier = SymbolManager::instance().get_multiplier(req->symbol);
        double notional = fill_price * static_cast<double>(req->volume) * multiplier;

        if (req->direction == 'B') {
            balance_ -= notional;
        } else if (req->direction == 'S') {
            balance_ += notional;
        }
        available_ = balance_;

        if (debug_) {
            LOG_DEBUG("[SimTrade] Trade {} {} vol={} price={} balance={}",
                      req->symbol,
                      req->direction,
                      req->volume,
                      fill_price,
                      balance_);
        }

        publishAccountLocked();
    }

    void publishAccount() {
        publishAccountLocked();
    }

    void publishAccountLocked() {
        AccountDetail acc;
        std::memset(&acc, 0, sizeof(acc));
        std::strncpy(acc.broker_id, broker_id_.c_str(), sizeof(acc.broker_id) - 1);
        std::strncpy(acc.account_id, account_id_.c_str(), sizeof(acc.account_id) - 1);
        acc.balance = balance_;
        acc.available = available_;
        acc.margin = 0.0;
        acc.close_pnl = 0.0;
        acc.position_pnl = 0.0;
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

private:
    EventBus* bus_ = nullptr;
    bool debug_ = false;
    double slippage_ticks_ = 0.0;
    double tick_size_ = 1.0;
    double initial_balance_ = 1000000.0;
    std::string account_id_ = "SIM";
    std::string broker_id_ = "SIM";

    double balance_ = 0.0;
    double available_ = 0.0;

    std::atomic<uint64_t> next_order_sys_id_{1};
    std::atomic<uint64_t> next_trade_id_{1};
};

EXPORT_MODULE(SimTradeModule)
