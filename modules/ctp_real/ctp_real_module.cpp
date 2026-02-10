#include "../../include/framework.h"
#include "../../core/include/symbol_manager.h"
#include "../../core/include/order_manager.h"
#include "ThostFtdcTraderApi.h"
#include <thread>
#include <chrono>
#include <atomic>
#include <cstring>
#include <iostream>
#include <vector>
#include <sstream>

class CtpRealModule : public IModule {
public:
    CtpRealModule();
    virtual ~CtpRealModule();

    void init(EventBus* bus, const ConfigMap& config, ITimerService* timer_svc = nullptr) override;
    void start() override;
    void stop() override;

    // Internal helper to send order
    void send_order(const OrderReq* req);
    void cancel_order(const CancelReq* req);
    
    // Helper to publish connection status
    void publish_status(char status, const char* msg) {
        if (!bus_) return;
        ConnectionStatus cs;
        memset(&cs, 0, sizeof(cs));
        strncpy(cs.account_id, user_id_.c_str(), 15);
        strncpy(cs.source, "CTP_TD", 15);
        cs.status = status;
        if (msg) strncpy(cs.msg, msg, 63);
        bus_->publish(EVENT_CONN_STATUS, &cs);
    }

private:
    // Config
    std::string td_front_;
    std::string broker_id_;
    std::string user_id_;
    std::string password_;
    std::string app_id_;
    std::string auth_code_;
    // 重连时间段配置：支持多组，格式 "HH:MM:SS-HH:MM:SS,HH:MM:SS-HH:MM:SS"
    // 例如: "09:00:00-15:00:00,21:00:00-02:30:00" (白天+夜盘)
    std::vector<std::pair<int, int>> reconnect_time_ranges_;  // 存储时间段对 (start_time, end_time) 整数格式 HHMMSS
    int reconnect_delay_sec_ = 3;  // 重连延迟秒数

    EventBus* bus_ = nullptr;
    ITimerService* timer_svc_ = nullptr;

    // CTP Trader API
    CThostFtdcTraderApi* td_api_ = nullptr;

    // Request ID counter
    std::atomic<int> req_id_{0};
    
