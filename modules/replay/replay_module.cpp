#include "framework.h"
#include "protocol.h"
#include "mmap_util.h"
#include "market_snapshot.h"
#include <memory>
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
            LOG_ERROR("[Replay] 配置文件中未指定 data_file!");
        }

        if (config.find("debug") != config.end()) {
            debug_ = (config.at("debug") == "true" || config.at("debug") == "1");
        }

        // 读取最大容量配置（用于实时读取场景，0 表示使用 meta 文件中的 capacity）
        if (config.find("max_capacity") != config.end()) {
            max_capacity_ = std::stoull(config.at("max_capacity"));
        } else {
            max_capacity_ = 0;  // 默认使用 meta 文件中的 capacity
        }

        if (config.find("idle_stop_sec") != config.end()) {
            idle_stop_sec_ = std::stoi(config.at("idle_stop_sec"));
        }

        if (max_capacity_ > 0) {
            LOG_INFO("[Replay] 模块初始化完成。Mmap 基础路径: {} Max Capacity: {} records (~{} GB)",
                     file_path_,
                     max_capacity_,
                     (max_capacity_ * sizeof(TickRecord) / (1024.0 * 1024.0 * 1024.0)));
        } else {
            LOG_INFO("[Replay] 模块初始化完成。Mmap 基础路径: {}", file_path_);
        }

        bus_->subscribe(EVENT_POLL_REPLAY,
                        StaticDelegate<void(void*)>::bind<ReplayModule, &ReplayModule::on_poll_replay>(this));
    }

    void start() override {
        running_ = true;
    }

    void stop() override {
        running_ = false;
        reader_.reset();
        MarketSnapshot::instance().clear();
        LOG_INFO("[Replay] Final Ticks: {}", tick_count_);
    }

private:
    void on_poll_replay(void*) {
        poll_once();
    }

    void poll_once() {
        if (!running_) return;

        if (!ensure_reader_ready()) {
            return;
        }

        constexpr size_t BATCH_SIZE = 16;
        const TickRecord* batch_ptrs[BATCH_SIZE];
        const size_t batch_count = reader_->read_batch(batch_ptrs, BATCH_SIZE);

        if (batch_count == 0) {
            maybe_stop_on_idle();
            return;
        }

        if (debug_ && tick_count_ == 0) {
            replay_start_t_ = std::chrono::high_resolution_clock::now();
        }

        for (size_t i = 0; i < batch_count; ++i) {
            publish_tick(*batch_ptrs[i]);
        }
        perf_logged_ = false;
        last_data_time_ = std::chrono::steady_clock::now();
    }

    bool ensure_reader_ready() {
        if (reader_) return true;

        auto now = std::chrono::steady_clock::now();
        if (now - last_open_attempt_ < std::chrono::seconds(1)) {
            return false;
        }
        last_open_attempt_ = now;

        try {
            reader_ = std::make_unique<MmapReader<TickRecord>>(file_path_, max_capacity_);
            last_data_time_ = now;
            perf_logged_ = false;
            LOG_INFO("[Replay] 已连接到 Mmap 管道，开始回放...");
            return true;
        } catch (const std::exception& e) {
            LOG_INFO("[Replay] 等待数据源 ({})... {}", file_path_, e.what());
            return false;
        }
    }

    void maybe_stop_on_idle() {
        if (idle_stop_sec_ > 0) {
            auto now = std::chrono::steady_clock::now();
            auto idle_sec = std::chrono::duration_cast<std::chrono::seconds>(now - last_data_time_).count();
            if (idle_sec >= idle_stop_sec_) {
                LOG_INFO("[Replay] Idle timeout reached ({}s). Stopping engine.", idle_stop_sec_);
                bus_->publish(EVENT_ENGINE_STOP, nullptr);
                running_ = false;
                return;
            }
        }

        if (debug_ && tick_count_ > 0 && !perf_logged_) {
            auto end_t = std::chrono::high_resolution_clock::now();
            auto cost_us = std::chrono::duration_cast<std::chrono::microseconds>(end_t - replay_start_t_).count();
            LOG_INFO("[Replay] Finished/Paused. Ticks: {}, Cost: {} us", tick_count_, cost_us);
            perf_logged_ = true;
        }
    }

    void publish_tick(const TickRecord& rec) {
        // 采样打印：前5条必打，之后每50条打一次
        // Debug mode: Use string comparison for robustness (no dependency on SymbolManager loading)
        if (debug_ && (tick_count_ < 5 || (tick_count_ % 10 == 0 && strcmp(rec.symbol, "au2606") == 0))) {
            LOG_DEBUG("[Bus] #{} | {} (ID:{}) | Trading Day: {} | Update Time: {} | Last: {} | Vol: {}",
                      tick_count_,
                      rec.symbol,
                      rec.symbol_id,
                      rec.trading_day,
                      rec.update_time,
                      rec.last_price,
                      rec.volume);
        }
        tick_count_++;

        MarketSnapshot::instance().update(rec);
        bus_->publish(EVENT_MARKET_DATA, const_cast<TickRecord*>(&rec));
    }

    EventBus* bus_ = nullptr;
    std::string file_path_;
    std::unique_ptr<MmapReader<TickRecord>> reader_;
    bool running_ = false;
    bool debug_ = false;
    uint64_t tick_count_ = 0; // 计数器
    uint64_t max_capacity_ = 0; // 最大容量（0 表示使用 meta 文件中的 capacity）
    int idle_stop_sec_ = 0; // 空闲超时停止（秒）
    std::chrono::steady_clock::time_point last_data_time_;
    std::chrono::steady_clock::time_point last_open_attempt_{};
    std::chrono::high_resolution_clock::time_point replay_start_t_{};
    bool perf_logged_ = false;
};

EXPORT_MODULE(ReplayModule)
