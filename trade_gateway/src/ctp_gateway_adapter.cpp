#include "../include/trade_gateway/ctp_gateway_adapter.h"

#include "../../core/include/symbol_manager.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <thread>

namespace trade_gateway {
namespace {

uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

void copy_header(MessageHeader* header,
                 EventType type,
                 size_t payload_size,
                 const GatewayConfig& config) {
    header->version = 1;
    header->type = static_cast<uint16_t>(type);
    header->payload_size = static_cast<uint32_t>(payload_size);
    header->ts_ns = now_ns();
    std::strncpy(header->gateway_id, config.gateway_id.c_str(), sizeof(header->gateway_id) - 1);
    std::strncpy(header->account_id, config.account_id.c_str(), sizeof(header->account_id) - 1);
}

void debug_log(const GatewayConfig& config, const std::string& message) {
    if (!config.debug) return;
    std::cout << "[CTPAdapter] " << message << std::endl;
}

}  // namespace

CtpGatewayAdapter::CtpGatewayAdapter(const GatewayConfig& config) : config_(config) {}

CtpGatewayAdapter::~CtpGatewayAdapter() {
    stop();
}

void CtpGatewayAdapter::set_event_publisher(EventPublisher publisher) {
    publisher_ = std::move(publisher);
}

void CtpGatewayAdapter::connect() {
    last_connect_attempt_ns_.store(now_ns(), std::memory_order_relaxed);
    debug_log(config_, "connect start gateway_id=" + config_.gateway_id +
                           " account_id=" + config_.account_id +
                           " td_front=" + config_.td_front);

    if (td_api_) {
        td_api_->RegisterSpi(nullptr);
        td_api_->Release();
        td_api_ = nullptr;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (td_spi_) {
        delete td_spi_;
        td_spi_ = nullptr;
    }

    ready_.store(false, std::memory_order_release);
    logged_in_.store(false, std::memory_order_release);

    std::string flow_path = config_.flow_dir + "/td_" + config_.user_id + "_";
    debug_log(config_, "create trader api flow_path=" + flow_path);
    td_api_ = CThostFtdcTraderApi::CreateFtdcTraderApi(flow_path.c_str());
    td_spi_ = new TraderSpi(this);
    td_api_->RegisterSpi(td_spi_);
    td_api_->RegisterFront(const_cast<char*>(config_.td_front.c_str()));
    td_api_->SubscribePublicTopic(THOST_TERT_QUICK);
    td_api_->SubscribePrivateTopic(THOST_TERT_QUICK);
    td_api_->Init();
}

void CtpGatewayAdapter::stop() {
    debug_log(config_, "stop");
    ready_.store(false, std::memory_order_release);
    logged_in_.store(false, std::memory_order_release);
    if (td_api_) {
        td_api_->RegisterSpi(nullptr);
        td_api_->Release();
        td_api_ = nullptr;
    }
    if (td_spi_) {
        delete td_spi_;
        td_spi_ = nullptr;
    }
}

bool CtpGatewayAdapter::should_reconnect_now() const {
    if (ready_.load(std::memory_order_acquire) || logged_in_.load(std::memory_order_acquire)) {
        return false;
    }
    if (config_.reconnect_time_ranges.empty()) {
        return true;
    }

    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    const tm* lt = std::localtime(&now);
    const int current_time = lt->tm_hour * 10000 + lt->tm_min * 100 + lt->tm_sec;

    for (const auto& range : config_.reconnect_time_ranges) {
        if (range.first <= range.second) {
            if (current_time >= range.first && current_time <= range.second) {
                return true;
            }
        } else if (current_time >= range.first || current_time <= range.second) {
            return true;
        }
    }
    return false;
}

bool CtpGatewayAdapter::submit_order(const OrderReq& req) {
    if (!td_api_ || !ready_.load(std::memory_order_acquire)) {
        publish_error(GatewayErrorCode::NotReady, "CTP gateway not ready");
        return false;
    }
    debug_log(config_, "submit order account_id=" + std::string(req.account_id) +
                           " symbol=" + req.symbol +
                           " order_ref=" + req.order_ref +
                           " direction=" + std::string(1, req.direction) +
                           " offset=" + std::string(1, req.offset_flag) +
                           " price=" + std::to_string(req.price) +
                           " volume=" + std::to_string(req.volume));
    CThostFtdcInputOrderField order = {0};
    std::strncpy(order.BrokerID, config_.broker_id.c_str(), sizeof(order.BrokerID) - 1);
    std::strncpy(order.InvestorID, config_.user_id.c_str(), sizeof(order.InvestorID) - 1);
    std::strncpy(order.InstrumentID, req.symbol, sizeof(order.InstrumentID) - 1);
    std::strncpy(order.OrderRef, req.order_ref, sizeof(order.OrderRef) - 1);
    order.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
    order.Direction = (req.direction == 'B') ? THOST_FTDC_D_Buy : THOST_FTDC_D_Sell;
    if (req.offset_flag == 'O') order.CombOffsetFlag[0] = THOST_FTDC_OF_Open;
    else if (req.offset_flag == 'T') order.CombOffsetFlag[0] = THOST_FTDC_OF_CloseToday;
    else order.CombOffsetFlag[0] = THOST_FTDC_OF_Close;
    order.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation;
    order.LimitPrice = req.price;
    order.VolumeTotalOriginal = req.volume;
    order.TimeCondition = THOST_FTDC_TC_GFD;
    order.VolumeCondition = THOST_FTDC_VC_AV;
    order.MinVolume = 1;
    order.ContingentCondition = THOST_FTDC_CC_Immediately;
    order.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;
    order.IsAutoSuspend = 0;

    int ret = td_api_->ReqOrderInsert(&order, req_id_++);
    if (ret != 0) {
        publish_error(GatewayErrorCode::ApiError, "ReqOrderInsert failed: " + std::to_string(ret));
        return false;
    }
    return true;
}

bool CtpGatewayAdapter::cancel_order(const CancelReq& req) {
    if (!td_api_ || !ready_.load(std::memory_order_acquire) || !td_spi_) {
        publish_error(GatewayErrorCode::NotReady, "CTP gateway not ready for cancel");
        return false;
    }
    debug_log(config_, "cancel order account_id=" + std::string(req.account_id) +
                           " symbol=" + req.symbol +
                           " order_ref=" + req.order_ref +
                           " client_id=" + std::to_string(req.client_id));
    CThostFtdcInputOrderActionField action = {0};
    std::strncpy(action.BrokerID, config_.broker_id.c_str(), sizeof(action.BrokerID) - 1);
    std::strncpy(action.InvestorID, config_.user_id.c_str(), sizeof(action.InvestorID) - 1);
    std::strncpy(action.InstrumentID, req.symbol, sizeof(action.InstrumentID) - 1);
    std::strncpy(action.OrderRef, req.order_ref, sizeof(action.OrderRef) - 1);
    action.FrontID = td_spi_->front_id_;
    action.SessionID = td_spi_->session_id_;
    action.ActionFlag = THOST_FTDC_AF_Delete;
    int ret = td_api_->ReqOrderAction(&action, req_id_++);
    if (ret != 0) {
        publish_error(GatewayErrorCode::ApiError, "ReqOrderAction failed: " + std::to_string(ret));
        return false;
    }
    return true;
}

bool CtpGatewayAdapter::query_position() {
    return query_position_internal(true);
}

bool CtpGatewayAdapter::query_account() {
    return query_account_internal(true);
}

bool CtpGatewayAdapter::query_open_orders() {
    return query_open_orders_internal(true);
}

void CtpGatewayAdapter::on_ready() {
    ready_.store(true, std::memory_order_release);
    debug_log(config_, "ready, query open orders / position / account");
    // query_open_orders_internal(false);
    query_position_internal(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    query_account_internal(false);
}

void CtpGatewayAdapter::publish_status(char status, const char* msg) {
    debug_log(config_, "status code=" + std::string(1, status) +
                           " msg=" + (msg ? std::string(msg) : std::string("")));
    if (!publisher_) return;
    GatewayEvent event{};
    copy_header(&event.header, EventType::ConnectionStatus, sizeof(ConnectionStatus), config_);
    std::strncpy(event.payload.conn.account_id, config_.account_id.c_str(),
                 sizeof(event.payload.conn.account_id) - 1);
    std::strncpy(event.payload.conn.source, "CTP_TD", sizeof(event.payload.conn.source) - 1);
    event.payload.conn.status = status;
    if (msg) {
        std::strncpy(event.payload.conn.msg, msg, sizeof(event.payload.conn.msg) - 1);
    }
    publisher_(event);
}

void CtpGatewayAdapter::publish_error(GatewayErrorCode code, const std::string& message) {
    std::cerr << "[CTPAdapter] error code=" << static_cast<int32_t>(code)
              << " msg=" << message << std::endl;
    if (!publisher_) return;
    GatewayEvent event{};
    copy_header(&event.header, EventType::GatewayError, sizeof(GatewayErrorPayload), config_);
    event.payload.error.code = static_cast<int32_t>(code);
    std::strncpy(event.payload.error.message, message.c_str(), sizeof(event.payload.error.message) - 1);
    publisher_(event);
}

void CtpGatewayAdapter::publish_order_rtn(const OrderRtn& rtn) {
    if (!publisher_) return;
    GatewayEvent event{};
    copy_header(&event.header, EventType::OrderRtn, sizeof(OrderRtn), config_);
    event.payload.order_rtn = rtn;
    publisher_(event);
}

void CtpGatewayAdapter::publish_trade_rtn(const TradeRtn& rtn) {
    if (!publisher_) return;
    GatewayEvent event{};
    copy_header(&event.header, EventType::TradeRtn, sizeof(TradeRtn), config_);
    event.payload.trade_rtn = rtn;
    publisher_(event);
}

void CtpGatewayAdapter::publish_position_rsp(const PositionDetail& pos) {
    if (!publisher_) return;
    GatewayEvent event{};
    copy_header(&event.header, EventType::PositionRsp, sizeof(PositionDetail), config_);
    event.payload.position = pos;
    publisher_(event);
}

void CtpGatewayAdapter::publish_account_rsp(const AccountDetail& acc) {
    if (!publisher_) return;
    GatewayEvent event{};
    copy_header(&event.header, EventType::AccountRsp, sizeof(AccountDetail), config_);
    event.payload.account = acc;
    publisher_(event);
}

void CtpGatewayAdapter::publish_cache_reset(uint32_t trading_day, const char* reason) {
    if (!publisher_) return;
    GatewayEvent event{};
    copy_header(&event.header, EventType::CacheReset, sizeof(CacheReset), config_);
    std::strncpy(event.payload.reset.account_id, config_.account_id.c_str(),
                 sizeof(event.payload.reset.account_id) - 1);
    event.payload.reset.trading_day = trading_day;
    event.payload.reset.reset_type = 0xFFFFFFFFu;
    if (reason) {
        std::strncpy(event.payload.reset.reason, reason, sizeof(event.payload.reset.reason) - 1);
    }
    publisher_(event);
}

bool CtpGatewayAdapter::query_account_internal(bool log_error) {
    if (!td_api_ || !ready_.load(std::memory_order_acquire)) {
        if (log_error) publish_error(GatewayErrorCode::NotReady, "query account before ready");
        return false;
    }
    CThostFtdcQryTradingAccountField req = {0};
    std::strncpy(req.BrokerID, config_.broker_id.c_str(), sizeof(req.BrokerID) - 1);
    std::strncpy(req.InvestorID, config_.user_id.c_str(), sizeof(req.InvestorID) - 1);
    debug_log(config_, "query account");
    int ret = td_api_->ReqQryTradingAccount(&req, req_id_++);
    if (ret != 0 && log_error) {
        publish_error(GatewayErrorCode::ApiError, "ReqQryTradingAccount failed: " + std::to_string(ret));
    }
    return ret == 0;
}

bool CtpGatewayAdapter::query_position_internal(bool log_error) {
    if (!td_api_ || !ready_.load(std::memory_order_acquire)) {
        if (log_error) publish_error(GatewayErrorCode::NotReady, "query position before ready");
        return false;
    }
    CThostFtdcQryInvestorPositionField req = {0};
    std::strncpy(req.BrokerID, config_.broker_id.c_str(), sizeof(req.BrokerID) - 1);
    std::strncpy(req.InvestorID, config_.user_id.c_str(), sizeof(req.InvestorID) - 1);
    debug_log(config_, "query position");
    int ret = td_api_->ReqQryInvestorPosition(&req, req_id_++);
    if (ret != 0 && log_error) {
        publish_error(GatewayErrorCode::ApiError, "ReqQryInvestorPosition failed: " + std::to_string(ret));
    }
    return ret == 0;
}

bool CtpGatewayAdapter::query_open_orders_internal(bool log_error) {
    if (!td_api_ || !ready_.load(std::memory_order_acquire)) {
        if (log_error) publish_error(GatewayErrorCode::NotReady, "query orders before ready");
        return false;
    }
    CThostFtdcQryOrderField req = {0};
    std::strncpy(req.BrokerID, config_.broker_id.c_str(), sizeof(req.BrokerID) - 1);
    std::strncpy(req.InvestorID, config_.user_id.c_str(), sizeof(req.InvestorID) - 1);
    debug_log(config_, "query open orders");
    int ret = td_api_->ReqQryOrder(&req, req_id_++);
    if (ret != 0 && log_error) {
        publish_error(GatewayErrorCode::ApiError, "ReqQryOrder failed: " + std::to_string(ret));
    }
    return ret == 0;
}

std::vector<std::pair<int, int>> CtpGatewayAdapter::parse_reconnect_times(const std::string& times_str) {
    std::vector<std::pair<int, int>> ranges;
    size_t begin = 0;
    while (begin < times_str.size()) {
        size_t end = times_str.find(',', begin);
        std::string range = times_str.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        size_t dash = range.find('-');
        if (dash != std::string::npos) {
            int start = parse_time_hhmmss(range.substr(0, dash));
            int stop = parse_time_hhmmss(range.substr(dash + 1));
            if (start >= 0 && stop >= 0) ranges.emplace_back(start, stop);
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return ranges;
}

int CtpGatewayAdapter::parse_time_hhmmss(const std::string& time_str) {
    if (time_str.size() < 8) return -1;
    try {
        int h = std::stoi(time_str.substr(0, 2));
        int m = std::stoi(time_str.substr(3, 2));
        int s = std::stoi(time_str.substr(6, 2));
        return h * 10000 + m * 100 + s;
    } catch (...) {
        return -1;
    }
}

void CtpGatewayAdapter::TraderSpi::OnFrontConnected() {
    debug_log(parent_->config_, "front connected, send login");
    parent_->publish_status('1', "Connected");
    CThostFtdcReqUserLoginField req = {0};
    std::strncpy(req.BrokerID, parent_->config_.broker_id.c_str(), sizeof(req.BrokerID) - 1);
    std::strncpy(req.UserID, parent_->config_.user_id.c_str(), sizeof(req.UserID) - 1);
    std::strncpy(req.Password, parent_->config_.password.c_str(), sizeof(req.Password) - 1);
    parent_->td_api_->ReqUserLogin(&req, parent_->req_id_++);
}

void CtpGatewayAdapter::TraderSpi::OnFrontDisconnected(int nReason) {
    debug_log(parent_->config_, "front disconnected reason=" + std::to_string(nReason));
    (void)nReason;
    parent_->logged_in_.store(false, std::memory_order_release);
    parent_->ready_.store(false, std::memory_order_release);
    parent_->publish_status('0', "Disconnected");
}

void CtpGatewayAdapter::TraderSpi::OnRspAuthenticate(CThostFtdcRspAuthenticateField*,
                                                     CThostFtdcRspInfoField* pRspInfo,
                                                     int,
                                                     bool) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        parent_->publish_error(GatewayErrorCode::ApiError, pRspInfo->ErrorMsg);
    }
}

void CtpGatewayAdapter::TraderSpi::OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
                                                  CThostFtdcRspInfoField* pRspInfo,
                                                  int,
                                                  bool) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        parent_->publish_error(GatewayErrorCode::ApiError, pRspInfo->ErrorMsg);
        parent_->publish_status('5', pRspInfo->ErrorMsg);
        return;
    }
    front_id_ = pRspUserLogin->FrontID;
    session_id_ = pRspUserLogin->SessionID;
    if (pRspUserLogin->TradingDay && std::strlen(pRspUserLogin->TradingDay) > 0) {
        parent_->ctp_trading_day_ = static_cast<uint32_t>(std::atoi(pRspUserLogin->TradingDay));
    }
    parent_->logged_in_.store(true, std::memory_order_release);
    debug_log(parent_->config_, "login ok front_id=" + std::to_string(front_id_) +
                                    " session_id=" + std::to_string(session_id_));
    std::string msg = "MaxOrderRef:" + std::string(pRspUserLogin->MaxOrderRef);
    parent_->publish_status('3', msg.c_str());

