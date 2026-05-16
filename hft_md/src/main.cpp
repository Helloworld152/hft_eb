#include "Recorder.h"
#include "logging.h"
#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <memory>
#include <string>

static std::atomic<bool> g_shutdown(false);
static TickRecorder* g_recorder = nullptr;

void signal_handler(int signum) {
    (void)signum;
    g_shutdown = true;
    if (g_recorder) {
        g_recorder->stop();
    }
}

int main(int argc, char* argv[]) {
    std::string config_path = "../conf/config.yaml";
    if (argc > 1) {
        config_path = argv[1];
    }
    
    try {
        hft::logging::init_logging_from_yaml_file(config_path, "hft_recorder");
        // 注册信号处理
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        // 使用 unique_ptr 在堆上分配，避免栈溢出（RingBuffer 占用约 13MB，超过默认栈限制）
        auto recorder = std::make_unique<TickRecorder>(config_path);
        g_recorder = recorder.get();
        recorder->start();

        LOG_INFO("========================================");
        LOG_INFO("OmniQuant HFT Market Data Recorder");
        LOG_INFO("Config: {}", config_path);
        LOG_INFO("========================================");
        LOG_INFO("Recording... Ctrl+C or out-of-range to shutdown.");

        while (!g_shutdown) {
            if (!recorder->is_in_time_range()) {
                LOG_INFO("[System] Current time is out of range, scheduled shutdown...");
                g_shutdown = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        LOG_INFO("Stopping recorder...");
        recorder->stop();
        g_recorder = nullptr;
        LOG_INFO("Done.");
        hft::logging::shutdown_logging();
    } catch (const std::exception& e) {
        LOG_ERROR("Error: {}", e.what());
        return 1;
    }
    
    return 0;
}
