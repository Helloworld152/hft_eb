#include "../../include/framework.h"
#include "../../core/include/symbol_manager.h"
#include "../../core/include/market_snapshot.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <iomanip>
#include <algorithm>

namespace fs = std::filesystem;

struct TwapTask {
    std::string filename;
    OrderReq base_req;
    double ref_price;
    int total_volume;
    int executed_volume;
    int interval_sec;
    uint64_t start_ts; // HHMMSSmmm
    uint64_t end_ts;
    std::chrono::steady_clock::time_point last_exec_time;
    bool finished = false;
};

class SweepTraderModule : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config, ITimerService* timer_svc = nullptr) override {
        bus_ = bus;
        timer_svc_ = timer_svc;

        // 配置读取
        order_dir_ = config.count("order_dir") ? config.at("order_dir") : "../data/orders";
        price_strategy_ = config.count("default_price_strategy") ? config.at("default_price_strategy") : "opp";
        default_account_ = config.count("default_account") ? config.at("default_account") : "888888";
        int scan_ms = config.count("scan_interval_ms") ? std::stoi(config.at("scan_interval_ms")) : 1000;

        // 创建必要目录
        fs::create_directories(order_dir_);
        fs::create_directories(fs::path(order_dir_) / "processed");
        fs::create_directories(fs::path(order_dir_) / "error");

        // 订阅行情用于自动定价
        bus_->subscribe(EVENT_MARKET_DATA, [this](void* d) {
            auto* tick = static_cast<TickRecord*>(d);
            ticks_[tick->symbol_id] = *tick;
        });

        // 注册目录扫描定时器
        if (timer_svc_) {
            timer_svc_->add_timer(scan_ms / 1000, [this]() {
                this->scanDirectory();
            });
            
            // TWAP 轮询检查 (每秒一次)
            timer_svc_->add_timer(1, [this]() {
                this->checkTwapTasks();
            });
        }