    CThostFtdcSettlementInfoConfirmField confirm = {0};
    std::strncpy(confirm.BrokerID, parent_->config_.broker_id.c_str(), sizeof(confirm.BrokerID) - 1);
    std::strncpy(confirm.InvestorID, parent_->config_.user_id.c_str(), sizeof(confirm.InvestorID) - 1);
    parent_->td_api_->ReqSettlementInfoConfirm(&confirm, parent_->req_id_++);
}

void CtpGatewayAdapter::TraderSpi::OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField*,
                                                              CThostFtdcRspInfoField* pRspInfo,
                                                              int,
                                                              bool) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        parent_->publish_error(GatewayErrorCode::ApiError, pRspInfo->ErrorMsg);
        return;
    }
    parent_->publish_cache_reset(parent_->ctp_trading_day_, "CTP_SETTLEMENT_CONFIRMED");
    parent_->publish_status('6', "Ready");
    parent_->on_ready();
}

void CtpGatewayAdapter::TraderSpi::OnRtnOrder(CThostFtdcOrderField* pOrder) {
    if (!pOrder) return;
    OrderRtn rtn{};
    std::strncpy(rtn.order_ref, pOrder->OrderRef, sizeof(rtn.order_ref) - 1);
    std::strncpy(rtn.symbol, pOrder->InstrumentID, sizeof(rtn.symbol) - 1);
    rtn.symbol_id = SymbolManager::instance().get_id(rtn.symbol);
    std::strncpy(rtn.account_id, parent_->config_.account_id.c_str(), sizeof(rtn.account_id) - 1);
    std::strncpy(rtn.exchange_id, pOrder->ExchangeID, sizeof(rtn.exchange_id) - 1);
    rtn.direction = (pOrder->Direction == THOST_FTDC_D_Buy) ? 'B' : 'S';
    if (pOrder->CombOffsetFlag[0] == THOST_FTDC_OF_Open) rtn.offset_flag = 'O';
    else if (pOrder->CombOffsetFlag[0] == THOST_FTDC_OF_CloseToday) rtn.offset_flag = 'T';
    else rtn.offset_flag = 'C';
    rtn.limit_price = pOrder->LimitPrice;
    rtn.volume_total = pOrder->VolumeTotalOriginal;
    rtn.volume_traded = pOrder->VolumeTraded;
    if (pOrder->OrderStatus == THOST_FTDC_OST_AllTraded) rtn.status = '0';
    else if (pOrder->OrderStatus == THOST_FTDC_OST_PartTradedQueueing) rtn.status = '1';
    else if (pOrder->OrderStatus == THOST_FTDC_OST_NoTradeQueueing) rtn.status = '3';
    else if (pOrder->OrderStatus == THOST_FTDC_OST_Canceled) rtn.status = '5';
    else rtn.status = 'a';
    std::strncpy(rtn.status_msg, pOrder->StatusMsg, sizeof(rtn.status_msg) - 1);
    std::strncpy(rtn.order_sys_id, pOrder->OrderSysID, sizeof(rtn.order_sys_id) - 1);
    debug_log(parent_->config_, "order rtn symbol=" + std::string(rtn.symbol) +
                                    " order_ref=" + rtn.order_ref +
                                    " order_sys_id=" + rtn.order_sys_id +
                                    " status=" + std::string(1, rtn.status) +
                                    " traded=" + std::to_string(rtn.volume_traded) +
                                    "/" + std::to_string(rtn.volume_total));
    parent_->publish_order_rtn(rtn);
}