    // Helper: 解析重连时间段配置字符串
    void parse_reconnect_times(const std::string& times_str);
    // Helper: 检查当前时间是否在重连时间范围内
    bool is_in_reconnect_time() const;
    // Helper: 守护线程主循环
    void monitor_loop();
    // Helper: 执行真正的 API 重启逻辑
    void do_connect();

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
        void OnRspOrderInsert(CThostFtdcInputOrderField *pInputOrder, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;
        void OnErrRtnOrderInsert(CThostFtdcInputOrderField *pInputOrder, CThostFtdcRspInfoField *pRspInfo) override;
        void OnRspQryInvestorPosition(CThostFtdcInvestorPositionField *pInvestorPosition, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;
        void OnRspQryTradingAccount(CThostFtdcTradingAccountField *pTradingAccount, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override;

        int front_id_ = 0;
        int session_id_ = 0;

    private:
        CtpRealModule* parent_;
    };

    friend class TraderSpi;
    TraderSpi* td_spi_ = nullptr;
    
    std::atomic<bool> logged_in_{false};
    uint32_t ctp_trading_day_ = 0;
    bool debug_ = false;
};

// ==========================================================
// Implementation
// ==========================================================

CtpRealModule::CtpRealModule() {}

CtpRealModule::~CtpRealModule() {
    stop();
}

void CtpRealModule::init(EventBus* bus, const ConfigMap& config, ITimerService* timer_svc) {
    bus_ = bus;
    timer_svc_ = timer_svc;

    if (config.count("td_front")) td_front_ = config.at("td_front");
    if (config.count("broker_id")) broker_id_ = config.at("broker_id");
    if (config.count("user_id")) user_id_ = config.at("user_id");
    if (config.count("password")) password_ = config.at("password");
    if (config.count("app_id")) app_id_ = config.at("app_id");
    if (config.count("auth_code")) auth_code_ = config.at("auth_code");
    
    // 解析重连时间段配置
    // 支持格式1: reconnect_times="09:00:00-15:00:00,21:00:00-02:30:00" (多组，逗号分隔)
    // 支持格式2: reconnect_start + reconnect_end (兼容旧配置，单组)
    if (config.count("reconnect_times")) {
        parse_reconnect_times(config.at("reconnect_times"));
    } else if (config.count("reconnect_start") && config.count("reconnect_end")) {
        // 兼容旧配置格式
        std::string start = config.at("reconnect_start");
        std::string end = config.at("reconnect_end");
        parse_reconnect_times(start + "-" + end);
    }
    
    if (config.count("reconnect_delay")) {
        reconnect_delay_sec_ = std::stoi(config.at("reconnect_delay"));
        if (reconnect_delay_sec_ < 1) reconnect_delay_sec_ = 1;
    }
    
    if (config.count("debug")) {
        std::string val = config.at("debug");
        debug_ = (val == "true" || val == "1");
    }

    std::cout << "[CTP-Trade] Initialized for Broker=" << broker_id_ << ", User=" << user_id_ 
              << ", Debug=" << (debug_ ? "ON" : "OFF") << std::endl;

    // Subscribe to Authorized Order Commands (from Risk/Manual)
    bus_->subscribe(EVENT_ORDER_SEND, [this](void* data) {
        this->send_order(static_cast<OrderReq*>(data));
    });

    bus_->subscribe(EVENT_CANCEL_SEND, [this](void* data) {
        this->cancel_order(static_cast<CancelReq*>(data));
    });

    bus_->subscribe(EVENT_QRY_ACC, [this](void* data) {
        if (!td_api_ || !logged_in_.load()) return;
        CThostFtdcQryTradingAccountField req = {0};
        strncpy(req.BrokerID, broker_id_.c_str(), sizeof(req.BrokerID)-1);
        strncpy(req.InvestorID, user_id_.c_str(), sizeof(req.InvestorID)-1);
        int ret = td_api_->ReqQryTradingAccount(&req, req_id_++);
        if (debug_) {
            std::cout << "[CTP-Trade] 请求查询资金, ret=" << ret << std::endl;
        }
    });

    // Subscribe to Query Commands (from Monitor)
    bus_->subscribe(EVENT_QRY_POS, [this](void* data) {
        if (!td_api_ || !logged_in_.load()) return;
        
        // Rate Limit Check (Optional)
        // ...

        CThostFtdcQryInvestorPositionField req = {0};
        strncpy(req.BrokerID, broker_id_.c_str(), sizeof(req.BrokerID)-1);
        strncpy(req.InvestorID, user_id_.c_str(), sizeof(req.InvestorID)-1);
        td_api_->ReqQryInvestorPosition(&req, req_id_++);
        
        // Also query Account?
        // CThostFtdcQryTradingAccountField accReq = {0};
        // ...
    });
}

void CtpRealModule::start() {
    if (td_front_.empty()) return;

    std::cout << "[CTP-Trade] Module started. Using engine timer for reconnect." << std::endl;
    if (timer_svc_) {
        timer_svc_->add_timer(reconnect_delay_sec_, [this]() {
            if (!logged_in_.load() && is_in_reconnect_time()) {
                if (debug_) std::cout << "[CTP-Trade] [Timer] 满足重连周期，尝试 do_connect" << std::endl;
                do_connect();
            }
        });
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
    publish_status('0', "Stopped");
}

void CtpRealModule::send_order(const OrderReq* req) {
    if (strlen(req->account_id) > 0 && strcmp(req->account_id, user_id_.c_str()) != 0) {
        return; // Not for this account
    }

    if (!td_api_ || !logged_in_.load()) {
        std::cerr << "[CTP-Trade] Error: Trader API not ready or not logged in." << std::endl;
        return;
    }

    CThostFtdcInputOrderField order = {0};
    strncpy(order.BrokerID, broker_id_.c_str(), sizeof(order.BrokerID) - 1);
    strncpy(order.InvestorID, user_id_.c_str(), sizeof(order.InvestorID) - 1);
    strncpy(order.InstrumentID, req->symbol, sizeof(order.InstrumentID) - 1);
    
    // 直接使用 OrderManager 分配好的映射 ID
    strncpy(order.OrderRef, req->order_ref, sizeof(order.OrderRef) - 1);
    
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
    } else if (debug_) {
        std::cout << "[CTP-Trade] Order Sent: " << req->symbol << " " << req->direction 
                  << " @ " << req->price << " (Ref=" << order.OrderRef << ")" << std::endl;
    }
}

void CtpRealModule::cancel_order(const CancelReq* req) {
    if (debug_) {
        std::cout << "[CTP-Trade] [" << user_id_ << "] 收到撤单请求: Acc=" << req->account_id 
                  << " Symbol=" << req->symbol << " Ref=" << req->order_ref << std::endl;
    }

    if (strlen(req->account_id) > 0 && strcmp(req->account_id, user_id_.c_str()) != 0) {
        if (debug_) {
            std::cout << "[CTP-Trade] [" << user_id_ << "] 忽略撤单请求: 目标账户不匹配 (" 
                      << req->account_id << " != " << user_id_ << ")" << std::endl;
        }
        return;
    }

    if (!td_api_ || !logged_in_.load()) {
        std::cerr << "[CTP-Trade] 撤单失败: 未登录或 API 未就绪" << std::endl;
        return;
    }

    CThostFtdcInputOrderActionField action = {0};
    strncpy(action.BrokerID, broker_id_.c_str(), sizeof(action.BrokerID) - 1);
    strncpy(action.InvestorID, user_id_.c_str(), sizeof(action.InvestorID) - 1);
    strncpy(action.InstrumentID, req->symbol, sizeof(action.InstrumentID) - 1);
    strncpy(action.OrderRef, req->order_ref, sizeof(action.OrderRef) - 1);
    
    // 关键：CTP 撤单通常需要 FrontID 和 SessionID，或者 ExchangeID + OrderSysID
    // 仅凭 OrderRef 撤单只能撤销“尚未报入交易所但在本地排队”的单，或者当前会话的单
    // 这是一个潜在的隐患点，如果需要更稳健的撤单，需要维护 OrderRef -> (FrontID, SessionID) 的映射
    action.FrontID = td_spi_->front_id_;
    action.SessionID = td_spi_->session_id_;
    
    action.ActionFlag = THOST_FTDC_AF_Delete;

    int ret = td_api_->ReqOrderAction(&action, req_id_++);
    if (ret != 0) {
        std::cerr << "[CTP-Trade] 撤单请求发送失败: Error=" << ret << " Ref=" << req->order_ref << std::endl;
    } else if (debug_) {
        std::cout << "[CTP-Trade] 撤单请求已发送: Ref=" << req->order_ref << std::endl;
    }
}

// ==========================================================
// TraderSpi Implementation
// ==========================================================

void CtpRealModule::TraderSpi::OnFrontConnected() {
    std::cout << "[CTP-Trade] Front Connected. Skipping Auth, Logging in..." << std::endl;
    parent_->publish_status('1', "Connected");
    
    // Skip Authentication for environments that don't need it
    // Or check if auth_code is empty
    /*
    CThostFtdcReqAuthenticateField authReq = {0};
    strncpy(authReq.BrokerID, parent_->broker_id_.c_str(), sizeof(authReq.BrokerID) - 1);
    strncpy(authReq.UserID, parent_->user_id_.c_str(), sizeof(authReq.UserID) - 1);
    strncpy(authReq.AppID, parent_->app_id_.c_str(), sizeof(authReq.AppID) - 1);
    strncpy(authReq.AuthCode, parent_->auth_code_.c_str(), sizeof(authReq.AuthCode) - 1);
    
    parent_->td_api_->ReqAuthenticate(&authReq, parent_->req_id_++);
    */

    // Directly Login
    CThostFtdcReqUserLoginField req = {0};
    strncpy(req.BrokerID, parent_->broker_id_.c_str(), sizeof(req.BrokerID) - 1);
    strncpy(req.UserID, parent_->user_id_.c_str(), sizeof(req.UserID) - 1);
    strncpy(req.Password, parent_->password_.c_str(), sizeof(req.Password) - 1);
    parent_->td_api_->ReqUserLogin(&req, parent_->req_id_++);
}

void CtpRealModule::TraderSpi::OnFrontDisconnected(int nReason) {
    std::cerr << "[CTP-Trade] Front Disconnected. Reason: " << nReason << std::endl;
    parent_->logged_in_ = false;
    parent_->publish_status('0', "Disconnected");
    
    // Monitor thread will handle reconnection logic automatically
}

void CtpRealModule::TraderSpi::OnRspAuthenticate(CThostFtdcRspAuthenticateField *pRspAuthenticateField, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        std::cerr << "[CTP-Trade] Authenticate Failed: " << pRspInfo->ErrorMsg << std::endl;
        parent_->publish_status('4', pRspInfo->ErrorMsg);
        return;
    }
    if (parent_->debug_) std::cout << "[CTP-Trade] Authenticated. Logging in..." << std::endl;
    parent_->publish_status('2', "Authenticated");
    
    CThostFtdcReqUserLoginField req = {0};
    strncpy(req.BrokerID, parent_->broker_id_.c_str(), sizeof(req.BrokerID) - 1);
    strncpy(req.UserID, parent_->user_id_.c_str(), sizeof(req.UserID) - 1);
    strncpy(req.Password, parent_->password_.c_str(), sizeof(req.Password) - 1);
    parent_->td_api_->ReqUserLogin(&req, parent_->req_id_++);
}

void CtpRealModule::TraderSpi::OnRspUserLogin(CThostFtdcRspUserLoginField *pRspUserLogin, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        std::cerr << "[CTP-Trade] Login Failed: " << pRspInfo->ErrorMsg << std::endl;
        parent_->publish_status('5', pRspInfo->ErrorMsg);
        return;
    }
    
