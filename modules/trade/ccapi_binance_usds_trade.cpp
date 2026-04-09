#include "../../include/framework.h"
#include "../../core/include/core_state.h"
#include "../../core/include/symbol_manager.h"
#include "ccapi_cpp/ccapi_macro.h"
#include "ccapi_cpp/ccapi_session.h"

#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

ccapi::Logger* ccapi::Logger::logger = nullptr;

namespace {

double to_double(const std::string& s) {
    if (s.empty()) return 0.0;
    try {
        return std::stod(s);
    } catch (...) {
        return 0.0;
    }
}

int to_int(const std::string& s) {
    if (s.empty()) return 0;
    try {
        return static_cast<int>(std::stod(s));
    } catch (...) {
        return 0;
    }
}

char map_side(const std::string& side) {
    if (side == CCAPI_EM_ORDER_SIDE_BUY || side == "BUY") return 'B';
    if (side == CCAPI_EM_ORDER_SIDE_SELL || side == "SELL") return 'S';
    return 'U';
}

char map_status(const std::string& status) {
    if (status == "FILLED") return '0';
    if (status == "PARTIALLY_FILLED") return '1';
    if (status == "NEW") return '3';
    if (status == "CANCELED" || status == "REJECTED" || status == "EXPIRED") return '5';
    return 'a';
}

std::string to_upper(const std::string& s) {
    std::string out = s;
    for (auto& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

}  // namespace

class BinanceUsdsTradeModule;

class BinanceCcapiEventHandler : public ccapi::EventHandler {
public:
    explicit BinanceCcapiEventHandler(BinanceUsdsTradeModule* owner) : owner_(owner) {}

    void processEvent(const ccapi::Event& event, ccapi::Session* session) override;

private:
    BinanceUsdsTradeModule* owner_ = nullptr;
};

class BinanceUsdsTradeModule : public IModule {
public:
    BinanceUsdsTradeModule() = default;
    ~BinanceUsdsTradeModule() override { stop(); }

    void init(EventBus* bus, const ConfigMap& config, ITimerService* timer_svc = nullptr) override {
        bus_ = bus;
        timer_svc_ = timer_svc;

        if (config.count("account_id")) account_id_ = config.at("account_id");
        if (config.count("api_key")) api_key_ = config.at("api_key");
        if (config.count("api_secret")) api_secret_ = config.at("api_secret");
        if (config.count("base_asset")) base_asset_ = to_upper(config.at("base_asset"));
        if (config.count("position_side")) position_side_ = to_upper(config.at("position_side"));
        if (config.count("proxy")) proxy_ = config.at("proxy");
        if (config.count("reduce_only_on_close")) {
            const auto& v = config.at("reduce_only_on_close");
            reduce_only_on_close_ = (v == "true" || v == "1");
        }
        if (config.count("debug")) {
            const auto& v = config.at("debug");
            debug_ = (v == "true" || v == "1");
        }

        if (!api_key_.empty()) credential_[CCAPI_BINANCE_USDS_FUTURES_API_KEY] = api_key_;
        if (!api_secret_.empty()) credential_[CCAPI_BINANCE_USDS_FUTURES_API_SECRET] = api_secret_;

        ccapi::SessionOptions session_options;
        ccapi::SessionConfigs session_configs;
        if (!credential_.empty()) {
            session_configs.setCredential(credential_);
        }

        event_handler_ = std::make_unique<BinanceCcapiEventHandler>(this);
        session_ = std::make_unique<ccapi::Session>(session_options, session_configs, event_handler_.get());

        std::string fields = std::string(CCAPI_EM_ORDER_UPDATE) + "," +
                             std::string(CCAPI_EM_PRIVATE_TRADE) + "," +
                             std::string(CCAPI_EM_BALANCE_UPDATE) + "," +
                             std::string(CCAPI_EM_POSITION_UPDATE);

        ccapi::Subscription sub(exchange_, "", fields, "", "em", credential_, proxy_);
        session_->subscribe(sub);

        bus_->subscribe(EVENT_ORDER_SEND, [this](void* d) { this->send_order(static_cast<OrderReq*>(d)); });
        bus_->subscribe(EVENT_CANCEL_SEND, [this](void* d) { this->cancel_order(static_cast<CancelReq*>(d)); });
        bus_->subscribe(EVENT_QRY_ACC, [this](void* d) { (void)d; this->query_account(); });
        bus_->subscribe(EVENT_QRY_POS, [this](void* d) { (void)d; this->query_positions(); });

        std::cout << "[CCAPI-BINANCE-USDS] Initialized. Account=" << account_id_ << " BaseAsset=" << base_asset_ << std::endl;
    }

    void start() override {
        running_ = true;
    }

    void stop() override {
        running_ = false;
        if (session_) {
            session_->stop();
            session_.reset();
        }
        event_handler_.reset();
    }

    void handle_event(const ccapi::Event& event) {
        const auto& msgs = event.getMessageList();
        for (const auto& msg : msgs) {
            switch (msg.getType()) {
                case ccapi::Message::Type::EXECUTION_MANAGEMENT_EVENTS_ORDER_UPDATE:
                    handle_order_message(msg);
                    break;
                case ccapi::Message::Type::EXECUTION_MANAGEMENT_EVENTS_PRIVATE_TRADE:
                    handle_trade_message(msg);
                    break;
                case ccapi::Message::Type::EXECUTION_MANAGEMENT_EVENTS_BALANCE_UPDATE:
                    handle_balance_message(msg);
                    break;
                case ccapi::Message::Type::EXECUTION_MANAGEMENT_EVENTS_POSITION_UPDATE:
                    handle_position_message(msg);
                    break;
                case ccapi::Message::Type::CREATE_ORDER:
                case ccapi::Message::Type::CANCEL_ORDER:
                case ccapi::Message::Type::GET_ORDER:
                case ccapi::Message::Type::GET_OPEN_ORDERS:
                    handle_order_message(msg);
                    break;
                case ccapi::Message::Type::GET_ACCOUNT_BALANCES:
                    handle_balance_message(msg);
                    break;
                case ccapi::Message::Type::GET_ACCOUNT_POSITIONS:
                    handle_position_message(msg);
                    break;
                default:
                    break;
            }
        }
    }

private:
    void send_order(const OrderReq* req) {
        if (!session_) return;
        if (req->price <= 0.0 || req->volume <= 0) {
            if (debug_) {
                std::cerr << "[CCAPI-BINANCE-USDS] Invalid order price/volume." << std::endl;
            }
            return;
        }

        ccapi::Request request(ccapi::Request::Operation::CREATE_ORDER, exchange_, req->symbol);
        std::map<std::string, std::string> param;
        param[CCAPI_EM_ORDER_SIDE] = (req->direction == 'B') ? CCAPI_EM_ORDER_SIDE_BUY : CCAPI_EM_ORDER_SIDE_SELL;
        param[CCAPI_EM_ORDER_QUANTITY] = std::to_string(req->volume);
        param[CCAPI_EM_ORDER_LIMIT_PRICE] = std::to_string(req->price);
        if (req->order_ref[0] != '\0') {
            param[CCAPI_EM_CLIENT_ORDER_ID] = req->order_ref;
        }
        param["timeInForce"] = "GTC";
        if (reduce_only_on_close_ && req->offset_flag != 'O') {
            param["reduceOnly"] = "true";
        }
        if (!position_side_.empty()) {
            param["positionSide"] = position_side_;
        }
        request.appendParam(param);
        if (debug_) {
            std::cout << "[CCAPI-BINANCE-USDS] Send order: " << request.toString() << std::endl;
        }
        session_->sendRequest(request);
    }

    void cancel_order(const CancelReq* req) {
        if (!session_) return;
        ccapi::Request request(ccapi::Request::Operation::CANCEL_ORDER, exchange_, req->symbol);
        std::map<std::string, std::string> param;
        if (req->order_sys_id[0] != '\0') {
            param[CCAPI_EM_ORDER_ID] = req->order_sys_id;
        } else if (req->order_ref[0] != '\0') {
            param[CCAPI_EM_CLIENT_ORDER_ID] = req->order_ref;
        } else {
            if (debug_) {
                std::cerr << "[CCAPI-BINANCE-USDS] Cancel missing order id." << std::endl;
            }
            return;
        }
        request.appendParam(param);
        if (debug_) {
            std::cout << "[CCAPI-BINANCE-USDS] Cancel order: " << request.toString() << std::endl;
        }
        session_->sendRequest(request);
    }

    void query_account() {
        if (!session_) return;
        ccapi::Request request(ccapi::Request::Operation::GET_ACCOUNT_BALANCES, exchange_);
        session_->sendRequest(request);
    }

    void query_positions() {
        if (!session_) return;
        ccapi::Request request(ccapi::Request::Operation::GET_ACCOUNT_POSITIONS, exchange_);
        session_->sendRequest(request);
    }

    void handle_order_message(const ccapi::Message& msg) {
        const auto& elements = msg.getElementList();
        for (const auto& el : elements) {
            OrderRtn rtn;
            std::memset(&rtn, 0, sizeof(rtn));

            std::string symbol = el.getValue(CCAPI_EM_ORDER_INSTRUMENT);
            std::string side = el.getValue(CCAPI_EM_ORDER_SIDE);
            std::string status = el.getValue(CCAPI_EM_ORDER_STATUS);
            std::string order_id = el.getValue(CCAPI_EM_ORDER_ID);
            std::string client_id = el.getValue(CCAPI_EM_CLIENT_ORDER_ID);

            if (!account_id_.empty()) {
                std::strncpy(rtn.account_id, account_id_.c_str(), sizeof(rtn.account_id) - 1);
            }
            std::strncpy(rtn.exchange_id, "BINANCE_USDS", sizeof(rtn.exchange_id) - 1);
            if (!symbol.empty()) {
                std::strncpy(rtn.symbol, symbol.c_str(), sizeof(rtn.symbol) - 1);
                rtn.symbol_id = SymbolManager::instance().get_id(rtn.symbol);
            }
            rtn.direction = map_side(side);
            rtn.offset_flag = 'O';
            rtn.limit_price = to_double(el.getValue(CCAPI_EM_ORDER_LIMIT_PRICE));
            rtn.volume_total = to_int(el.getValue(CCAPI_EM_ORDER_QUANTITY));
            rtn.volume_traded = to_int(el.getValue(CCAPI_EM_ORDER_CUMULATIVE_FILLED_QUANTITY));
            rtn.status = map_status(status);
            if (!status.empty()) {
                std::strncpy(rtn.status_msg, status.c_str(), sizeof(rtn.status_msg) - 1);
            }
            if (!client_id.empty()) {
                std::strncpy(rtn.order_ref, client_id.c_str(), sizeof(rtn.order_ref) - 1);
            } else if (!order_id.empty()) {
                std::strncpy(rtn.order_ref, order_id.c_str(), sizeof(rtn.order_ref) - 1);
            }
            if (!order_id.empty()) {
                std::strncpy(rtn.order_sys_id, order_id.c_str(), sizeof(rtn.order_sys_id) - 1);
            }

            const auto& core = core::CoreServicesRegistry::get();
            if (core.order_service) {
                core.order_service->enqueue_order_rtn(rtn);
            }
            bus_->publish(EVENT_RTN_ORDER, &rtn);
        }
    }

    void handle_trade_message(const ccapi::Message& msg) {
        const auto& elements = msg.getElementList();
        for (const auto& el : elements) {
            TradeRtn rtn;
            std::memset(&rtn, 0, sizeof(rtn));

            std::string symbol = el.getValue(CCAPI_EM_ORDER_INSTRUMENT);
            std::string side = el.getValue(CCAPI_EM_ORDER_SIDE);
            std::string order_id = el.getValue(CCAPI_EM_ORDER_ID);
            std::string client_id = el.getValue(CCAPI_EM_CLIENT_ORDER_ID);

            if (!account_id_.empty()) {
                std::strncpy(rtn.account_id, account_id_.c_str(), sizeof(rtn.account_id) - 1);
            }
            std::strncpy(rtn.exchange_id, "BINANCE_USDS", sizeof(rtn.exchange_id) - 1);
            if (!symbol.empty()) {
                std::strncpy(rtn.symbol, symbol.c_str(), sizeof(rtn.symbol) - 1);
                rtn.symbol_id = SymbolManager::instance().get_id(rtn.symbol);
            }
            rtn.direction = map_side(side);
            rtn.offset_flag = 'O';
            rtn.price = to_double(el.getValue(CCAPI_EM_ORDER_LAST_EXECUTED_PRICE));
            rtn.volume = to_int(el.getValue(CCAPI_EM_ORDER_LAST_EXECUTED_SIZE));

            std::string trade_id = el.getValue(CCAPI_TRADE_ID);
            if (!trade_id.empty()) {
                std::strncpy(rtn.trade_id, trade_id.c_str(), sizeof(rtn.trade_id) - 1);
            }
            if (!client_id.empty()) {
                std::strncpy(rtn.order_ref, client_id.c_str(), sizeof(rtn.order_ref) - 1);
            }
            if (!order_id.empty()) {
                std::strncpy(rtn.order_sys_id, order_id.c_str(), sizeof(rtn.order_sys_id) - 1);
            }

            const auto& core = core::CoreServicesRegistry::get();
            if (core.order_service) {
                core.order_service->enqueue_trade_rtn(rtn);
            }
            if (core.position_service) {
                core.position_service->enqueue_trade(rtn);
            }
            bus_->publish(EVENT_RTN_TRADE, &rtn);
        }
    }

    void handle_balance_message(const ccapi::Message& msg) {
        const auto& elements = msg.getElementList();
        for (const auto& el : elements) {
            std::string asset = to_upper(el.getValue(CCAPI_EM_ASSET));
            if (!base_asset_.empty() && asset != base_asset_) {
                continue;
            }
            AccountDetail acc;
            std::memset(&acc, 0, sizeof(acc));
            std::strncpy(acc.broker_id, "BINANCE", sizeof(acc.broker_id) - 1);
            if (!account_id_.empty()) {
                std::strncpy(acc.account_id, account_id_.c_str(), sizeof(acc.account_id) - 1);
            }
            acc.balance = to_double(el.getValue(CCAPI_EM_QUANTITY_TOTAL));
            acc.available = to_double(el.getValue(CCAPI_EM_QUANTITY_AVAILABLE_FOR_TRADING));
            const auto& core = core::CoreServicesRegistry::get();
            if (core.account_service) {
                core.account_service->enqueue_account(acc);
            }
            bus_->publish(EVENT_ACC_UPDATE, &acc);
        }
    }

    void handle_position_message(const ccapi::Message& msg) {
        const auto& elements = msg.getElementList();
        for (const auto& el : elements) {
            std::string symbol = el.getValue(CCAPI_INSTRUMENT);
            std::string side = to_upper(el.getValue(CCAPI_EM_POSITION_SIDE));
            double qty = to_double(el.getValue(CCAPI_EM_POSITION_QUANTITY));
            if (symbol.empty() || qty == 0.0) {
                continue;
            }

            PositionDetail pos;
            std::memset(&pos, 0, sizeof(pos));
            if (!account_id_.empty()) {
                std::strncpy(pos.account_id, account_id_.c_str(), sizeof(pos.account_id) - 1);
            }
            std::strncpy(pos.exchange_id, "BINANCE_USDS", sizeof(pos.exchange_id) - 1);
            std::strncpy(pos.symbol, symbol.c_str(), sizeof(pos.symbol) - 1);
            pos.symbol_id = SymbolManager::instance().get_id(pos.symbol);
            pos.position_date = '3';

            double abs_qty = qty >= 0.0 ? qty : -qty;
            if (side == "LONG") {
                pos.direction = '2';
                pos.long_td = static_cast<int>(abs_qty);
            } else if (side == "SHORT") {
                pos.direction = '3';
                pos.short_td = static_cast<int>(abs_qty);
            } else {
                if (qty >= 0.0) {
                    pos.direction = '2';
                    pos.long_td = static_cast<int>(abs_qty);
                } else {
                    pos.direction = '3';
                    pos.short_td = static_cast<int>(abs_qty);
                }
            }

            pos.net_pnl = to_double(el.getValue(CCAPI_EM_UNREALIZED_PNL));
            const auto& core = core::CoreServicesRegistry::get();
            if (core.position_service) {
                core.position_service->enqueue_rsp_pos(pos);
            }
        }
    }

private:
    EventBus* bus_ = nullptr;
    ITimerService* timer_svc_ = nullptr;
    std::string account_id_;
    std::string api_key_;
    std::string api_secret_;
    std::string base_asset_ = "USDT";
    std::string position_side_ = "BOTH";
    std::string proxy_;
    bool reduce_only_on_close_ = true;
    bool debug_ = false;
    bool running_ = false;

    const std::string exchange_ = "binance-usds-futures";
    std::map<std::string, std::string> credential_;

    std::unique_ptr<ccapi::Session> session_;
    std::unique_ptr<BinanceCcapiEventHandler> event_handler_;

    friend class BinanceCcapiEventHandler;
};

void BinanceCcapiEventHandler::processEvent(const ccapi::Event& event, ccapi::Session* session) {
    (void)session;
    if (owner_) owner_->handle_event(event);
}

EXPORT_MODULE(BinanceUsdsTradeModule)