void CtpGatewayAdapter::TraderSpi::OnRtnTrade(CThostFtdcTradeField* pTrade) {
    if (!pTrade) return;
    TradeRtn rtn{};
    std::strncpy(rtn.symbol, pTrade->InstrumentID, sizeof(rtn.symbol) - 1);
    rtn.symbol_id = SymbolManager::instance().get_id(rtn.symbol);
    std::strncpy(rtn.account_id, parent_->config_.account_id.c_str(), sizeof(rtn.account_id) - 1);
    std::strncpy(rtn.exchange_id, pTrade->ExchangeID, sizeof(rtn.exchange_id) - 1);
    rtn.direction = (pTrade->Direction == THOST_FTDC_D_Buy) ? 'B' : 'S';
    if (pTrade->OffsetFlag == THOST_FTDC_OF_Open) rtn.offset_flag = 'O';
    else if (pTrade->OffsetFlag == THOST_FTDC_OF_CloseToday) rtn.offset_flag = 'T';
    else rtn.offset_flag = 'C';
    rtn.price = pTrade->Price;
    rtn.volume = pTrade->Volume;
    std::strncpy(rtn.trade_id, pTrade->TradeID, sizeof(rtn.trade_id) - 1);
    std::strncpy(rtn.order_ref, pTrade->OrderRef, sizeof(rtn.order_ref) - 1);
    std::strncpy(rtn.order_sys_id, pTrade->OrderSysID, sizeof(rtn.order_sys_id) - 1);
    debug_log(parent_->config_, "trade rtn symbol=" + std::string(rtn.symbol) +
                                    " trade_id=" + rtn.trade_id +
                                    " order_ref=" + rtn.order_ref +
                                    " price=" + std::to_string(rtn.price) +
                                    " volume=" + std::to_string(rtn.volume));
    parent_->publish_trade_rtn(rtn);
}