    // 保存会话信息，用于后续撤单
    this->front_id_ = pRspUserLogin->FrontID;
    this->session_id_ = pRspUserLogin->SessionID;

    if (pRspUserLogin->TradingDay && strlen(pRspUserLogin->TradingDay) > 0) {
        parent_->ctp_trading_day_ = std::atoi(pRspUserLogin->TradingDay);
    }

    std::string msg = "MaxOrderRef:" + std::string(pRspUserLogin->MaxOrderRef);
    std::cout << "[CTP-Trade] Login Success. TradingDay: " << pRspUserLogin->TradingDay 
              << ", FrontID=" << front_id_ << ", SessionID=" << session_id_
              << ", " << msg << ". Confirming Settlement..." << std::endl;
    parent_->publish_status('3', msg.c_str());
    
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
    std::cout << "[CTP-Trade] Settlement Confirmed. Ready for commands." << std::endl;
    parent_->logged_in_ = true;

    // 触发重置信号
    CacheReset cr = {0};
    strncpy(cr.account_id, parent_->user_id_.c_str(), 15);
    cr.trading_day = parent_->ctp_trading_day_;
    cr.reset_type = 0xFFFFFFFF; // 重置所有
    strncpy(cr.reason, "CTP_SETTLEMENT_CONFIRMED", 63);
    parent_->bus_->publish(EVENT_CACHE_RESET, &cr);
}

