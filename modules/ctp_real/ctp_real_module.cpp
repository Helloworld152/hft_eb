#include "../../include/framework.h"
#include "ThostFtdcTraderApi.h"
#include <thread>
#include <chrono>
#include <atomic>
#include <cstring>
#include <iostream>
#include <mutex>

class CtpRealModule : public IModule {
public:
    CtpRealModule();
    virtual ~CtpRealModule();

    void init(EventBus* bus, const ConfigMap& config) override;
    void start() override;
    void stop() override;

    // Internal helper to send order
    void send_order(const OrderReq* req);

private:
    // Config
    std::string td_front_;
    std::string broker_id_;
    std::string user_id_;
    std::string password_;
    std::string app_id_;
    std::string auth_code_;

    EventBus* bus_ = nullptr;
    
    // CTP Trader API
    CThostFtdcTraderApi* td_api_ = nullptr;

    // Request ID counter
    std::atomic<int> req_id_{0};

    class TraderSpi : public CThostFtdcTraderSpi {
    public:
        TraderSpi(CtpRealModule* parent) : parent_(parent) {}

        void OnFrontConnected() override;
        void OnFrontDisconnected(int nReason) override;
        void OnRspAuthenticate(CThostFtdcRspAuthenticateField *pRspAuthenticateField, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;
        void OnRspUserLogin(CThostFtdcRspUserLoginField *pRspUserLogin, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;
        void OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField *pSettlementInfoConfirm, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;
        void OnRtnOrder(CThostFtdcOrderField *pOrder) override;
        void OnRtnTrade(CThostFtdcTradeField *pTrade) override;
        void OnErrRtnOrderInsert(CThostFtdcInputOrderField *pInputOrder, CThostFtdcRspInfoField *pRspInfo) override;

    private:
        CtpRealModule* parent_;
    };

    friend class TraderSpi;
    TraderSpi* td_spi_ = nullptr;
    
    std::atomic<bool> logged_in_{false};
};

// ==========================================================
// Implementation
// ==========================================================

CtpRealModule::CtpRealModule() {}

CtpRealModule::~CtpRealModule() {
    stop();
}

void CtpRealModule::init(EventBus* bus, const ConfigMap& config) {
    bus_ = bus;

    if (config.count("td_front")) td_front_ = config.at("td_front");
    if (config.count("broker_id")) broker_id_ = config.at("broker_id");
    if (config.count("user_id")) user_id_ = config.at("user_id");
    if (config.count("password")) password_ = config.at("password");
    if (config.count("app_id")) app_id_ = config.at("app_id");
    if (config.count("auth_code")) auth_code_ = config.at("auth_code");

    std::cout << "[CTP-Trade] Initialized for Broker=" << broker_id_ << ", User=" << user_id_ << std::endl;

    // Subscribe to Authorized Order Commands (from Risk/Manual)
    bus_->subscribe(EVENT_ORDER_SEND, [this](void* data) {
        this->send_order(static_cast<OrderReq*>(data));
    });
}

void CtpRealModule::start() {
    if (!td_front_.empty()) {
        std::cout << "[CTP-Trade] Connecting to TD Front: " << td_front_ << std::endl;
        td_api_ = CThostFtdcTraderApi::CreateFtdcTraderApi("./flow_log/td_");
        td_spi_ = new TraderSpi(this);
        td_api_->RegisterSpi(td_spi_);
        td_api_->RegisterFront(const_cast<char*>(td_front_.c_str()));
        td_api_->SubscribePublicTopic(THOST_TERT_QUICK);
        td_api_->SubscribePrivateTopic(THOST_TERT_QUICK);
        td_api_->Init();
    }
}

void CtpRealModule::stop() {
    if (td_api_) {
        td_api_->RegisterSpi(nullptr);
        td_api_->Release();
        td_api_ = nullptr;
    }
    if (td_spi_) { delete td_spi_; td_spi_ = nullptr; }
    logged_in_ = false;
}

void CtpRealModule::send_order(const OrderReq* req) {
    if (!td_api_ || !logged_in_.load()) {
        std::cerr << "[CTP-Trade] Error: Trader API not ready or not logged in." << std::endl;
        return;
    }

    CThostFtdcInputOrderField order = {0};
    strncpy(order.BrokerID, broker_id_.c_str(), sizeof(order.BrokerID) - 1);
    strncpy(order.InvestorID, user_id_.c_str(), sizeof(order.InvestorID) - 1);
    strncpy(order.InstrumentID, req->symbol, sizeof(order.InstrumentID) - 1);
    
    snprintf(order.OrderRef, sizeof(order.OrderRef), "%d", req_id_++);
    
    order.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
    order.Direction = (req->direction == 'B') ? THOST_FTDC_D_Buy : THOST_FTDC_D_Sell;
    
    // Mapping OffsetFlag
    if (req->offset_flag == 'O') order.CombOffsetFlag[0] = THOST_FTDC_OF_Open;
    else if (req->offset_flag == 'T') order.CombOffsetFlag[0] = THOST_FTDC_OF_CloseToday;
    else order.CombOffsetFlag[0] = THOST_FTDC_OF_Close;

    order.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation;
    
    order.LimitPrice = req->price;
    order.VolumeTotalOriginal = req->volume;
    
    order.TimeCondition = THOST_FTDC_TC_GFD;
    order.VolumeCondition = THOST_FTDC_VC_AV;
    order.MinVolume = 1;
    order.ContingentCondition = THOST_FTDC_CC_Immediately;
    order.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;
    order.IsAutoSuspend = 0;

    int ret = td_api_->ReqOrderInsert(&order, req_id_++);
    if (ret != 0) {
        std::cerr << "[CTP-Trade] Order Insert Failed: " << ret << std::endl;
    } else {
        std::cout << "[CTP-Trade] Order Sent: " << req->symbol << " " << req->direction 
                  << " @ " << req->price << " (Ref=" << order.OrderRef << ")" << std::endl;
    }
}

// ==========================================================
// TraderSpi Implementation
// ==========================================================

void CtpRealModule::TraderSpi::OnFrontConnected() {
    std::cout << "[CTP-Trade] Front Connected. Authenticating..." << std::endl;
    
    CThostFtdcReqAuthenticateField authReq = {0};
    strncpy(authReq.BrokerID, parent_->broker_id_.c_str(), sizeof(authReq.BrokerID) - 1);
    strncpy(authReq.UserID, parent_->user_id_.c_str(), sizeof(authReq.UserID) - 1);
    strncpy(authReq.AppID, parent_->app_id_.c_str(), sizeof(authReq.AppID) - 1);
    strncpy(authReq.AuthCode, parent_->auth_code_.c_str(), sizeof(authReq.AuthCode) - 1);
    
    parent_->td_api_->ReqAuthenticate(&authReq, parent_->req_id_++);
}

void CtpRealModule::TraderSpi::OnFrontDisconnected(int nReason) {
    std::cout << "[CTP-Trade] Front Disconnected. Reason: " << nReason << std::endl;
    parent_->logged_in_ = false;
}

void CtpRealModule::TraderSpi::OnRspAuthenticate(CThostFtdcRspAuthenticateField *pRspAuthenticateField, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        std::cerr << "[CTP-Trade] Authenticate Failed: " << pRspInfo->ErrorMsg << std::endl;
        return;
    }
    std::cout << "[CTP-Trade] Authenticated. Logging in..." << std::endl;
    
