#include "Recorder.cpp"
#include <iostream>

int main(int argc, char* argv[]) {
    std::string config_path = "../conf/config.json";
    if (argc > 1) {
        config_path = argv[1];
    }

    std::cout << "========================================" << std::endl;
    std::cout << "  OmniQuant HFT Market Data Recorder    " << std::endl;
    std::cout << "  Config: " << config_path << std::endl;
    std::cout << "========================================" << std::endl;
    
    try {
        // 使用 unique_ptr 在堆上分配，避免栈溢出（RingBuffer 占用约 13MB，超过默认栈限制）
        auto recorder = std::make_unique<TickRecorder>(config_path);
        recorder->start();

        std::cout << "Recording... Press ENTER to shutdown." << std::endl;
        std::cin.get();

        std::cout << "Stopping recorder..." << std::endl;
        recorder->stop();
        std::cout << "Done." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}