void CtpRealModule::TraderSpi::OnRspQryInvestorPosition(CThostFtdcInvestorPositionField *pInvestorPosition, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        std::cerr << "[CTP-Trade] 查询持仓失败: " << pRspInfo->ErrorMsg << std::endl;
        return;
    }
    if (!pInvestorPosition) return;
    
    if (parent_->debug_) {
        std::cout << "[CTP-Trade] [POS_RAW] User=" << parent_->user_id_ 
                  << " Inst=" << pInvestorPosition->InstrumentID 
                  << " Dir=" << pInvestorPosition->PosiDirection
                  << " Vol=" << pInvestorPosition->Position
                  << " Td=" << pInvestorPosition->TodayPosition 
                  << " Cost=" << pInvestorPosition->PositionCost << std::endl;
    }
    
    PositionDetail pos = {0};
    strncpy(pos.symbol, pInvestorPosition->InstrumentID, 31);
    pos.symbol_id = SymbolManager::instance().get_id(pos.symbol);
    strncpy(pos.account_id, parent_->user_id_.c_str(), 15);
    strncpy(pos.exchange_id, pInvestorPosition->ExchangeID, 8); // 透传交易所ID
    pos.direction = pInvestorPosition->PosiDirection; // 记录原始方向
    pos.position_date = pInvestorPosition->PositionDate; // 记录持仓日期标识
    
    // CTP Position: 2=Buy(Long), 3=Sell(Short), 1=Net
    if (pInvestorPosition->PosiDirection == THOST_FTDC_PD_Long || pInvestorPosition->PosiDirection == THOST_FTDC_PD_Net) {
        pos.long_td = pInvestorPosition->TodayPosition;
        pos.long_yd = pInvestorPosition->Position - pInvestorPosition->TodayPosition;
        // 不计算持仓均价，保持为0
    } else if (pInvestorPosition->PosiDirection == THOST_FTDC_PD_Short) {
        pos.short_td = pInvestorPosition->TodayPosition;
        pos.short_yd = pInvestorPosition->Position - pInvestorPosition->TodayPosition;
        // 不计算持仓均价，保持为0
    }

    pos.net_pnl = pInvestorPosition->PositionProfit;

    // [CRITICAL] 发送原始回报给 PositionModule，而不是直接广播 POS_UPDATE
    parent_->bus_->publish(EVENT_RSP_POS, &pos);
}

