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

    // 3. 启动并运行
    // 默认运行 5 秒，或者可以修改 engine 接口支持一直运行直到信号中断
    engine.run(5);

    // 4. 停止（析构函数也会调用 stop，显式调用更清晰）
    engine.stop();

    return 0;
}
