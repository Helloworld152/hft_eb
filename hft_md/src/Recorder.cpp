#include "protocol.h"
#include "ring_buffer.h"
#include "ThostFtdcMdApi.h"
#include "mmap_util.h"
#include <thread>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <memory>
#include "rapidjson/document.h"
#include "rapidjson/istreamwrapper.h"

namespace fs = std::filesystem;

class TickRecorder : public CThostFtdcMdSpi {
public:
    TickRecorder(const std::string& config_path) : running_(false) {
        load_config(config_path);
    }
    
    virtual ~TickRecorder() { stop(); }

    void start() {
        if (running_) return;
        running_ = true;

        // 1. 启动异步写入线程
        writer_thread_ = std::thread(&TickRecorder::writer_loop, this);

        // 2. 初始化 CTP 行情接口
        md_api_ = CThostFtdcMdApi::CreateFtdcMdApi("./log/");
        if (!md_api_) {
            std::cerr << "FATAL: Failed to create CTP API!" << std::endl;
            return;
        }

        md_api_->RegisterSpi(this);
        md_api_->RegisterFront(const_cast<char*>(md_front_.c_str()));
        md_api_->Init();
        
        std::cout << "[Recorder] Running independently (Mmap Mode). Output: " << output_path_ << std::endl;
    }

    void stop() {
        if (!running_) return;
        running_ = false;

        if (md_api_) {
            md_api_->RegisterSpi(nullptr);
            md_api_->Release();
            md_api_ = nullptr;
        }

        if (writer_thread_.joinable()) {
            writer_thread_.join();
        }
    }

    // CTP SPI 实现
    void OnFrontConnected() override {
        std::cout << "[Recorder] Front connected. Logging in..." << std::endl;
        CThostFtdcReqUserLoginField req = {0};
        strncpy(req.BrokerID, broker_id_.c_str(), sizeof(req.BrokerID)-1);
        strncpy(req.UserID, user_id_.c_str(), sizeof(req.UserID)-1);
        strncpy(req.Password, password_.c_str(), sizeof(req.Password)-1);
        md_api_->ReqUserLogin(&req, 0);
    }

    void OnRspUserLogin(CThostFtdcRspUserLoginField *pRspUserLogin, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override {
        if (pRspInfo && pRspInfo->ErrorID == 0) {
            std::string tday = pRspUserLogin->TradingDay;
            trading_day_int_ = std::stoi(tday);
            
            std::vector<char*> subs;
            for (auto& s : symbols_) subs.push_back(const_cast<char*>(s.c_str()));
            md_api_->SubscribeMarketData(subs.data(), subs.size());
            std::cout << "[Recorder] Login Success. Day: " << tday << std::endl;
        }
    }

    void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField *pData) override {
        if (!pData) return;
        
        TickRecord rec;
        memset(&rec, 0, sizeof(TickRecord));
        
        strncpy(rec.symbol, pData->InstrumentID, sizeof(rec.symbol)-1);
        rec.trading_day = trading_day_int_;
        
        // 价格与成交
        rec.last_price = pData->LastPrice;
        rec.volume = pData->Volume;
        rec.turnover = pData->Turnover;
        rec.open_interest = pData->OpenInterest;
        
        // 统计数据
        rec.upper_limit = pData->UpperLimitPrice;
        rec.lower_limit = pData->LowerLimitPrice;
        rec.open_price = pData->OpenPrice;
        rec.highest_price = pData->HighestPrice;
        rec.lowest_price = pData->LowestPrice;
        rec.pre_close_price = pData->PreClosePrice;

        // 五档行情映射
        rec.bid_price[0] = pData->BidPrice1; rec.bid_volume[0] = pData->BidVolume1;
        rec.bid_price[1] = pData->BidPrice2; rec.bid_volume[1] = pData->BidVolume2;
        rec.bid_price[2] = pData->BidPrice3; rec.bid_volume[2] = pData->BidVolume3;
        rec.bid_price[3] = pData->BidPrice4; rec.bid_volume[3] = pData->BidVolume4;
        rec.bid_price[4] = pData->BidPrice5; rec.bid_volume[4] = pData->BidVolume5;

        rec.ask_price[0] = pData->AskPrice1; rec.ask_volume[0] = pData->AskVolume1;
        rec.ask_price[1] = pData->AskPrice2; rec.ask_volume[1] = pData->AskVolume2;
        rec.ask_price[2] = pData->AskPrice3; rec.ask_volume[2] = pData->AskVolume3;
        rec.ask_price[3] = pData->AskPrice4; rec.ask_volume[3] = pData->AskVolume4;
        rec.ask_price[4] = pData->AskPrice5; rec.ask_volume[4] = pData->AskVolume5;
        
        // 解析时间
        int hh, mm, ss;
        if (sscanf(pData->UpdateTime, "%d:%d:%d", &hh, &mm, &ss) == 3) {
            rec.update_time = (static_cast<uint64_t>(hh) * 10000 + mm * 100 + ss) * 1000 + pData->UpdateMillisec;
        }

        if (!rb_.push(rec)) {
            // 记录丢失警告
        }
    }

private:
    struct WriterContext {
        std::unique_ptr<MmapWriter<TickRecord>> writer;
        uint32_t current_day = 0;
    };