void CtpRealModule::TraderSpi::OnRspQryTradingAccount(CThostFtdcTradingAccountField *pTradingAccount, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        std::cerr << "[CTP-Trade] 查询资金失败: " << pRspInfo->ErrorMsg << std::endl;
        return;
    }
    if (!pTradingAccount) return;
    
    if (parent_->debug_) {
        std::cout << "[CTP-Trade] 收到资金回报: Acc=" << pTradingAccount->AccountID 
                  << ", Balance=" << pTradingAccount->Balance << ", Avail=" << pTradingAccount->Available << std::endl;
    }
    
    AccountDetail acc = {0};
    strncpy(acc.broker_id, pTradingAccount->BrokerID, 10);
    strncpy(acc.account_id, pTradingAccount->AccountID, 12);
    acc.balance = pTradingAccount->Balance;
    acc.available = pTradingAccount->Available;
    acc.margin = pTradingAccount->CurrMargin;
    acc.close_pnl = pTradingAccount->CloseProfit;
    acc.position_pnl = pTradingAccount->PositionProfit;

    parent_->bus_->publish(EVENT_ACC_UPDATE, &acc);
}

void CtpRealModule::TraderSpi::OnRtnOrder(CThostFtdcOrderField *pOrder) {
    if (!pOrder) return;
    
    if (parent_->debug_) {
        std::cout << "[CTP-Trade] Order Update: " << pOrder->InstrumentID << " Ref=" << pOrder->OrderRef 
                  << " Status=" << pOrder->OrderStatus << " Msg=" << pOrder->StatusMsg << std::endl;
    }

    OrderRtn rtn;
    strncpy(rtn.order_ref, pOrder->OrderRef, 12);
    strncpy(rtn.symbol, pOrder->InstrumentID, 31);
    rtn.symbol_id = SymbolManager::instance().get_id(rtn.symbol);
    strncpy(rtn.account_id, parent_->user_id_.c_str(), 15);
    strncpy(rtn.exchange_id, pOrder->ExchangeID, 8); // 透传交易所ID
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
    strncpy(rtn.order_sys_id, pOrder->OrderSysID, 20);

    parent_->bus_->publish(EVENT_RTN_RAW_ORDER, &rtn);
}

