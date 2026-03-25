# 低延迟订单簿核心重构方案

## 摘要

- 新增 `OrderBook` 与 `OrderBookSnapshot`（本地 + 共享内存），以 seqlock 方式实现低延迟快照读写。
- 引擎层统一订阅 `EVENT_MARKET_DATA`，同时更新 `MarketSnapshot` 与 `OrderBookSnapshot`。
- 保留 `TickRecord` 不变，OrderBook 固定 10 档，但当前仅前 5 档有效。

## 关键实现变化

1. 核心数据结构

- 新增 `OrderBook` 结构：`symbol_id / trading_day / update_time / valid_depth / bid_price[10] / bid_volume[10] / ask_price[10] / ask_volume[10]`
- 提供 `update_from_tick` 或 `from_tick`：仅填充前 5 档，`valid_depth=5`，其余置 0

1. 订单簿快照（Local + SHM）

- 新增 `OrderBookSnapshot` 接口：`update`、`update_from_tick`、`get`、`clear`
- `LocalOrderBookSnapshot`：固定槽位数组 + seqlock
- `ShmOrderBookSnapshot`：共享内存布局 + symbol_index 映射 + seqlock

1. 引擎集成

- 初始化 `OrderBookSnapshot`（配置块 `orderbook_snapshot`）
- 全局订阅 `EVENT_MARKET_DATA`，统一更新 `MarketSnapshot` 与 `OrderBookSnapshot`

1. 回放模块调整

- `ReplayModule` 不再直接更新 `MarketSnapshot`，由引擎统一处理

1. 构建系统

- `hft_core` 增加订单簿快照实现源文件

## 订单簿更新流程（快照派生）

1. 行情源产生 `TickRecord` 并发布 `EVENT_MARKET_DATA`（例如 `ReplayModule` 或实时行情模块）。
2. 引擎在启动时注册全局订阅 `EVENT_MARKET_DATA` 的低延迟处理器（`StaticDelegate`）。
3. 处理器同步执行两件事（无堆分配，顺序固定）：
  1. `MarketSnapshot::instance().update(tick)`
  2. `OrderBookSnapshot::instance().update_from_tick(tick)`
4. `OrderBookSnapshot::update_from_tick` 内部构造 `OrderBook`：

- 填充 `symbol_id / trading_day / update_time`
- 复制前 5 档 `bid/ask` 价量
- `valid_depth = 5`，6-10 档置 0

1. `OrderBookSnapshot` 用 seqlock 写入对应 `symbol_id` 的槽位：

- `seq` 置奇数（写入中）
- 写入 `OrderBook` 数据
- `seq` 置偶数（写入完成）

1. 任何读者调用 `OrderBookSnapshot::get(symbol_id, out)`：

- 读取 `seq`，若为奇数则重试或短暂 `_mm_pause`
- 拷贝 `OrderBook`
- 再读 `seq`，一致则成功返回

## 订单簿更新流程（逐笔委托 + 逐笔成交）

1. 行情源发布逐笔委托事件（新单、撤单）和逐笔成交事件。
2. 引擎订阅这两类事件并进入订单簿更新器（低延迟处理器）。
3. 维护订单级索引：

- `order_id -> {price, qty, side}` 用于快速撤单/成交扣减

1. 维护价位聚合：

- `price -> level{agg_qty, order_count}`
- 新单：增加订单索引 + 价位聚合
- 撤单：查订单索引扣减 + 删除索引
- 成交：查订单索引扣减 qty；qty 归零则删除索引

1. 维护最优价位结构（低延迟）：

- 双端价格树/数组 + 最优价位缓存
- 事件更新后 O(1) 或 O(logN) 更新 best bid/ask

1. 对外输出：

- `OrderBookSnapshot` 快照更新（10 档）
- `valid_depth` 根据当前深度填充
- 上层仍可用 `TickRecord`（如果保留 5 档）

## 公共接口/配置变化

- 新增核心头文件 `order_book_snapshot.h`（或合并在 `order_book.h`）
- 新增配置块 `orderbook_snapshot`（同 `snapshot` 结构：`type/path/is_writer`）
- `TickRecord` 保持 5 档，OrderBook 的 6-10 档为 0，`valid_depth=5`

## 测试计划

1. 编译验证：`hft_core` 与引擎编译通过
2. 单进程读取：`OrderBookSnapshot::get` 的 5 档与 `TickRecord` 一致
3. SHM 验证：writer/reader 双进程读取稳定、无撕裂

## 假设与默认

- 固定 `ORDERBOOK_DEPTH = 10`
- 当前行情源仅提供 5 档深度
- 引擎统一更新快照作为唯一入口