    CThostFtdcReqUserLoginField req = {0};
    strncpy(req.BrokerID, parent_->broker_id_.c_str(), sizeof(req.BrokerID) - 1);
    strncpy(req.UserID, parent_->user_id_.c_str(), sizeof(req.UserID) - 1);
    strncpy(req.Password, parent_->password_.c_str(), sizeof(req.Password) - 1);
    parent_->td_api_->ReqUserLogin(&req, parent_->req_id_++);
}

void CtpRealModule::TraderSpi::OnRspUserLogin(CThostFtdcRspUserLoginField *pRspUserLogin, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        std::cerr << "[CTP-Trade] Login Failed: " << pRspInfo->ErrorMsg << std::endl;
        return;
    }
    std::cout << "[CTP-Trade] Login Success. TradingDay: " << pRspUserLogin->TradingDay 
              << ". Confirming Settlement..." << std::endl;
    
    CThostFtdcSettlementInfoConfirmField confirm = {0};
    strncpy(confirm.BrokerID, parent_->broker_id_.c_str(), sizeof(confirm.BrokerID) - 1);
    strncpy(confirm.InvestorID, parent_->user_id_.c_str(), sizeof(confirm.InvestorID) - 1);
    parent_->td_api_->ReqSettlementInfoConfirm(&confirm, parent_->req_id_++);
}

