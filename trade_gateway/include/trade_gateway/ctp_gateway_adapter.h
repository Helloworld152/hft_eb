#pragma once

#include "gateway_config.h"
#include "gateway_protocol.h"

#include "ThostFtdcTraderApi.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace trade_gateway {

class CtpGatewayAdapter {
public:
    using EventPublisher = std::function<void(const GatewayEvent&)>;

    explicit CtpGatewayAdapter(const GatewayConfig& config);
    ~CtpGatewayAdapter();

    CtpGatewayAdapter(const CtpGatewayAdapter&) = delete;
    CtpGatewayAdapter& operator=(const CtpGatewayAdapter&) = delete;

    void set_event_publisher(EventPublisher publisher);

    void connect();
    void stop();

    bool is_ready() const noexcept { return ready_.load(std::memory_order_acquire); }
    bool is_logged_in() const noexcept { return logged_in_.load(std::memory_order_acquire); }

    bool should_reconnect_now() const;

    bool submit_order(const OrderReq& req);
    bool cancel_order(const CancelReq& req);
    bool query_position();
    bool query_account();
    bool query_open_orders();
    void on_ready();

private:
    class TraderSpi : public CThostFtdcTraderSpi {
    public:
        explicit TraderSpi(CtpGatewayAdapter* parent) : parent_(parent) {}

        void OnFrontConnected() override;
        void OnFrontDisconnected(int nReason) override;
        void OnRspAuthenticate(CThostFtdcRspAuthenticateField* pRspAuthenticateField,
                               CThostFtdcRspInfoField* pRspInfo,
                               int nRequestID,
                               bool bIsLast) override;
        void OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
                            CThostFtdcRspInfoField* pRspInfo,
                            int nRequestID,
                            bool bIsLast) override;
        void OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField* pSettlementInfoConfirm,
                                        CThostFtdcRspInfoField* pRspInfo,
                                        int nRequestID,
                                        bool bIsLast) override;
        void OnRtnOrder(CThostFtdcOrderField* pOrder) override;
        void OnRtnTrade(CThostFtdcTradeField* pTrade) override;
        void OnRspOrderInsert(CThostFtdcInputOrderField* pInputOrder,
                              CThostFtdcRspInfoField* pRspInfo,
                              int nRequestID,
                              bool bIsLast) override;
        void OnErrRtnOrderInsert(CThostFtdcInputOrderField* pInputOrder,
                                 CThostFtdcRspInfoField* pRspInfo) override;
        void OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* pInvestorPosition,
                                      CThostFtdcRspInfoField* pRspInfo,
                                      int nRequestID,
                                      bool bIsLast) override;
        void OnRspQryTradingAccount(CThostFtdcTradingAccountField* pTradingAccount,
                                    CThostFtdcRspInfoField* pRspInfo,
                                    int nRequestID,
                                    bool bIsLast) override;
        void OnRspQryOrder(CThostFtdcOrderField* pOrder,
                           CThostFtdcRspInfoField* pRspInfo,
                           int nRequestID,
                           bool bIsLast) override;

        int front_id_ = 0;
        int session_id_ = 0;

    private:
        CtpGatewayAdapter* parent_ = nullptr;
    };

    void publish_status(char status, const char* msg);
    void publish_error(GatewayErrorCode code, const std::string& message);
    void publish_order_rtn(const OrderRtn& rtn);
    void publish_trade_rtn(const TradeRtn& rtn);
    void publish_position_rsp(const PositionDetail& pos);
    void publish_account_rsp(const AccountDetail& acc);
    void publish_cache_reset(uint32_t trading_day, const char* reason);

    bool query_account_internal(bool log_error);
    bool query_position_internal(bool log_error);
    bool query_open_orders_internal(bool log_error);

    static std::vector<std::pair<int, int>> parse_reconnect_times(const std::string& times_str);
    static int parse_time_hhmmss(const std::string& time_str);

    GatewayConfig config_;
    EventPublisher publisher_;
    CThostFtdcTraderApi* td_api_ = nullptr;
    TraderSpi* td_spi_ = nullptr;
    std::atomic<int> req_id_{0};
    std::atomic<bool> logged_in_{false};
    std::atomic<bool> ready_{false};
    std::atomic<uint64_t> last_connect_attempt_ns_{0};
    uint32_t ctp_trading_day_ = 0;
};

}  // namespace trade_gateway