void CtpGatewayAdapter::TraderSpi::OnRspOrderInsert(CThostFtdcInputOrderField* pInputOrder,
                                                    CThostFtdcRspInfoField* pRspInfo,
                                                    int,
                                                    bool) {
    if (!pRspInfo || pRspInfo->ErrorID == 0 || !pInputOrder) return;
    OrderRtn rtn{};
    std::strncpy(rtn.order_ref, pInputOrder->OrderRef, sizeof(rtn.order_ref) - 1);
    std::strncpy(rtn.symbol, pInputOrder->InstrumentID, sizeof(rtn.symbol) - 1);
    std::strncpy(rtn.account_id, parent_->config_.account_id.c_str(), sizeof(rtn.account_id) - 1);
    rtn.status = '5';
    std::strncpy(rtn.status_msg, pRspInfo->ErrorMsg, sizeof(rtn.status_msg) - 1);
    parent_->publish_order_rtn(rtn);
}

void CtpGatewayAdapter::TraderSpi::OnErrRtnOrderInsert(CThostFtdcInputOrderField* pInputOrder,
                                                       CThostFtdcRspInfoField* pRspInfo) {
    if (!pInputOrder || !pRspInfo) return;
    OrderRtn rtn{};
    std::strncpy(rtn.order_ref, pInputOrder->OrderRef, sizeof(rtn.order_ref) - 1);
    std::strncpy(rtn.symbol, pInputOrder->InstrumentID, sizeof(rtn.symbol) - 1);
    std::strncpy(rtn.account_id, parent_->config_.account_id.c_str(), sizeof(rtn.account_id) - 1);
    rtn.status = '5';
    std::strncpy(rtn.status_msg, pRspInfo->ErrorMsg, sizeof(rtn.status_msg) - 1);
    parent_->publish_order_rtn(rtn);
}

