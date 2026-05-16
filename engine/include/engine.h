#pragma once

#include <vector>
#include <memory>
#include <string>
#include <cstdint>
#include "framework.h"
#include "core_state.h"

class EventBusImpl;
struct PluginHandle;
class EngineTimerAdapter;
class MarketSnapshot; // 前置声明

// Engine 内部定时任务项
struct TimerTask {
    int interval_sec;
    uint64_t next_fire;
    StaticDelegate<void()> sd_callback;
};

class HftEngine {
    friend class EngineTimerAdapter;
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

    // 运行主循环 (阻塞，直到收到信号或达到结束时间)
    void run();

    // 停止所有插件并清理资源
    void stop();

private:
    std::unique_ptr<EventBusImpl> bus_;
    std::vector<std::shared_ptr<PluginHandle>> plugins_;
    bool is_running_;
    std::string start_time_;
    int end_time_seconds_ = -1;  // 预解析的结束时间(秒)，-1 表示未设置

    // 统一定时器：任务列表 + 运行秒数，由 run() 每秒驱动
    std::vector<TimerTask> timer_tasks_;
    uint64_t total_seconds_ = 0;
    std::unique_ptr<ITimerService> timer_svc_;
    std::unique_ptr<core::PositionService> position_service_;
    std::unique_ptr<core::OrderService> order_service_;
    std::unique_ptr<core::AccountService> account_service_;

    // 定时器注册入口
    void add_timer_impl(int interval_sec, StaticDelegate<void()> cb, int phase_sec = 0);
    void run_due_timers();

    std::unique_ptr<MarketSnapshot> snapshot_impl_;
};
