#include "../../include/framework.h"
#include "../../core/include/ring_buffer.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <immintrin.h>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

struct SignalRow {
    uint64_t timestamp = 0;
    double value = 0.0;
    char symbol[32] = {0};
    char factor_name[32] = {0};
    char source_id[32] = {0};
};

}  // namespace

class SignalCsvModule : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config, ITimerService* timer_svc = nullptr) override {
        (void)timer_svc;
        bus_ = bus;

        if (config.find("output_path") != config.end()) {
            output_path_ = config.at("output_path");
        }
        if (config.find("capacity") != config.end()) {
            capacity_ = std::stoull(config.at("capacity"));
        }
        if (config.find("flush_every") != config.end()) {
            flush_every_ = std::stoull(config.at("flush_every"));
        }
        if (config.find("include_header") != config.end()) {
            include_header_ = (config.at("include_header") == "true" || config.at("include_header") == "1");
        }
        if (config.find("log_interval_ms") != config.end()) {
            log_interval_ms_ = std::stoull(config.at("log_interval_ms"));
        }

        ring_ = std::make_unique<SpscQueue<SignalRow>>(capacity_);

        bus_->subscribe(EVENT_SIGNAL,
                        StaticDelegate<void(void*)>::bind<SignalCsvModule, &SignalCsvModule::on_signal_event>(this));

        std::cout << "[SignalCsv] Initialized. output=" << output_path_
                  << ", capacity=" << ring_->capacity()
                  << ", flush_every=" << flush_every_ << std::endl;
    }

    void start() override {
        if (running_) return;
        running_ = true;
        writer_ = std::thread([this]() { this->writer_loop(); });
    }

    void stop() override {
        running_ = false;
        if (writer_.joinable()) writer_.join();
    }

private:
    void on_signal_event(void* d) {
        on_signal(static_cast<SignalRecord*>(d));
    }

    void on_signal(const SignalRecord* sig) {
        if (!sig) return;
        SignalRow row;
        row.timestamp = sig->timestamp;
        row.value = sig->value;
        std::strncpy(row.symbol, sig->symbol, sizeof(row.symbol) - 1);
        std::strncpy(row.factor_name, sig->factor_name, sizeof(row.factor_name) - 1);
        std::strncpy(row.source_id, sig->source_id, sizeof(row.source_id) - 1);

        // Non-overwrite mode: spin-wait when full (no drop, no overwrite).
        while (ring_->size() >= ring_->capacity()) {
            _mm_pause();
        }
        while (!ring_->push(row)) {
            _mm_pause();
        }
    }

    void writer_loop() {
        std::ofstream out(output_path_, std::ios::out | std::ios::trunc);
        if (!out.is_open()) {
            std::cerr << "[SignalCsv] Failed to open output file: " << output_path_ << std::endl;
            return;
        }

        if (include_header_) {
            out << "timestamp,symbol,factor_name,value,source_id\n";
        }

        uint64_t written = 0;
        auto last_log = std::chrono::steady_clock::now();
        auto last_flush = written;

        while (running_ || ring_->size() > 0) {
            SignalRow row;
            bool any = false;
            while (ring_->pop(row)) {
                any = true;
                out << row.timestamp << ','
                    << row.symbol << ','
                    << row.factor_name << ','
                    << row.value << ','
                    << row.source_id << '\n';
                written++;

                if (flush_every_ > 0 && (written - last_flush) >= flush_every_) {
                    out.flush();
                    last_flush = written;
                }
            }

            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log).count() >=
                static_cast<long long>(log_interval_ms_)) {
                size_t depth = ring_->size();
                std::cout << "[SignalCsv] written=" << written
                          << " depth=" << depth
                          << std::endl;
                last_log = now;
            }

            if (!any) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        size_t final_depth = ring_->size();
        std::cout << "[SignalCsv] final_written=" << written
                  << " final_depth=" << final_depth
                  << std::endl;

        out.flush();
        out.close();
    }

private:
    EventBus* bus_ = nullptr;
    std::unique_ptr<SpscQueue<SignalRow>> ring_;
    std::thread writer_;
    std::atomic<bool> running_{false};

    std::string output_path_ = "../log/signal.csv";
    size_t capacity_ = (1ULL << 20);
    size_t flush_every_ = 5000;
    bool include_header_ = true;
    uint64_t log_interval_ms_ = 1000;
};

EXPORT_MODULE(SignalCsvModule)
