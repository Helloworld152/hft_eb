#pragma once

#include "protocol.h"
#include "ring_buffer.h"
#include "market_snapshot.h"
#include "ThostFtdcMdApi.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class TickRecorder : public CThostFtdcMdSpi {
public:
    explicit TickRecorder(const std::string& config_path);
    ~TickRecorder();

    void start();
    void stop();
    bool is_in_time_range() const;

    void OnFrontConnected() override;
    void OnRspUserLogin(
        CThostFtdcRspUserLoginField* pRspUserLogin,
        CThostFtdcRspInfoField* pRspInfo,
        int nRequestID,
        bool bIsLast) override;
    void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* pData) override;

private:
    struct WriterContext;

    void load_config(const std::string& config_path);
    uint32_t parse_time(const std::string& time_str);
    void writer_loop();
    void save_to_file(const TickRecord& rec);

    std::string md_front_;
    std::string broker_id_;
    std::string user_id_;
    std::string password_;
    std::vector<std::string> symbols_;
    std::string output_path_;
    std::string file_suffix_;
    uint32_t start_time_ = 0;
    uint32_t end_time_ = 0;
    uint64_t initial_capacity_ = 50000000;

    bool use_shm_ = false;
    std::string shm_path_ = "/hft_md_snapshot";
    std::unique_ptr<MarketSnapshot> shm_impl_;

    CThostFtdcMdApi* md_api_ = nullptr;
    RingBuffer<TickRecord, 65536> rb_;
    std::thread writer_thread_;
    std::atomic<bool> running_{false};
    uint32_t trading_day_int_ = 0;

    std::unique_ptr<WriterContext> global_ctx_;
};
