#include "Recorder.h"
#include "symbol_manager.h"
#include "ccapi_cpp/ccapi_session.h"
#include <yaml-cpp/yaml.h>
#include <map>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

ccapi::Logger* ccapi::Logger::logger = nullptr;

namespace {
uint64_t to_epoch_ms(const ccapi::TimePoint& tp) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
    if (ms < 0) return 0;
    return static_cast<uint64_t>(ms);
}

template <typename MapT>
bool try_get_double(const MapT& m, std::string_view key, double& out) {
    for (const auto& kv : m) {
        if (kv.first == key) {
            try {
                out = std::stod(kv.second);
                return true;
            } catch (...) {
                return false;
            }
        }
    }
    return false;
}

template <typename MapT>
bool try_get_any_double(const MapT& m,
                        const std::vector<std::string_view>& keys,
                        double& out) {
    for (const auto& key : keys) {
        if (try_get_double(m, key, out)) return true;
    }
    return false;
}
}

class BaCcapiEventHandler : public ccapi::EventHandler {
public:
    explicit BaCcapiEventHandler(BaTickRecorder* owner) : owner_(owner) {}

    void processEvent(const ccapi::Event& event, ccapi::Session* session) override {
        (void)session;
        owner_->handle_event(event);
    }

private:
    BaTickRecorder* owner_ = nullptr;
};

BaTickRecorder::BaTickRecorder(const std::string& config_path) {
    load_config(config_path);
}

BaTickRecorder::~BaTickRecorder() {
    stop();
}

void BaTickRecorder::start() {
    if (running_) {
        return;
    }
    running_ = true;

    SymbolManager::instance().load("../conf/symbols_crypto.txt");

    if (use_shm_) {
        try {
            shm_impl_ = std::make_unique<ShmMarketSnapshot>(shm_path_, true);
            MarketSnapshot::set_instance(shm_impl_.get());
            std::cout << "[BaRecorder] SHM Snapshot initialized at: " << shm_path_ << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[BaRecorder] Failed to init SHM: " << e.what() << std::endl;
        }
    }

    if (!proxy_.empty()) {
        std::cout << "[BaRecorder] Proxy enabled for CCAPI: " << proxy_ << std::endl;
    }

    ws_thread_ = std::thread(&BaTickRecorder::connect_loop, this);

    std::cout << "[BaRecorder] Running (SHM-only). Symbols: " << symbols_.size() << std::endl;
}

void BaTickRecorder::stop() {
    if (!running_) {
        return;
    }
    running_ = false;

    if (ws_thread_.joinable()) {
        ws_thread_.join();
    }
}

bool BaTickRecorder::is_in_time_range() const {
    if (start_time_ == 0 && end_time_ == 0) {
        return true;
    }

    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm lt {};
    localtime_r(&now, &lt);
    uint32_t current_time = lt.tm_hour * 10000 + lt.tm_min * 100 + lt.tm_sec;

    if (start_time_ <= end_time_) {
        return current_time >= start_time_ && current_time <= end_time_;
    }

    return current_time >= start_time_ || current_time <= end_time_;
}

