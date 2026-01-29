#include <iostream>
#include <thread>
#include "../include/engine.h"

int main(int argc, char* argv[]) {
    // 0. 基础环境准备
    std::thread([]{}).join(); // Force pthread init

    std::string config_path = "config.json";
    if (argc > 1) config_path = argv[1];

    // 1. 创建引擎实例
    HftEngine engine;

    // 2. 加载配置
    if (!engine.loadConfig(config_path)) {
        return 1;
    }

    engine.run();

    engine.stop();

    return 0;
}
