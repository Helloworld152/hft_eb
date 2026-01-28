# HFT 风控模块设计文档 (Risk Management Design)

## 核心理念：交易前风控 (Pre-Trade Risk Check)
风控模块充当策略与执行网关之间的**强制拦截器**。所有交易请求必须经过风控模块的批准才能发送至交易所，这是高频交易系统的最后一道防线。

## 事件流架构 (Event Flow)

### 当前流程 (不安全)
```mermaid
graph LR
    Strategy(策略) -->|EVENT_ORDER_REQ| CtpReal(实盘交易)
    CtpReal -->|API Call| Exchange(交易所)
```

### 提议流程 (安全)
```mermaid
graph LR
    Strategy["策略"] -->|EVENT_ORDER_REQ| RiskModule{"风控模块"}
    RiskModule -->|"通过: EVENT_ORDER_SEND"| CtpReal["实盘交易"]
    RiskModule -->|"拒绝: EVENT_LOG"| Logger["日志"]
    CtpReal -->|"API Call"| Exchange["交易所"]
```

## 实施步骤

### 1. 更新框架 (`framework.h`)
- 新增事件类型：`EVENT_ORDER_SEND`。
- `EVENT_ORDER_REQ`：代表交易**意图**（由策略生成）。
- `EVENT_ORDER_SEND`：代表已**授权**的指令（由风控生成）。

### 2. 创建风控模块 (`modules/risk/risk_module.cpp`)
#### 职责：
1.  **订阅** `EVENT_ORDER_REQ`。
2.  **订阅** `EVENT_MARKET_DATA` (预留位置)。
3.  **执行检查**：
    - **流控 (Flow Control)**：限制每秒最大报单数。当前实现采用**滑动窗口 (Sliding Window)** 算法，通过 `std::vector` 存储最近一秒内的时间戳，超过限制则拦截。
    - **线程安全**：使用 `std::mutex` 保护时间戳向量的并发读写。
4.  **发布**：
    - 如果通过：记录当前时间戳，并发布 `EVENT_ORDER_SEND`。
    - 如果拦截：打印错误日志并拒绝转发。

### 5. 监控支持 (State Exposure)
风控模块需要维护一组原子计数器（如 `rejected_count`, `last_flow_rate`），并将这些指标更新到全局 `SystemState` 快照中，以便监控模块定时拉取。

### 4. 更新执行模块 (`modules/ctp_real/ctp_real_module.cpp`)
- 将订阅从 `EVENT_ORDER_REQ` 改为 `EVENT_ORDER_SEND`。
- 这确保了实盘模块**只**执行经过批准的订单。

### 4. 配置文件 (`config.json`)
在插件链中加入 `mod_risk`：

```json
{
    "plugins": [
        { "name": "CTP_Market", ... },
        { "name": "Grid_Strategy", ... },
        { "name": "Risk_Control", "library": "./libmod_risk.so", "config": { "max_orders_per_sec": "5", "price_deviation": "0.02" } },
        { "name": "CTP_Trade_Real", ... }
    ]
}
```