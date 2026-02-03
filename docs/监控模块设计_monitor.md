# Monitor Module Design (WebSocket & Multi-Account)

## 1. 概述 (Overview)
`MonitorModule` 是高频交易系统的“眼睛”和“遥控器”，负责将核心交易数据（行情、订单、成交、持仓、资金）实时推送到外部（GUI、Web前端、监控脚本），并支持接收来自外部的简单交易指令（下单、撤单）。

本设计涵盖了以下关键特性：
1.  **WebSocket 服务**: 提供全双工实时通信。
2.  **多账户支持 (Multi-Account)**: 支持同时监控和操作多个交易账户。
3.  **鉴权机制 (Auth)**: 基于静态 Token 的轻量级安全认证。
4.  **ZMQ 广播**: 兼容传统的 ZMQ SUB 模式监控。

## 2. 内存模型 (Memory Model)

### 2.1 数据结构
模块内部维护两级缓存结构，通过 `Account ID` 进行物理隔离。

```cpp
// 1. 持仓缓存 (双层映射)
// Layer 1: Account ID (std::string) -> 账户维度
// Layer 2: Symbol ID (uint64_t)     -> 合约维度
using PositionMap = std::unordered_map<uint64_t, PositionDetail>;
std::unordered_map<std::string, PositionMap> pos_cache_;

// 2. 资金缓存
// Key: Account ID
std::unordered_map<std::string, AccountDetail> acc_cache_;
```

### 2.2 线程安全
引入 `std::mutex data_mtx_` 保护上述所有缓存的读写操作。

## 3. 通信协议 (WebSocket/ZMQ JSON Protocol)

所有业务层面的 JSON 消息**必须**包含 `account_id` 字段。

### 3.1 下行推送 (Server -> Client)

#### A. 资金更新 (Account Update)
```json
{
  "type": "account",
  "account_id": "80001",      // [关键] 区分账户
  "balance": 100500.0,
  "available": 50200.0,
  "margin": 2000.0,
  "pnl": 300.0
}
```

#### B. 持仓快照 (Position Snapshot)
采用**扁平化列表**格式，每条记录携带账户归属。

```json
{
  "type": "pos_snapshot",
  "data": [
    {
      "account_id": "80001",
      "symbol": "rb2405",
      "long_td": 2,
      "short_td": 0,
      "net_pnl": 150.0
    },
    {
      "account_id": "Sim999",
      "symbol": "hc2405",
      "long_td": 0,
      "short_td": 5,
      "net_pnl": -50.0
    }
  ]
}
```

#### C. 订单/成交回报 (Order/Trade Return)
```json
{
  "type": "rtn",              // 或 "trade"
  "account_id": "80001",      // [关键]
  "order_ref": "1002",
  "symbol": "rb2405",
  "status": "0",              // "0": 全部成交
  "msg": "All Traded"
}
```

#### D. 实时行情 (Market Data)
```json
{
  "type": "tick",
  "symbol": "rb2405",
  "price": 3650.0,
  "volume": 123456
}
```

### 3.2 上行指令 (Client -> Server)

客户端在发送控制指令时，必须指定操作的目标账户。

#### A. 下单指令 (Order Action)
```json
{
  "action": "order",
  "account_id": "80001",      // [必填] 指定使用哪个账户下单
  "symbol": "rb2405",
  "direction": "B",           // "B"uy, "S"ell
  "offset": "O",              // "O"pen, "C"lose
  "price": 3650.0,
  "volume": 1
}
```

#### B. 撤单指令 (Cancel Action)
```json
{
  "action": "cancel",
  "account_id": "80001",      // [必填]
  "symbol": "rb2405",
  "order_ref": "1002"
}
```

## 4. 安全鉴权 (Authentication)

在模块配置文件中，通过 `auth_token` 字段启用鉴权。

### 4.1 配置
```yaml
plugins:
  - name: WebSocket_Monitor
    library: "../bin/libmod_monitor.so"
    config:
      ws_port: 8888
      # 如果此项缺失或为空，则禁用鉴权（允许所有连接）
      auth_token: "hft_secret_2026"
```

### 4.2 认证方式
服务端支持以下两种方式传递凭证，两者满足其一即可：

1.  **HTTP Header (推荐)**
    *   Header Key: `X-Auth-Token`
    *   Header Value: 配置文件中定义的 Token。

2.  **Query Parameter**
    *   URL: `ws://<ip>:<port>/?token=<token>`

### 4.3 验证逻辑
在 WebSocket `Open` 事件中：
- 检查 Token 是否匹配。
- **匹配**: 建立连接，立即发送持仓快照。
- **不匹配**: 关闭连接 (Code 4001)。

## 5. 核心逻辑 (Implementation Logic)

### 5.1 事件处理 (`on_event`)
- **POS_UPDATE**:
  - 解析 `account_id`。
  - 懒加载更新 `pos_cache_[acc_id][sym_id]`。
  - 标记 `pos_dirty = true`。
- **ACC_UPDATE**:
  - 更新 `acc_cache_[acc_id]`。
- **ORDER_REQ** (来自 WS):
  - 解析 JSON 中的 `account_id`，填充至 `OrderReq`。
  - 发布 `EVENT_ORDER_REQ` 到总线。

### 5.2 节流推送 (Debounce)
为了避免高频持仓更新淹没前端，采用 500ms 节流机制。当 `pos_dirty` 为真且距离上次推送超过 500ms 时，构建全量快照 JSON 推送。

## 6. ZMQ 兼容
模块保留了 ZMQ PUB 套接字，用于向旧版监控工具或 Python 脚本广播相同的 JSON 数据流。