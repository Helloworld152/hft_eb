#include "Recorder.h"

#include "mmap_util.h"
#include "symbol_manager.h"

#include <yaml-cpp/yaml.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

struct TickRecorder::WriterContext {
    std::unique_ptr<MmapWriter<TickRecord>> writer;
};

TickRecorder::TickRecorder(const std::string& config_path) {
    load_config(config_path);
}

TickRecorder::~TickRecorder() {
    stop();
}

void TickRecorder::start() {
    if (running_) {
        return;
    }
    running_ = true;

    SymbolManager::instance().load("../conf/symbols.txt");

    if (use_shm_) {
        try {
            shm_impl_ = std::make_unique<ShmMarketSnapshot>(shm_path_, true);
            MarketSnapshot::set_instance(shm_impl_.get());
            std::cout << "[Recorder] SHM Snapshot initialized at: " << shm_path_ << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[Recorder] Failed to init SHM: " << e.what() << std::endl;
        }
    }

    writer_thread_ = std::thread(&TickRecorder::writer_loop, this);

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

void TickRecorder::stop() {
    if (!running_) {
        return;
    }
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

bool TickRecorder::is_in_time_range() const {
    if (start_time_ == 0 && end_time_ == 0) {
        return true;
    }

    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm* lt = localtime(&now);
    uint32_t current_time = lt->tm_hour * 10000 + lt->tm_min * 100 + lt->tm_sec;

    if (start_time_ <= end_time_) {
        return current_time >= start_time_ && current_time <= end_time_;
    }

    return current_time >= start_time_ || current_time <= end_time_;
}

void TickRecorder::OnFrontConnected() {
    std::cout << "[Recorder] Front connected. Logging in..." << std::endl;
    CThostFtdcReqUserLoginField req = {0};
    strncpy(req.BrokerID, broker_id_.c_str(), sizeof(req.BrokerID) - 1);
    strncpy(req.UserID, user_id_.c_str(), sizeof(req.UserID) - 1);
    strncpy(req.Password, password_.c_str(), sizeof(req.Password) - 1);
    md_api_->ReqUserLogin(&req, 0);
}

void TickRecorder::OnRspUserLogin(
    CThostFtdcRspUserLoginField* pRspUserLogin,
    CThostFtdcRspInfoField* pRspInfo,
    int nRequestID,
    bool bIsLast) {
    (void)nRequestID;
    (void)bIsLast;

    if (pRspInfo && pRspInfo->ErrorID == 0) {
        std::string tday = pRspUserLogin->TradingDay;
        std::cout << "[Recorder] Login Success. Exchange TradingDay: " << tday
                  << " | Using Config TradingDay: " << trading_day_int_ << std::endl;

        std::vector<char*> subs;
        for (auto& s : symbols_) {
            subs.push_back(const_cast<char*>(s.c_str()));
        }
        md_api_->SubscribeMarketData(subs.data(), subs.size());
        std::cout << "[Recorder] Login Success. Day: " << tday << std::endl;
    }
}

void TickRecorder::OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* pData) {
    if (!pData) {
        return;
    }

    TickRecord rec;
    memset(&rec, 0, sizeof(TickRecord));

    strncpy(rec.symbol, pData->InstrumentID, sizeof(rec.symbol) - 1);
    rec.symbol_id = SymbolManager::instance().get_id(rec.symbol);

    if (pData->TradingDay[0] != '\0') {
        rec.trading_day = std::stoi(pData->TradingDay);
    } else {
        rec.trading_day = trading_day_int_;
    }

    rec.last_price = pData->LastPrice;
    rec.volume = pData->Volume;
    rec.turnover = pData->Turnover;
    rec.open_interest = pData->OpenInterest;

    rec.upper_limit = pData->UpperLimitPrice;
    rec.lower_limit = pData->LowerLimitPrice;
    rec.open_price = pData->OpenPrice;
    rec.highest_price = pData->HighestPrice;
    rec.lowest_price = pData->LowestPrice;
    rec.pre_close_price = pData->PreClosePrice;

    rec.bid_price[0] = pData->BidPrice1;
    rec.bid_volume[0] = pData->BidVolume1;
    rec.bid_price[1] = pData->BidPrice2;
    rec.bid_volume[1] = pData->BidVolume2;
    rec.bid_price[2] = pData->BidPrice3;
    rec.bid_volume[2] = pData->BidVolume3;
    rec.bid_price[3] = pData->BidPrice4;
    rec.bid_volume[3] = pData->BidVolume4;
    rec.bid_price[4] = pData->BidPrice5;
    rec.bid_volume[4] = pData->BidVolume5;

    rec.ask_price[0] = pData->AskPrice1;
    rec.ask_volume[0] = pData->AskVolume1;
    rec.ask_price[1] = pData->AskPrice2;
    rec.ask_volume[1] = pData->AskVolume2;
    rec.ask_price[2] = pData->AskPrice3;
    rec.ask_volume[2] = pData->AskVolume3;
    rec.ask_price[3] = pData->AskPrice4;
    rec.ask_volume[3] = pData->AskVolume4;
    rec.ask_price[4] = pData->AskPrice5;
    rec.ask_volume[4] = pData->AskVolume5;

    int hh = 0;
    int mm = 0;
    int ss = 0;
    if (sscanf(pData->UpdateTime, "%d:%d:%d", &hh, &mm, &ss) == 3) {
        rec.update_time =
            (static_cast<uint64_t>(hh) * 10000 + mm * 100 + ss) * 1000 + pData->UpdateMillisec;
    }

    if (use_shm_) {
        MarketSnapshot::instance().update(rec);
    }
    if (!rb_.push(rec)) {
    }
}

void TickRecorder::load_config(const std::string& config_path) {
    YAML::Node doc;
    try {
        doc = YAML::LoadFile(config_path);
    } catch (const YAML::Exception& e) {
        throw std::runtime_error("FATAL: YAML Parse Error in " + config_path + ": " + e.what());
    }

    if (doc["md_front"]) {
        md_front_ = doc["md_front"].as<std::string>();
    }
    if (doc["broker_id"]) {
        broker_id_ = doc["broker_id"].as<std::string>();
    }
    if (doc["user_id"]) {
        user_id_ = doc["user_id"].as<std::string>();
    }
    if (doc["password"]) {
        password_ = doc["password"].as<std::string>();
    }
    if (doc["output_path"]) {
        output_path_ = doc["output_path"].as<std::string>();
    }
    if (doc["file_suffix"]) {
        file_suffix_ = doc["file_suffix"].as<std::string>();
    }

    if (doc["trading_day"]) {
        trading_day_int_ = std::stoi(doc["trading_day"].as<std::string>());
    } else {
        throw std::runtime_error("FATAL: Missing mandatory config 'trading_day'");
    }

    if (doc["start_time"]) {
        start_time_ = parse_time(doc["start_time"].as<std::string>());
    }
    if (doc["end_time"]) {
        end_time_ = parse_time(doc["end_time"].as<std::string>());
    }

    if (doc["symbols"] && doc["symbols"].IsSequence()) {
        for (const auto& s : doc["symbols"]) {
            symbols_.push_back(s.as<std::string>());
        }
    }

    if (doc["initial_capacity"]) {
        initial_capacity_ = doc["initial_capacity"].as<uint64_t>();
    }

    if (doc["shm"]) {
        use_shm_ = true;
        shm_path_ = doc["shm"].as<std::string>();
    }
}

uint32_t TickRecorder::parse_time(const std::string& time_str) {
    int hh = 0;
    int mm = 0;
    int ss = 0;
    if (sscanf(time_str.c_str(), "%d:%d:%d", &hh, &mm, &ss) >= 2) {
        return hh * 10000 + mm * 100 + ss;
    }
    return 0;
}

void TickRecorder::writer_loop() {
    while (running_) {
        TickRecord rec;
        if (rb_.pop(rec)) {
            save_to_file(rec);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    TickRecord rec;
    while (rb_.pop(rec)) {
        save_to_file(rec);
    }
    global_ctx_.reset();
}

void TickRecorder::save_to_file(const TickRecord& rec) {
    if (!global_ctx_) {
        global_ctx_ = std::make_unique<WriterContext>();

        fs::create_directories(output_path_);

        char date_str[16];
        snprintf(date_str, sizeof(date_str), "%u", trading_day_int_);

        std::string base_path = output_path_ + "/market_data_" + date_str + file_suffix_;

        std::cout << "[Recorder] Output File: " << base_path << std::endl;
        std::cout << "[Recorder] Initial Capacity: " << initial_capacity_ << " records (~"
                  << (initial_capacity_ * sizeof(TickRecord) / (1024.0 * 1024.0 * 1024.0))
                  << " GB)" << std::endl;

        global_ctx_->writer = std::make_unique<MmapWriter<TickRecord>>(base_path, initial_capacity_);
    }

    if (global_ctx_->writer) {
        if (!global_ctx_->writer->write(rec)) {
            std::cerr << "[Recorder] WARN: Mmap buffer full!" << std::endl;
        }
    }
}
