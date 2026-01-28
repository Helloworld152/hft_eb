#pragma once

#include <vector>
#include <memory>
#include <string>
#include "framework.h"

// 前向声明，隐藏实现细节
class EventBusImpl;
struct PluginHandle;

class HftEngine {
public:
    HftEngine();
    ~HftEngine();

    // 禁止拷贝和赋值
    HftEngine(const HftEngine&) = delete;
    HftEngine& operator=(const HftEngine&) = delete;

    // 加载配置并初始化插件
    // 返回 true 表示成功，false 表示失败
    bool loadConfig(const std::string& config_path);

    // 启动所有插件
    void start();

    // 运行主循环（阻塞，直到达到指定持续时间）
    void run(int duration_sec);

    // 停止所有插件并清理资源
    void stop();

private:
    // PImpl idiom to hide implementation details
    std::unique_ptr<EventBusImpl> bus_;
    std::vector<std::shared_ptr<PluginHandle>> plugins_;
    bool is_running_;
};
