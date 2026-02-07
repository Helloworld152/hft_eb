# 订单管理模块 (Order Manager) 设计

## 1. 核心目标
- **唯一性**：跨进程、跨重启、跨交易日全局唯一。
- **全生命周期追踪**：从 `OrderReq` 生成开始，覆盖内部风控、柜台、交易所及成交/撤单全路径。
- **鲁棒性**：在柜台断线重连或订单因异常（如被风控拦截）未到达交易所时，确保内部标识（LocalID）依然有效且可追溯。
- **高性能**：采用 64 位整数标识，支持亚微秒级的哈希查找与状态更新。

## 2. LocalID 生成方案 (uint64_t)
使用 64 位无符号整数作为 `client_id`，采用以下位段分配：

| 段名 | 位数 | 描述 | 容量/周期 |
| :--- | :--- | :--- | :--- |
| **Epoch** | 32 bit | 自 2026-01-01 起的秒数 | 可持续 ~136 年 |
| **NodeID** | 10 bit | 进程实例/节点 ID (0-1023) | 支持 1024 个独立节点 |
| **Sequence** | 22 bit | 每秒内的自增序列号 | 支持 4,194,303 笔/秒 |

**生成逻辑**：`((timestamp - EPOCH) << 32) | (node_id << 22) | (sequence++)`

## 3. 订单状态机 (Order Lifecycle)
订单在内部系统中的状态流转如下：

1. **CREATED**: 策略模块生成 `client_id` 并构造请求。
2. **PENDING_RISK**: 请求已发送至总线，等待风控模块审核。
3. **PENDING_SEND**: 风控通过，交易模块（如 CTP）已接收并准备发往柜台。
4. **SUBMITTED**: 柜台/交易所已确认接收报单。
5. **PARTIAL_FILLED**: 部分成交。
6. **FILLED**: 全部成交（终端状态）。
7. **CANCELLED**: 用户撤单成功（终端状态）。
8. **REJECTED**: 内部风控拒绝或柜台拒单（终端状态，覆盖订单未达交易所场景）。

## 4. 映射机制与抗重连设计
为了确保在收到柜台异步回报（OrderRtn/TradeRtn）时能找回原始订单，`OrderManager` 维护三张核心映射表：

- **LocalID -> OrderContext**: 
  - 存储订单的原始请求（`OrderReq`）、当前状态、成交数量、柜台参考号以及交易所系统 ID。
- **OrderRef -> LocalID**:
  - 建立柜台报单引用（本地生成）与内部 `client_id` 的绑定。用于处理柜台层面的报单回报。
- **OrderSysID -> LocalID**:
  - **关键点**：建立交易所生成的 `OrderSysID` 与内部 `client_id` 的绑定。
  - **作用**：成交回报（TradeRtn）通常携带 `OrderSysID`。在某些重连场景下，`OrderRef` 可能会失效或变得不可靠，而 `OrderSysID` 是交易所持久化的唯一凭证，是确保成交准确归属的最后防线。

## 5. 关键路径流程
1. **策略端**：调用 `OrderManager::next_id()` -> 填充 `OrderReq.client_id` -> `publish(EVENT_ORDER_REQ)`。
2. **交易端**：收到请求 -> 调用柜台 API -> 得到柜台 `OrderRef` -> 调用 `OrderManager::bind_ref(client_id, order_ref)`。
3. **柜台/交易所响应**：收到 `OrderRtn`（含 `OrderSysID`） -> 调用 `OrderManager::bind_sys_id(client_id, sys_id)`。
4. **成交回报**：收到 `TradeRtn` -> 提取 `OrderSysID` -> 查表得 `client_id` -> 更新成交状态。

## 6. 接口预览 (C++)
```cpp
struct OrderContext {
    OrderReq request;
    char order_ref[13];     // 柜台参考号
    char order_sys_id[21];  // 交易所系统 ID (SysID)
    int filled_volume;
    char status;
    uint64_t insert_time;
};

class OrderManager {
public:
    static OrderManager& instance();
    
    // 生成 ID
    uint64_t next_id();
    
    // 绑定关系
    void bind_ref(uint64_t client_id, const char* order_ref);
    void bind_sys_id(uint64_t client_id, const char* sys_id);
    
    // 查找
    uint64_t get_id_by_ref(const char* order_ref);
    uint64_t get_id_by_sys_id(const char* sys_id);
    
    // 状态管理
    void on_order_rtn(OrderRtn* rtn);
    void on_trade_rtn(TradeRtn* rtn);
};
```