        std::cout << "[SweepTrader] Initialized. Dir: " << order_dir_ << ", Strategy: " << price_strategy_ << std::endl;
    }

    void scanDirectory() {
        for (const auto& entry : fs::directory_iterator(order_dir_)) {
            if (entry.is_regular_file() && entry.path().extension() == ".csv") {
                processFile(entry.path());
            }
        }
    }

    void processFile(const fs::path& path) {
        std::ifstream file(path);
        if (!file.is_open()) return;

        std::string line;
        bool is_first = true;
        while (std::getline(file, line)) {
            if (line.empty() || is_first) { // 跳过表头
                is_first = false;
                continue;
            }
            parseAndExecute(line, path.filename().string());
        }
        file.close();
        
        // 只有非 TWAP 的文件才立即移动
        if (active_tasks_.find(path.filename().string()) == active_tasks_.end()) {
            fs::rename(path, fs::path(order_dir_) / "processed" / path.filename());
        }
    }

    void parseAndExecute(const std::string& line, const std::string& filename) {
        std::stringstream ss(line);
        std::string item;
        std::vector<std::string> fields;
        while (std::getline(ss, item, ',')) {
            fields.push_back(item);
        }

        if (fields.size() < 9) return;

        try {
            OrderReq req{};
            strncpy(req.symbol, fields[0].c_str(), 31);
            req.symbol_id = SymbolManager::instance().get_id(req.symbol);
            req.direction = fields[1][0];
            req.offset_flag = fields[2][0];
            double ref_price = std::stod(fields[3]);
            int volume = std::stoi(fields[4]);
            strncpy(req.account_id, fields[5].empty() ? default_account_.c_str() : fields[5].c_str(), 15);
            
            uint64_t start_ts = timeToUint(fields[6]);
            uint64_t end_ts = timeToUint(fields[7]);
            std::string algo = fields[8];
            int interval = (fields.size() > 9) ? std::stoi(fields[9]) : 0;

            if (algo == "twap") {
                TwapTask task;
                task.filename = filename;
                task.base_req = req;
                task.ref_price = ref_price;
                task.total_volume = volume;
                task.executed_volume = 0;
                task.interval_sec = (interval > 0) ? interval : 60;
                task.start_ts = start_ts;
                task.end_ts = end_ts;
                task.last_exec_time = std::chrono::steady_clock::now() - std::chrono::seconds(task.interval_sec);
                active_tasks_[filename] = task;
                std::cout << "[SweepTrader] TWAP task added: " << req.symbol << " vol=" << volume << std::endl;
            } else {
                // Direct 执行
                if (isCurrentTimeInRange(start_ts, end_ts)) {
                    req.volume = volume;
                    executeOrder(req);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[SweepTrader] Parse error: " << e.what() << " in line: " << line << std::endl;
        }
    }

    void checkTwapTasks() {
        auto now = std::chrono::steady_clock::now();
        uint64_t current_ts = getCurrentTimeUint();

        for (auto it = active_tasks_.begin(); it != active_tasks_.end(); ) {
            auto& task = it->second;

            // 检查时间窗口
            if (current_ts < task.start_ts) { ++it; continue; }
            if (current_ts > task.end_ts || task.executed_volume >= task.total_volume) {
                std::cout << "[SweepTrader] TWAP finished: " << task.base_req.symbol << std::endl;
                // 移动文件
                fs::path old_path = fs::path(order_dir_) / task.filename;
                if (fs::exists(old_path)) {
                    fs::rename(old_path, fs::path(order_dir_) / "processed" / task.filename);
                }
                it = active_tasks_.erase(it);
                continue;
            }

            // 检查间隔
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - task.last_exec_time).count();
            if (elapsed >= task.interval_sec) {
                // 计算此批次单量
                int remaining = task.total_volume - task.executed_volume;
                // 简单平分逻辑：剩余量 / 剩余批次
                // 简化处理：每批次固定 1 手，或自定义逻辑
                int batch_vol = std::max(1, remaining / 10); // 示例：每批次 10%
                if (batch_vol > remaining) batch_vol = remaining;

                OrderReq req = task.base_req;
                req.volume = batch_vol;
                if (executeOrder(req)) {
                    task.executed_volume += batch_vol;
                    task.last_exec_time = now;
                }
            }
            ++it;
        }
    }

    bool executeOrder(OrderReq& req) {
        TickRecord tick;
        bool has_tick = MarketSnapshot::instance().get(req.symbol_id, tick);
        if (!has_tick && ticks_.find(req.symbol_id) != ticks_.end()) {
            tick = ticks_[req.symbol_id];
            has_tick = true;
        }
        if (!has_tick) return false;

        double price = 0;
        if (price_strategy_ == "opp") {
            price = (req.direction == 'B') ? tick.ask_price[0] : tick.bid_price[0];
        } else if (price_strategy_ == "mid" && tick.bid_price[0] > 0 && tick.ask_price[0] > 0) {
            price = (tick.bid_price[0] + tick.ask_price[0]) * 0.5;
        } else {
            price = tick.last_price;
        }

        if (price <= 0) return false;

        req.price = price;
        bus_->publish(EVENT_ORDER_REQ, &req);
        
        std::cout << "[SweepTrader] Order Published: " << req.symbol 
                  << " " << req.direction << " " << req.volume << " @ " << price << std::endl;
        return true;
    }

private:
    uint64_t timeToUint(const std::string& time_str) {
        // HH:MM:SS -> HHMMSS000
        int h, m, s;
        sscanf(time_str.c_str(), "%d:%d:%d", &h, &m, &s);
        return h * 1000000ULL + m * 10000ULL + s * 100ULL;
    }

    uint64_t getCurrentTimeUint() {
        auto now = std::chrono::system_clock::now();
        time_t t = std::chrono::system_clock::to_time_t(now);
        struct tm tm;
        localtime_r(&t, &tm);
        return tm.tm_hour * 1000000ULL + tm.tm_min * 10000ULL + tm.tm_sec * 100ULL;
    }

    bool isCurrentTimeInRange(uint64_t start, uint64_t end) {
        uint64_t now = getCurrentTimeUint();
        return now >= start && now <= end;
    }

    EventBus* bus_;
    ITimerService* timer_svc_;
    std::string order_dir_;
    std::string price_strategy_;
    std::string default_account_;

    std::unordered_map<uint64_t, TickRecord> ticks_;
    std::unordered_map<std::string, TwapTask> active_tasks_;
};

EXPORT_MODULE(SweepTraderModule)