    void load_config(const std::string& config_path) {
        std::ifstream ifs(config_path);
        if (!ifs.is_open()) {
            throw std::runtime_error("FATAL: Could not open config file: " + config_path);
        }
        
        rapidjson::IStreamWrapper isw(ifs);
        rapidjson::Document doc;
        doc.ParseStream(isw);

        if (doc.HasParseError()) {
            throw std::runtime_error("FATAL: JSON Parse Error in " + config_path);
        }

        if (doc.HasMember("md_front")) md_front_ = doc["md_front"].GetString();
        if (doc.HasMember("broker_id")) broker_id_ = doc["broker_id"].GetString();
        if (doc.HasMember("user_id")) user_id_ = doc["user_id"].GetString();
        if (doc.HasMember("password")) password_ = doc["password"].GetString();
        if (doc.HasMember("output_path")) output_path_ = doc["output_path"].GetString();
        
        if (doc.HasMember("symbols") && doc["symbols"].IsArray()) {
            for (auto& s : doc["symbols"].GetArray()) {
                symbols_.push_back(s.GetString());
            }
        }
    }

    void writer_loop() {
        while (running_) {
            TickRecord rec;
            if (rb_.pop(rec)) {
                save_to_file(rec);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        TickRecord rec;
        while (rb_.pop(rec)) save_to_file(rec);
        global_ctx_.reset();
    }

    void save_to_file(const TickRecord& rec) {
        if (!global_ctx_) {
            global_ctx_ = std::make_unique<WriterContext>();
        }

        if (!global_ctx_->writer || global_ctx_->current_day != rec.trading_day) {
            fs::create_directories(output_path_);

            char date_str[16];
            snprintf(date_str, sizeof(date_str), "%u", rec.trading_day);
            
            // 基础文件名，MmapWriter 会自动补全后缀
            std::string base_path = output_path_ + "/market_data_" + date_str;
            
            std::cout << "[Recorder] Switching Mmap file: " << base_path << std::endl;
            
            // 预分配 500 万条记录 (约 1GB)
            global_ctx_->writer = std::make_unique<MmapWriter<TickRecord>>(base_path, 5000000);
            global_ctx_->current_day = rec.trading_day;
        }

        if (global_ctx_->writer) {
            if (!global_ctx_->writer->write(rec)) {
                 std::cerr << "[Recorder] WARN: Mmap buffer full!" << std::endl;
            }
        }
    }

    // 配置项
    std::string md_front_;
    std::string broker_id_;
    std::string user_id_;
    std::string password_;
    std::vector<std::string> symbols_;
    std::string output_path_;

    CThostFtdcMdApi* md_api_ = nullptr;
    RingBuffer<TickRecord, 65536> rb_;
    std::thread writer_thread_;
    std::atomic<bool> running_;
    uint32_t trading_day_int_ = 0;
    
    std::unique_ptr<WriterContext> global_ctx_;
};