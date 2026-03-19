# 并行策略树插件设计 (Parallel StrategyTree Module)

## 1. 目标
在不改变现有因子/策略节点接口的前提下，为策略树引入多线程并行计算能力，实现：
- **按品种分片并行**，同一品种强顺序。
- **复用现有 StrategyNode SO**，无需改造已有因子节点。
- **线程安全信号分发**，与现有 EventBus 兼容。

## 2. 设计概述
该插件作为一个新的 IModule 动态库存在，替换原有 `StrategyTree` 使用：
- 主线程订阅 `EVENT_MARKET_DATA`，将 Tick 路由到固定分片队列。
- 每个分片线程**串行**执行本分片内所有并行节点的 `onTick`，保证同品种顺序一致。
- 节点发出的 `send_signal` 进入无锁队列，主线程集中 drain，并进行：
  1) 分发给兄弟节点 `onSignal`  
  2) 可选发布到 `EVENT_SIGNAL`

## 3. 并行策略
### 3.1 按品种分片
同一 symbol 永远落在同一 shard，避免状态竞争：
- 优先使用 `symbol_id` 分片  
- 若 `symbol_id` 不可用，则对 `symbol` 做 hash 分片

### 3.2 节点并行控制
节点支持配置是否参与并行：
- `parallel: true/false`
- 或 `role: aggregator|strategy|factor`（`aggregator` / `strategy` 默认串行）

## 4. 配置示例
```yaml
plugins:
  - name: StrategyTreeParallel
    library: "../bin/libmod_strategy_tree_parallel.so"
    enabled: true
    config:
      parallel: true
      shard_count: 8
      queue_capacity: 8192
      shard_by: "symbol_id"   # 或 "symbol"
      publish_signals: true
      nodes:
        - id: FACTOR_SMA
          library: "../bin/libstrat_sma.so"
          parallel: true
          params:
            window_size: 20

        - id: CS_COMBINER
          library: "../bin/libstrat_cs_combiner.so"
          role: aggregator
          params:
            weights:
              SMA_Diff: 0.5
```

## 5. 兼容性与行为
- 对外接口与原 StrategyTree 一致（`IStrategyNode` 不变）。
- `send_signal` 从“同步直发”变为“异步入队 -> 主线程分发”。
- 若 `parallel: false`，则退化为单线程处理（等价于旧版行为）。

## 6. 注意事项
- 并行节点会按分片**各自实例化**，确保线程安全；避免共享可变状态。
- 跨品种/截面类因子建议配置 `role: aggregator` 或 `parallel: false`。
- 若 tick 很稀疏，可考虑后续添加定时 drain（保证信号及时发布）。

