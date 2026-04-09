# Modules 插件功能概览

本文档简要介绍 `modules/` 目录下各插件的功能定位，便于快速理解系统模块分工与数据流向。

## 相关文档

- 文档总导航：[docs/README.md](README.md)
- 并发与共享内存拓扑：[docs/并发架构设计_concurrency_design.md](并发架构设计_concurrency_design.md)
- 交易 gateway 进程化方向：[docs/交易网关进程化设计_gateway_process.md](交易网关进程化设计_gateway_process.md)

## 模块列表与职责

- `ctp`：演示/联调用 CTP 模块，可发布模拟行情并消费下单事件。
- `factor`：因子计算模块（DAG）。解析 YAML，动态加载因子节点，并在 Tick/K 线/定时器触发下输出 `EVENT_SIGNAL`。
- `kline`：K 线模块集合。`kline_module.cpp` 负责聚合 Tick 生成 K 线，`kline_parquet_replay_module.cpp` 负责 Parquet K 线回放。
- `monitor`：监控与转发模块。通过 WebSocket / ZMQ 对外发布行情、回报、持仓、资金等事件。
- `monitor/signal_csv_module.cpp`：信号落盘插件。异步把 `EVENT_SIGNAL` 写入 CSV，便于研究与审计。
- `portfolio`：组合层模块。聚合多来源信号，结合账户/持仓/名义金额约束，输出 `EVENT_ORDER_REQ`。
- `py_strategy`：嵌入式 Python 策略插件。加载 Python 类并将 Tick/K 线映射为字典回调，策略通过 `send_order(...)` 发单。
- `replay`：Tick 回放模块。读取 mmap 数据流，更新 `MarketSnapshot` 并发布 `EVENT_MARKET_DATA`。
- `risk`：交易前风控模块。当前核心能力是报单频率限制（`max_orders_per_second`）。
- `strategy`：策略插件与策略树实现集合。既包含简单示例策略，也包含 `StrategyTree` / 并行策略树和多个叶子节点实现。
- `sweep_trader`：扫单/TWAP 执行模块。读取目录中的订单文件并按配置拆分执行。
- `test`：事件采样/调试插件。按配置订阅事件并输出，便于排查链路。
- `test_harness`：测试编排插件。根据 JSON 规则校验预期事件，适合回归验证。
- `trade/simple_trade.cpp`：最小下单示例模块，主要用于联调和日志验证。
- `trade/sim_trade_module.cpp`：模拟成交模块。处理下单/撤单事件并生成订单、成交回报。
- `trade/gateway_poll_module.cpp`：独立 `trade_gateway` 进程桥接模块。通过共享内存 ring 发送命令、轮询回报，并把结果回灌 EventBus 与 Core。
- `trade/ccapi_binance_usds_trade.cpp`：Binance USDS 期货交易模块（CCAPI）。

## 说明

- `modules/` 当前没有单独的 `order` / `position` 插件目录；这两类状态更多收口在 `core/` 与独立设计文档里。
- 交易 gateway 进程本体位于 `trade_gateway/`，`GatewayPoll` 只是主引擎一侧的桥接插件。

## 代码位置

- 模块入口文件位于 `modules/<name>/*_module.cpp` 或同目录的功能实现文件（如 `simple_trade.cpp`）。
- 相关设计文档在 `docs/` 下，例如：[docs/因子插件设计_factor.md](因子插件设计_factor.md)、[docs/并行策略树插件设计_parallel_strategy_tree.md](并行策略树插件设计_parallel_strategy_tree.md)、[docs/扫单交易插件设计_sweep_trader.md](扫单交易插件设计_sweep_trader.md)。
