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
    EVENT_ORDER_SEND,      // 报单指令 (经风控扶准)
    EVENT_RTN_ORDER,       // 报单回报 (交易所状态)
    EVENT_RTN_TRADE,       // 成交回报 (交易所成交)
    EVENT_POS_UPDATE,      // 持仓更新
    EVENT_LOG,             // 日志
    MAX_EVENTS
};

// 事件载荷 (Payload)
// 全系统直接使用 core/protocol.h 中的 TickRecord 作为标准行情结构
// 彻底废弃 MarketData
// 彻底底弃 MarketData

struct OrderReq {
    char symbol[32];
    char direction;   // 'B'uy or 'S'ell
    char offset_flag; // 'O'pen, 'C'lose, 'T'oday (上期所平今)
    double price;
    int volume;
};

// 报单回报
struct OrderRtn {
    char order_ref[13];
    char symbol[32];
    char direction;      // 'B'/'S'
    char offset_flag;    // 'O'/'C'/'T'
    double limit_price;
    int volume_total;    // 报单总量
    int volume_traded;   // 已成交量
    char status;         // '0':全部成交, '1':部分成交, '3':未成交, '5':已退单
    char status_msg[81];
};

// 成交回报
struct TradeRtn {
    char symbol[32];
    char direction;      // 'B'/'S'
    char offset_flag;    // 'O'/'C'/'T'
    double price;
    int volume;
    char trade_id[21];
    char order_ref[13];
};

// 持仓明细
struct PositionDetail {
    char symbol[32];
    
    // 多头
    int long_td;
    int long_yd;
    double long_avg_price;
    
    // 假头
    int short_td;
    int short_yd;
    double short_avg_price;
    
    double net_pnl;
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
// 3. 插件接口 (Plugin 实现)
// ==========================================
using ConfigMap = std::unordered_map<std::string, std::string>;

class IModule {
public:
    virtual ~IModule() = default;
    
    // 初始化：Host 会把 bus 指针传进来
    virtual void init(EventBus* bus, const ConfigMap& config) = 0;
    
    // 可选：启动/停止
    virtual void start() {}
    virtual void stop() {}
};

// ==========================================
// 4. 二级策略插件接口 (Strategy Tree Leaf)
// ==========================================
struct StrategyContext {
    std::string strategy_id;
    std::function<void(const OrderReq&)> send_order;
    std::function<void(const char* msg)> log;
};

class IStrategyNode {
public:
    virtual ~IStrategyNode() = default;
    virtual void init(StrategyContext* ctx, const ConfigMap& config) = 0;
    virtual void onTick(const TickRecord* tick) = 0;
    virtual void onOrderUpdate(const OrderRtn* rtn) = 0;
};

// ==========================================
// 5. 匾出符号约定
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
