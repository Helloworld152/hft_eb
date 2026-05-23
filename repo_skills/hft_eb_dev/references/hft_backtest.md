# 回测开发

## 事件流

```
tick → Replay → EVENT_MARKET_DATA
  ├─ PyStrategy.on_tick() → buy() → py_send_order → EVENT_ORDER_REQ
  │    └─ Risk → EVENT_ORDER_SEND
  │         └─ SimTrade → 撮合 → EVENT_RTN_ORDER + EVENT_RTN_TRADE
  │              └─ Recorder → CSV
  │              └─ PyStrategy.on_order() / on_trade()
  └─ SimTrade.onTick() → 行情驱动撮合挂单队列
```

撤单：`cancel_order() → py_cancel_order → EVENT_CANCEL_REQ → Risk → EVENT_CANCEL_SEND → SimTrade`

## ID 体系

| ID | 谁生成 | 用途 |
|----|--------|------|
| `client_id` (uint64_t) | OrderIDGenerator::next_id()，py_strategy 发单前 | 主键，buy() 返回，cancel 用它 |
| `order_sys_id` (uint64_t→"SIM1") | SimTrade::next_order_sys_id_ | CSV 显示，引擎 key |

`order_ref` 已废弃，不生成、不存、不写 CSV。

## 三层架构

```
hft_backtest/     Python 策略层: BaseStrategy, objects, engine
py_strategy/      C++ 桥接: py_send_order, py_cancel_order, 回调 Python
sim_trade/        撮合+账户: TickMatchingEngine, client_to_sys_, cost_map_
recorder/         CSV 输出: orders.csv, trades.csv, accounts.csv
```

## SimTrade 撮合

`TickMatchingEngine` — IntrusivePool + 双向链表，per-symbol FIFO：
- submit: 扫对手队列 → 不成交挂单 (status='3')
- apply_tick: 扫挂单队列 vs tick 5档盘口 → 成交 (status='0')
- cancel: O(1) 链表删除 (status='5')

账户：开仓记录 cost_map_，平仓结算 PnL 到 balance_。

## 常见陷阱

| 陷阱 | 后果 | 正确做法 |
|------|------|----------|
| IntrusivePool 用 std::string 字段 | 编译失败 (not trivially destructible) | 用 char[N] |
| 往 __init__ 传 send_order/cancel_order | 策略签名暴露内部细节 | PyObject_SetAttrString 注入 |
| 改 Recorder CSV header 不同步数据行 | CSV 列数错位 | 同一次改动改 header+数据行 |
| 改 BaseStrategy.__init__ 签名 | 所有策略报错 | 只收 config，不改签名 |
| client_id 为 0 | 引擎拒绝重复 ID | py_strategy 发单前调 next_id() |
