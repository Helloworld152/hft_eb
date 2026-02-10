#include "Recorder.cpp"
#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>

static std::atomic<bool> g_shutdown(false);
static TickRecorder* g_recorder = nullptr;

void signal_handler(int signum) {
    std::cout << "\n[System] Caught signal " << signum << ", shutting down..." << std::endl;
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

    std::cout << "========================================" << std::endl;
    std::cout << "  OmniQuant HFT Market Data Recorder    " << std::endl;
    std::cout << "  Config: " << config_path << std::endl;
    std::cout << "========================================" << std::endl;
    
    try {
        // 注册信号处理
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        // 使用 unique_ptr 在堆上分配，避免栈溢出（RingBuffer 占用约 13MB，超过默认栈限制）
        auto recorder = std::make_unique<TickRecorder>(config_path);
        g_recorder = recorder.get();
        recorder->start();

        std::cout << "Recording... Ctrl+C or out-of-range to shutdown." << std::endl;

        while (!g_shutdown) {
            if (!recorder->is_in_time_range()) {
                std::cout << "[System] Current time is out of range, scheduled shutdown..." << std::endl;
                g_shutdown = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "Stopping recorder..." << std::endl;
        recorder->stop();
        g_recorder = nullptr;
        std::cout << "Done." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}