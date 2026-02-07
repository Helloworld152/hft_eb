# HFT-EDA WebSocket 交互协议 (v1.0)

## 1. 核心约定
- **编码**：UTF-8。
- **数值精度**：`client_id` 为 18 位十进制整数，JavaScript 端处理时建议作为 **String** 以防精度丢失。
- **时间戳**：Unix 毫秒。

## 2. 下行指令 (Client -> Server)

### 下单 (action: "order")
```json
{
  "action": "order",
  "account_id": "247060",
  "symbol": "au2606",
  "direction": "B",
  "offset": "O",
  "price": 480.5,
  "volume": 1
}
```

### 撤单 (action: "cancel")
```json
{
  "action": "cancel",
  "client_id": 202602041234010001,
  "account_id": "247060",
  "symbol": "au2606"
}
```

## 3. 上行推送 (Server -> Client)

### 状态回报 (type: "rtn")
```json
{
  "type": "rtn",
  "client_id": 202602041234010001,
  "account_id": "247060",
  "order_ref": "010000000123",
  "order_sys_id": "12345678",
  "symbol": "au2606",
  "direction": "B",
  "offset": "O",
  "status": "3",
  "vol_total": 1,
  "vol_traded": 0,
  "price": 480.5,
  "msg": "已报入交易所",
  "timestamp": 1707012345678
}
```

### 成交回报 (type: "trade")
```json
{
  "type": "trade",
  "client_id": 202602041234010001,
  "trade_id": "999999",
  "price": 480.4,
  "volume": 1,
  "timestamp": 1707012345678
}
```

### 持仓快照 (type: "pos_snapshot")
```json
{
  "type": "pos_snapshot",
  "data": [
    {
      "account_id": "247060",
      "symbol": "au2606",
      "long_td": 1,
      "long_yd": 0,
      "long_price": 480.1,
      "long_pnl": 120.5,
      "short_td": 0,
      "short_yd": 0,
      "short_price": 0.0,
      "short_pnl": 0.0,
      "pnl": 120.5
    }
  ]
}
```
