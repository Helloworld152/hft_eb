#pragma once

#include "market_snapshot.h"
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ccapi {
class Event;
class Message;
}  // namespace ccapi

class BaCcapiEventHandler;

class BaTickRecorder {
public:
    explicit BaTickRecorder(const std::string& config_path);
    ~BaTickRecorder();

    void start();
    void stop();
    bool is_in_time_range() const;

private:
    friend class BaCcapiEventHandler;

    void load_config(const std::string& config_path);
    uint32_t parse_time(const std::string& time_str);
    bool check_proxy_reachable() const;
    void connect_loop();
    void handle_event(const ccapi::Event& event);
    void handle_depth_message(const ccapi::Message& message, const std::string& symbol);
    static uint64_t epoch_ms_to_hhmmssmmm_utc(uint64_t epoch_ms);

    std::string proxy_;
    std::vector<std::string> symbols_;
    uint32_t start_time_ = 0;
    uint32_t end_time_ = 0;
    uint32_t trading_day_int_ = 0;

    bool use_shm_ = true;
    std::string shm_path_ = "/hft_ba_md_snapshot";
    std::unique_ptr<MarketSnapshot> shm_impl_;

    std::thread ws_thread_;
    std::atomic<bool> running_{false};
    bool debug_ = false;  // 是否打印行情输出

};
