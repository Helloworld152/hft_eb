#include "framework.h"
#include "protocol.h"
#include "mmap_util.h"
#include <iostream>
#include <thread>
#include <atomic>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <immintrin.h> // 用于 _mm_pause

namespace fs = std::filesystem;

class ReplayModule : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config) override {
        bus_ = bus;
        
        if (config.find("data_file") != config.end()) {
            file_path_ = config.at("data_file");
        } else {
            std::cerr << "[Replay] 配置文件中未指定 data_file!" << std::endl;
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
    }

private:
    void run() {
        while (running_) {
            try {
                // 尝试连接到 Mmap 通道
                MmapReader<TickRecord> reader(file_path_);
                std::cout << "[Replay] 已连接到 Mmap 管道，开始回放..." << std::endl;

                TickRecord rec;
                while (running_) {
                    if (reader.read(rec)) {
                        publish_tick(rec);
                    } else {
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
        if (tick_count_ < 5 || tick_count_ % 50 == 0 && strcmp(rec.symbol, "au2606") == 0) {
            std::cout << "[Bus] #" << tick_count_ << " | " << rec.symbol
                      << " | Trading Day: " << rec.trading_day
                      << " | Update Time: " << rec.update_time
                      << " | Last: " << rec.last_price << " | Vol: " << rec.volume << std::endl;
        }
        tick_count_++;

        bus_->publish(EVENT_MARKET_DATA, const_cast<TickRecord*>(&rec));
    }

    EventBus* bus_ = nullptr;
    std::string file_path_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    uint64_t tick_count_ = 0; // 计数器
};

EXPORT_MODULE(ReplayModule)
