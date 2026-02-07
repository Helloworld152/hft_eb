#include "framework.h"
#include "protocol.h"
#include "mmap_util.h"
#include "market_snapshot.h"
#include <iostream>
#include <thread>
#include <atomic>
#include <cstring>
#include <chrono>
#include <immintrin.h> // 用于 _mm_pause

class ReplayModule : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config, ITimerService* timer_svc = nullptr) override {
        bus_ = bus;
        
        if (config.find("data_file") != config.end()) {
            file_path_ = config.at("data_file");
        } else {
            std::cerr << "[Replay] 配置文件中未指定 data_file!" << std::endl;
        }

        if (config.find("debug") != config.end()) {
            debug_ = (config.at("debug") == "true" || config.at("debug") == "1");
        }

        std::cout << "[Replay] 模块初始化完成。Mmap 基础路径: " << file_path_ << std::endl;
    }

    void start() override {
        running_ = true;
        thread_ = std::thread(&ReplayModule::run, this);
    }

    void stop() override {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        MarketSnapshot::instance().clear();
    }

private:
    void run() {
        while (running_) {
            try {
                // 尝试连接到 Mmap 通道
                MmapReader<TickRecord> reader(file_path_);
                std::cout << "[Replay] 已连接到 Mmap 管道，开始回放..." << std::endl;

                auto start_t = std::chrono::high_resolution_clock::now();
                bool perf_logged = false;
                
                // 批量读取缓冲区（可选优化）
                constexpr size_t BATCH_SIZE = 16;
                const TickRecord* batch_ptrs[BATCH_SIZE];

                while (running_) {
                    // 批量读取模式：一次读取多条记录
                    size_t batch_count = reader.read_batch(batch_ptrs, BATCH_SIZE);
                    
                    if (batch_count > 0) {
                        if (debug_ && tick_count_ == 0) {
                            start_t = std::chrono::high_resolution_clock::now();
                        }
                        
                        // 处理批量数据
                        for (size_t i = 0; i < batch_count; ++i) {
                            publish_tick(*batch_ptrs[i]);
                        }
                        perf_logged = false;
                    } else {
                        if (debug_ &&tick_count_ > 0 && !perf_logged) {
                            auto end_t = std::chrono::high_resolution_clock::now();
                            auto cost_us = std::chrono::duration_cast<std::chrono::microseconds>(end_t - start_t).count();
                            std::cout << "[Replay] Finished/Paused. Ticks: " << tick_count_ 
                                      << ", Cost: " << cost_us << " us" << std::endl;
                            perf_logged = true;
                        }

                        // 无锁轮询，极低延迟
                        _mm_pause(); 
                        
                        // 可选：如果 CPU 负载过高，可取消下面的注释
                        // std::this_thread::sleep_for(std::chrono::microseconds(1));
                    }
                }
                return;
            } catch (const std::exception& e) {
                // 可能 Writer 尚未创建文件，等待并重试
                std::cout << "[Replay] 等待数据源 (" << file_path_ << ")... " << e.what() << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }

    void publish_tick(const TickRecord& rec) {
        // 采样打印：前5条必打，之后每50条打一次
        // Debug mode: Use string comparison for robustness (no dependency on SymbolManager loading)
        if (debug_ && (tick_count_ < 5 || (tick_count_ % 10 == 0 && strcmp(rec.symbol, "au2606") == 0))) {
            std::cout << "[Bus] #" << tick_count_ << " | " << rec.symbol << " (ID:" << rec.symbol_id << ")"
                      << " | Trading Day: " << rec.trading_day
                      << " | Update Time: " << rec.update_time
                      << " | Last: " << rec.last_price << " | Vol: " << rec.volume << std::endl;
        }
        tick_count_++;

        MarketSnapshot::instance().update(rec);
        bus_->publish(EVENT_MARKET_DATA, const_cast<TickRecord*>(&rec));
    }

    EventBus* bus_ = nullptr;
    std::string file_path_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    bool debug_ = false;
    uint64_t tick_count_ = 0; // 计数器
};

EXPORT_MODULE(ReplayModule)