void CtpRealModule::TraderSpi::OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField *pSettlementInfoConfirm, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) {
     if (pRspInfo && pRspInfo->ErrorID != 0) {
        std::cerr << "[CTP-Trade] Settlement Confirm Failed: " << pRspInfo->ErrorMsg << std::endl;
        return;
    }
    std::cout << "[CTP-Trade] Settlement Confirmed. System Ready." << std::endl;
    parent_->logged_in_ = true;
}

void CtpRealModule::TraderSpi::OnRtnOrder(CThostFtdcOrderField *pOrder) {
    if (!pOrder) return;
    std::cout << "[CTP-Trade] Order Update: " << pOrder->InstrumentID << " Ref=" << pOrder->OrderRef 
              << " Status=" << pOrder->OrderStatus << " Msg=" << pOrder->StatusMsg << std::endl;

    OrderRtn rtn;
    strncpy(rtn.order_ref, pOrder->OrderRef, 12);
    strncpy(rtn.symbol, pOrder->InstrumentID, 31);
    rtn.direction = (pOrder->Direction == THOST_FTDC_D_Buy) ? 'B' : 'S';
    
    if (pOrder->CombOffsetFlag[0] == THOST_FTDC_OF_Open) rtn.offset_flag = 'O';
    else if (pOrder->CombOffsetFlag[0] == THOST_FTDC_OF_CloseToday) rtn.offset_flag = 'T';
    else rtn.offset_flag = 'C';

    rtn.limit_price = pOrder->LimitPrice;
    rtn.volume_total = pOrder->VolumeTotalOriginal;
    rtn.volume_traded = pOrder->VolumeTraded;
    
    // Map CTP status to our protocol
    if (pOrder->OrderStatus == THOST_FTDC_OST_AllTraded) rtn.status = '0';
    else if (pOrder->OrderStatus == THOST_FTDC_OST_PartTradedQueueing) rtn.status = '1';
    else if (pOrder->OrderStatus == THOST_FTDC_OST_NoTradeQueueing) rtn.status = '3';
    else if (pOrder->OrderStatus == THOST_FTDC_OST_Canceled) rtn.status = '5';
    else rtn.status = 'a'; // Unknown/Other

    strncpy(rtn.status_msg, pOrder->StatusMsg, 80);

    parent_->bus_->publish(EVENT_RTN_ORDER, &rtn);
}

void CtpRealModule::TraderSpi::OnRtnTrade(CThostFtdcTradeField *pTrade) {
    if (!pTrade) return;
    std::cout << "[CTP-Trade] EXECUTION: " << pTrade->InstrumentID << " " << pTrade->Direction 
              << " @ " << pTrade->Price << " Vol=" << pTrade->Volume << std::endl;

    TradeRtn rtn;
    strncpy(rtn.symbol, pTrade->InstrumentID, 31);
    rtn.direction = (pTrade->Direction == THOST_FTDC_D_Buy) ? 'B' : 'S';
    
    if (pTrade->OffsetFlag == THOST_FTDC_OF_Open) rtn.offset_flag = 'O';
    else if (pTrade->OffsetFlag == THOST_FTDC_OF_CloseToday) rtn.offset_flag = 'T';
    else rtn.offset_flag = 'C';

    rtn.price = pTrade->Price;
    rtn.volume = pTrade->Volume;
    strncpy(rtn.trade_id, pTrade->TradeID, 20);
    strncpy(rtn.order_ref, pTrade->OrderRef, 12);

    parent_->bus_->publish(EVENT_RTN_TRADE, &rtn);
}

void CtpRealModule::TraderSpi::OnErrRtnOrderInsert(CThostFtdcInputOrderField *pInputOrder, CThostFtdcRspInfoField *pRspInfo) {
    std::cerr << "[CTP-Trade] Order Insert Error: " << (pRspInfo ? pRspInfo->ErrorMsg : "Unknown") << std::endl;
}

EXPORT_MODULE(CtpRealModule)