void CtpRealModule::TraderSpi::OnRtnTrade(CThostFtdcTradeField *pTrade) {
    if (!pTrade) return;
    if (parent_->debug_) {
        std::cout << "[CTP-Trade] EXECUTION: " << pTrade->InstrumentID << " " << pTrade->Direction 
                  << " @ " << pTrade->Price << " Vol=" << pTrade->Volume << std::endl;
    }

    TradeRtn rtn;
    strncpy(rtn.symbol, pTrade->InstrumentID, 31);
    rtn.symbol_id = SymbolManager::instance().get_id(rtn.symbol);
    strncpy(rtn.account_id, parent_->user_id_.c_str(), 15);
    strncpy(rtn.exchange_id, pTrade->ExchangeID, 8); // 透传交易所ID
    rtn.direction = (pTrade->Direction == THOST_FTDC_D_Buy) ? 'B' : 'S';
    
    if (pTrade->OffsetFlag == THOST_FTDC_OF_Open) rtn.offset_flag = 'O';
    else if (pTrade->OffsetFlag == THOST_FTDC_OF_CloseToday) rtn.offset_flag = 'T';
    else rtn.offset_flag = 'C';

    rtn.price = pTrade->Price;
    rtn.volume = pTrade->Volume;
    strncpy(rtn.trade_id, pTrade->TradeID, 20);
    strncpy(rtn.order_ref, pTrade->OrderRef, 12);
    strncpy(rtn.order_sys_id, pTrade->OrderSysID, 20);

    parent_->bus_->publish(EVENT_RTN_RAW_TRADE, &rtn);
}

void CtpRealModule::TraderSpi::OnRspOrderInsert(CThostFtdcInputOrderField *pInputOrder, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        std::cerr << "[CTP-Trade] Order Insert Rsp Error: " << pRspInfo->ErrorMsg << std::endl;
        if (pInputOrder) {
            OrderRtn rtn;
            std::memset(&rtn, 0, sizeof(rtn));
            strncpy(rtn.order_ref, pInputOrder->OrderRef, 12);
            strncpy(rtn.symbol, pInputOrder->InstrumentID, 31);
            strncpy(rtn.account_id, parent_->user_id_.c_str(), 15);
            rtn.status = '5'; 
            strncpy(rtn.status_msg, pRspInfo->ErrorMsg, 80);
            parent_->bus_->publish(EVENT_RTN_RAW_ORDER, &rtn);
        }
    }
}

void CtpRealModule::TraderSpi::OnErrRtnOrderInsert(CThostFtdcInputOrderField *pInputOrder, CThostFtdcRspInfoField *pRspInfo) {
    std::cerr << "[CTP-Trade] Order Insert Error: " << (pRspInfo ? pRspInfo->ErrorMsg : "Unknown") << std::endl;
    if (pInputOrder && pRspInfo) {
        OrderRtn rtn;
        std::memset(&rtn, 0, sizeof(rtn));
        strncpy(rtn.order_ref, pInputOrder->OrderRef, 12);
        strncpy(rtn.symbol, pInputOrder->InstrumentID, 31);
        rtn.status = '5'; // 视为已撤单/失败
        strncpy(rtn.status_msg, pRspInfo->ErrorMsg, 80);
        parent_->bus_->publish(EVENT_RTN_RAW_ORDER, &rtn);
    }
}

