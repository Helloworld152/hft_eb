#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <functional>
#include <iostream>
#include <array>
#include "../core/include/protocol.h" // 引入 TickRecord 定义

// ==========================================
// 1. 基础数据结构
// ==========================================

// 事件类型
enum EventType {
    EVENT_MARKET_DATA = 0, // 行情
    EVENT_ORDER_REQ,       // 报单请求 (策略意图)
    EVENT_ORDER_SEND,      // 报单指令 (经风控批准)
    EVENT_RTN_ORDER,       // 处理后的报单回报 (OrderManager -> Strategy/Position)
    EVENT_RTN_TRADE,       // 处理后的成交回报 (OrderManager -> Strategy/Position)
    EVENT_RTN_RAW_ORDER,   // 原始报单回报 (TradeModule -> OrderManager)
    EVENT_RTN_RAW_TRADE,   // 原始成交回报 (TradeModule -> OrderManager)
    EVENT_POS_UPDATE,      // 持仓更新 (PositionModule -> Others)
    EVENT_RSP_POS,         // 持仓查询回报 (CtpReal -> PositionModule)
    EVENT_KLINE,           // K线数据
    EVENT_SIGNAL,          // 因子信号 (Factor Signal)
    EVENT_QRY_POS,         // 主动查询持仓请求
    EVENT_QRY_ACC,         // 主动查询资金请求
    EVENT_CANCEL_REQ,      // 撤单请求 (策略发出)
    EVENT_CANCEL_SEND,     // 撤单指令 (OrderManager 发出)
    EVENT_ACC_UPDATE,      // 资金更新回报
    EVENT_CONN_STATUS,     // 连接状态更新
    EVENT_LOG,             // 日志
    MAX_EVENTS
};

// ==========================================
// 2. 事件总线 (Host 提供)
// ==========================================
class EventBus {
public:
    using Handler = std::function<void(void*)>;
    
    virtual ~EventBus() = default;

    virtual void subscribe(EventType type, Handler handler) = 0;
    virtual void publish(EventType type, void* data) = 0;
    
    // 安全退出：清空所有回调
    virtual void clear() = 0;
};

// ==========================================
// 3. 定时器服务 (由 Engine 实现并传入需定时的模块)
// ==========================================
class ITimerService {
public:
    virtual ~ITimerService() = default;
    // 每 interval_sec 秒执行一次 callback；phase_sec 为相位(0~interval_sec-1)，首次触发在 total_seconds % interval_sec == phase_sec 的时刻
    virtual void add_timer(int interval_sec, std::function<void()> callback, int phase_sec = 0) = 0;
};

// ==========================================
// 4. 插件接口 (Plugin 实现)
// ==========================================
using ConfigMap = std::unordered_map<std::string, std::string>;

class IModule {
public:
    virtual ~IModule() = default;
    
    // 初始化：bus、config 必填；timer_svc 非空时可向 Engine 注册定时任务
    virtual void init(EventBus* bus, const ConfigMap& config, ITimerService* timer_svc = nullptr) = 0;
    
    virtual void start() {}
    virtual void stop() {}
};

// ==========================================
// 5. 二级策略插件接口 (Strategy Tree Leaf)
// ==========================================
struct StrategyContext {
    std::string strategy_id;
    std::function<void(const OrderReq&)> send_order;
    std::function<void(const SignalRecord&)> send_signal; // 新增：支持发送信号
    std::function<void(const char* msg)> log;
};

class IStrategyNode {
public:
    virtual ~IStrategyNode() = default;
    virtual void init(StrategyContext* ctx, const ConfigMap& config) = 0;
    virtual void onTick(const TickRecord* tick) = 0;
    virtual void onKline(const KlineRecord* kline) = 0; // 处理 K线数据
    virtual void onSignal(const SignalRecord* signal) = 0; // 处理因子信号
    virtual void onOrderUpdate(const OrderRtn* rtn) = 0;
};

// ==========================================
// 6. 匾出符号约定
// ==========================================
// 每个 .so 必须实现这个函数来创建模块实例
typedef IModule* (*CreateModuleFunc)();
#define EXPORT_MODULE(CLASS_NAME) \
    extern "C" { \
        IModule* create_module() { return new CLASS_NAME(); } \
    }

// 每个策略 .so 必须实现
typedef IStrategyNode* (*CreateStrategyFunc)();
#define EXPORT_STRATEGY(CLASS_NAME) \
    extern "C" { \
        IStrategyNode* create_strategy() { return new CLASS_NAME(); } \
    }
    