void BaTickRecorder::load_config(const std::string& config_path) {
    YAML::Node doc;
    try {
        doc = YAML::LoadFile(config_path);
    } catch (const YAML::Exception& e) {
        throw std::runtime_error("FATAL: YAML Parse Error in " + config_path + ": " + e.what());
    }

    if (doc["proxy"]) {
        proxy_ = doc["proxy"].as<std::string>();
    }

    if (doc["symbols"] && doc["symbols"].IsSequence()) {
        for (const auto& s : doc["symbols"]) {
            symbols_.push_back(s.as<std::string>());
        }
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

    if (doc["shm"]) {
        use_shm_ = true;
        shm_path_ = doc["shm"].as<std::string>();
    }

    if (doc["debug"]) {
        std::string debug_str = doc["debug"].as<std::string>();
        debug_ = (debug_str == "true" || debug_str == "1" || debug_str == "yes");
    }
}

uint32_t BaTickRecorder::parse_time(const std::string& time_str) {
    int hh = 0;
    int mm = 0;
    int ss = 0;
    if (sscanf(time_str.c_str(), "%d:%d:%d", &hh, &mm, &ss) >= 2) {
        return hh * 10000 + mm * 100 + ss;
    }
    return 0;
}

uint64_t BaTickRecorder::epoch_ms_to_hhmmssmmm_utc(uint64_t epoch_ms) {
    std::time_t t = static_cast<std::time_t>(epoch_ms / 1000);
    std::tm tm_utc {};
    gmtime_r(&t, &tm_utc);
    uint64_t ms = epoch_ms % 1000;
    uint64_t hhmmss = static_cast<uint64_t>(tm_utc.tm_hour) * 10000
        + static_cast<uint64_t>(tm_utc.tm_min) * 100
        + static_cast<uint64_t>(tm_utc.tm_sec);
    return hhmmss * 1000 + ms;
}

void BaTickRecorder::connect_loop() {
    ccapi::SessionOptions session_options;
    if (!proxy_.empty()) {
        session_options.websocketConnectTimeoutMilliseconds = 30000;
    }
    ccapi::SessionConfigs session_configs;
    BaCcapiEventHandler event_handler(this);
    ccapi::Session session(session_options, session_configs, &event_handler);

    std::cout << "[BaRecorder] CCAPI session created. Preparing subscriptions..." << std::endl;

    std::vector<ccapi::Subscription> subscriptions;
    subscriptions.reserve(symbols_.size() * 2);  // depth + ticker

    for (const auto& sym : symbols_) {
        // 检查是否为永续合约（以 _PERP 结尾）
        bool is_perp = (sym.size() > 5 && sym.compare(sym.size() - 5, 5, "_PERP") == 0);
        
        std::string base_symbol = is_perp ? sym.substr(0, sym.size() - 5) : sym;
        std::string exchange = is_perp ? "binance-usds-futures" : "binance";
        std::string ccapi_symbol = base_symbol;

        // 1. 订阅深度行情
        std::string depth_cid = "md:" + sym;
        std::string depth_options = "MARKET_DEPTH_MAX=5&CONFLATE_INTERVAL_MILLISECONDS=1000";
        if (proxy_.empty()) {
            subscriptions.emplace_back(exchange, ccapi_symbol, "MARKET_DEPTH", depth_options, depth_cid);
        } else {
            std::map<std::string, std::string> credential;
            subscriptions.emplace_back(exchange, ccapi_symbol, "MARKET_DEPTH", depth_options, depth_cid, credential, proxy_);
        }

    }

    std::cout << "[BaRecorder] Subscribing to " << subscriptions.size()
              << " streams." << std::endl;
    session.subscribe(subscriptions);
    std::cout << "[BaRecorder] Subscribe request sent." << std::endl;

    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    session.stop();
}

void BaTickRecorder::handle_event(const ccapi::Event& event) {
    if (event.getType() == ccapi::Event::Type::SUBSCRIPTION_STATUS) {
        for (const auto& message : event.getMessageList()) {
            const auto type = message.getType();
            const auto& cids = message.getCorrelationIdList();
            std::cout << "[BaRecorder] Subscription status: "
                      << ccapi::Message::typeToString(type);
            if (!cids.empty()) {
                std::cout << " cids=[";
                for (size_t i = 0; i < cids.size(); ++i) {
                    if (i) std::cout << ",";
                    std::cout << cids[i];
                }
                std::cout << "]";
            }
            std::cout << std::endl;

            if (type == ccapi::Message::Type::SUBSCRIPTION_FAILURE ||
                type == ccapi::Message::Type::SUBSCRIPTION_FAILURE_DUE_TO_CONNECTION_FAILURE ||
                type == ccapi::Message::Type::GENERIC_ERROR ||
                type == ccapi::Message::Type::INCORRECT_STATE_FOUND) {
                for (const auto& el : message.getElementList()) {
                    const auto& m = el.getNameValueMap();
                    if (m.empty()) {
                        continue;
                    }
                    std::cerr << "[BaRecorder] Subscription error details:" << std::endl;
                    for (const auto& kv : m) {
                        std::cerr << "  " << kv.first << "=" << kv.second << std::endl;
                    }
                }
            }
        }
        return;
    }
    if (event.getType() != ccapi::Event::Type::SUBSCRIPTION_DATA) {
        return;
    }

    for (const auto& message : event.getMessageList()) {
        const auto& correlation_ids = message.getCorrelationIdList();
        if (correlation_ids.empty()) {
            continue;
        }

        const std::string& cid = correlation_ids.front();
        if (cid.rfind("md:", 0) == 0) {
            handle_depth_message(message, cid.substr(3));
        }
    }
}
    
void BaTickRecorder::handle_depth_message(const ccapi::Message& message, const std::string& symbol) {
    if (!use_shm_) return;

    TickRecord rec;
    std::memset(&rec, 0, sizeof(TickRecord));
    std::strncpy(rec.symbol, symbol.c_str(), sizeof(rec.symbol) - 1);
    rec.symbol_id = SymbolManager::instance().get_id(rec.symbol);
    rec.trading_day = trading_day_int_;

    uint64_t event_time = to_epoch_ms(message.getTime());
    rec.update_time = epoch_ms_to_hhmmssmmm_utc(event_time);

    rec.last_price = 0.0;
    rec.volume = 0;
    rec.turnover = 0.0;
    rec.open_interest = 0.0;

    std::vector<std::pair<double, int>> bids;
    std::vector<std::pair<double, int>> asks;

    for (const auto& element : message.getElementList()) {
        const auto& m = element.getNameValueMap();

        double bid_price = 0.0;
        double bid_size = 0.0;
        if (try_get_any_double(m, {"BID_PRICE", "PRICE"}, bid_price) &&
            try_get_any_double(m, {"BID_SIZE", "BID_QUANTITY", "SIZE", "QUANTITY"}, bid_size)) {
            bids.emplace_back(bid_price, static_cast<int>(bid_size));
        }

        double ask_price = 0.0;
        double ask_size = 0.0;
        if (try_get_any_double(m, {"ASK_PRICE", "PRICE"}, ask_price) &&
            try_get_any_double(m, {"ASK_SIZE", "ASK_QUANTITY", "SIZE", "QUANTITY"}, ask_size)) {
            asks.emplace_back(ask_price, static_cast<int>(ask_size));
        }
    }

    for (size_t i = 0; i < 5 && i < bids.size(); ++i) {
        rec.bid_price[i] = bids[i].first;
        rec.bid_volume[i] = bids[i].second;
    }
    for (size_t i = 0; i < 5 && i < asks.size(); ++i) {
        rec.ask_price[i] = asks[i].first;
        rec.ask_volume[i] = asks[i].second;
    }

    // 打印收到的快照（仅 debug 模式）
    if (debug_) {
        std::cout << "[SNAPSHOT] " << symbol << " @ " << rec.update_time << " | ";
        std::cout << "BID: ";
        for (int i = 0; i < 5; ++i) {
            if (rec.bid_price[i] > 0) {
                std::cout << rec.bid_price[i] << "x" << rec.bid_volume[i] << " ";
            }
        }
        std::cout << "| ASK: ";
        for (int i = 0; i < 5; ++i) {
            if (rec.ask_price[i] > 0) {
                std::cout << rec.ask_price[i] << "x" << rec.ask_volume[i] << " ";
            }
        }
        std::cout << std::endl;
    }

    MarketSnapshot::instance().update(rec);
}