void CtpGatewayAdapter::TraderSpi::OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* pInvestorPosition,
                                                            CThostFtdcRspInfoField* pRspInfo,
                                                            int,
                                                            bool) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        parent_->publish_error(GatewayErrorCode::ApiError, pRspInfo->ErrorMsg);
        return;
    }
    if (!pInvestorPosition) return;
    PositionDetail pos{};
    std::strncpy(pos.symbol, pInvestorPosition->InstrumentID, sizeof(pos.symbol) - 1);
    pos.symbol_id = SymbolManager::instance().get_id(pos.symbol);
    std::strncpy(pos.account_id, parent_->config_.account_id.c_str(), sizeof(pos.account_id) - 1);
    std::strncpy(pos.exchange_id, pInvestorPosition->ExchangeID, sizeof(pos.exchange_id) - 1);
    pos.direction = pInvestorPosition->PosiDirection;
    pos.position_date = pInvestorPosition->PositionDate;
    if (pInvestorPosition->PosiDirection == THOST_FTDC_PD_Long ||
        pInvestorPosition->PosiDirection == THOST_FTDC_PD_Net) {
        pos.long_td = pInvestorPosition->TodayPosition;
        pos.long_yd = pInvestorPosition->Position - pInvestorPosition->TodayPosition;
    } else if (pInvestorPosition->PosiDirection == THOST_FTDC_PD_Short) {
        pos.short_td = pInvestorPosition->TodayPosition;
        pos.short_yd = pInvestorPosition->Position - pInvestorPosition->TodayPosition;
    }
    pos.net_pnl = pInvestorPosition->PositionProfit;
    parent_->publish_position_rsp(pos);
}

