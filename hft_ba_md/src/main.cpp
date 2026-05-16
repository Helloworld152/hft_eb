#include "Recorder.h"
#include "logging.h"
#include <atomic>
#include <csignal>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

static std::atomic<bool> g_shutdown(false);
static BaTickRecorder* g_recorder = nullptr;

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
        hft::logging::init_logging_from_yaml_file(config_path, "hft_ba_recorder");
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        auto recorder = std::make_unique<BaTickRecorder>(config_path);
        g_recorder = recorder.get();
        recorder->start();

        LOG_INFO("========================================");
        LOG_INFO("OmniQuant HFT Binance MD Snapshot");
        LOG_INFO("Config: {}", config_path);
        LOG_INFO("========================================");
        LOG_INFO("Snapshotting... Ctrl+C or out-of-range to shutdown.");

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