void CtpRealModule::parse_reconnect_times(const std::string& times_str) {
    reconnect_time_ranges_.clear();
    
    if (times_str.empty()) return;
    
    // 解析时间字符串 "HH:MM:SS" -> HHMMSS (整数)
    auto parse_time = [](const std::string& time_str) -> int {
        if (time_str.length() < 8) return -1;
        try {
            int h = std::stoi(time_str.substr(0, 2));
            int m = std::stoi(time_str.substr(3, 2));
            int s = std::stoi(time_str.substr(6, 2));
            return h * 10000 + m * 100 + s;
        } catch (...) {
            return -1;
        }
    };
    
    // 按逗号分割多个时间段
    std::istringstream iss(times_str);
    std::string range_str;
    while (std::getline(iss, range_str, ',')) {
        // 去除前后空格
        range_str.erase(0, range_str.find_first_not_of(" \t"));
        range_str.erase(range_str.find_last_not_of(" \t") + 1);
        
        // 查找 "-" 分隔符
        size_t dash_pos = range_str.find('-');
        if (dash_pos == std::string::npos) continue;
        
        std::string start_str = range_str.substr(0, dash_pos);
        std::string end_str = range_str.substr(dash_pos + 1);
        
        // 去除空格
        start_str.erase(0, start_str.find_first_not_of(" \t"));
        start_str.erase(start_str.find_last_not_of(" \t") + 1);
        end_str.erase(0, end_str.find_first_not_of(" \t"));
        end_str.erase(end_str.find_last_not_of(" \t") + 1);
        
        int start_time = parse_time(start_str);
        int end_time = parse_time(end_str);
        
        if (start_time >= 0 && end_time >= 0) {
            reconnect_time_ranges_.emplace_back(start_time, end_time);
            if (debug_) {
                std::cout << "[CTP-Trade] 加载重连时间段: " << start_str 
                          << " - " << end_str << std::endl;
            }
        }
    }
    
    if (debug_ && !reconnect_time_ranges_.empty()) {
        std::cout << "[CTP-Trade] 共加载 " << reconnect_time_ranges_.size() 
                  << " 个重连时间段" << std::endl;
    }
}

bool CtpRealModule::is_in_reconnect_time() const {
    // 如果未配置重连时间范围，默认不重连
    if (reconnect_time_ranges_.empty()) {
        return false;
    }
    
    // 获取当前时间
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm* lt = std::localtime(&now);
    int current_time = lt->tm_hour * 10000 + lt->tm_min * 100 + lt->tm_sec;
    
    // 检查是否在任意一个时间段内
    for (const auto& range : reconnect_time_ranges_) {
        int start_time = range.first;
        int end_time = range.second;
        
        // 判断是否在时间范围内（支持跨午夜）
        if (start_time <= end_time) {
            // 正常时间段（不跨午夜）
            if (current_time >= start_time && current_time <= end_time) {
                return true;
            }
        } else {
            // 跨午夜情况（例如 21:00:00 到 02:30:00）
            if (current_time >= start_time || current_time <= end_time) {
                return true;
            }
        }
    }
    
    return false;
}

void CtpRealModule::do_connect() {
    // 1. 安全清理旧连接
    if (td_api_) {
        if (debug_) std::cout << "[CTP-Trade] Releasing old API instance..." << std::endl;
        
        td_api_->RegisterSpi(nullptr); // 切断回调
        td_api_->Release();
        td_api_ = nullptr;
        
        // 给 API 内部线程一点退出时间，防止文件锁冲突
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    
    if (td_spi_) {
        delete td_spi_;
        td_spi_ = nullptr;
    }
    
    // 2. 创建新连接
    if (!td_front_.empty()) {
        std::string flow_path = "./flow_log/td_" + user_id_ + "_";
        
        td_api_ = CThostFtdcTraderApi::CreateFtdcTraderApi(flow_path.c_str());
        td_spi_ = new TraderSpi(this);
        td_api_->RegisterSpi(td_spi_);
        td_api_->RegisterFront(const_cast<char*>(td_front_.c_str()));
        td_api_->SubscribePublicTopic(THOST_TERT_QUICK);
        td_api_->SubscribePrivateTopic(THOST_TERT_QUICK);
        td_api_->Init();
        
        if (debug_) std::cout << "[CTP-Trade] Init called." << std::endl;
    }
}

EXPORT_MODULE(CtpRealModule)