void CtpGatewayAdapter::TraderSpi::OnRspQryTradingAccount(CThostFtdcTradingAccountField* pTradingAccount,
                                                          CThostFtdcRspInfoField* pRspInfo,
                                                          int,
                                                          bool) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        parent_->publish_error(GatewayErrorCode::ApiError, pRspInfo->ErrorMsg);
        return;
    }
    if (!pTradingAccount) return;
    AccountDetail acc{};
    std::strncpy(acc.broker_id, pTradingAccount->BrokerID, sizeof(acc.broker_id) - 1);
    std::strncpy(acc.account_id, pTradingAccount->AccountID, sizeof(acc.account_id) - 1);
    acc.balance = pTradingAccount->Balance;
    acc.available = pTradingAccount->Available;
    acc.margin = pTradingAccount->CurrMargin;
    acc.close_pnl = pTradingAccount->CloseProfit;
    acc.position_pnl = pTradingAccount->PositionProfit;
    parent_->publish_account_rsp(acc);
}

void CtpGatewayAdapter::TraderSpi::OnRspQryOrder(CThostFtdcOrderField* pOrder,
                                                 CThostFtdcRspInfoField* pRspInfo,
                                                 int,
                                                 bool) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        parent_->publish_error(GatewayErrorCode::ApiError, pRspInfo->ErrorMsg);
        return;
    }
    if (!pOrder) return;
    OnRtnOrder(pOrder);
}

}  // namespace trade_gateway
