# Modules 插件功能概览

本文档简要介绍 `modules/` 目录下各插件的功能定位，便于快速理解系统模块分工与数据流向。

## 模块列表与职责

- `ctp`：CTP 模拟网关。订阅 `EVENT_ORDER_REQ` 打印/模拟报单，同时定时发布模拟行情到 `EVENT_MARKET_DATA`。
- `ctp_real`：CTP 实盘交易网关（Trader API）。负责鉴权、登录、断线重连、报单/撤单、回报处理，并发布连接状态。
- `factor`：因子计算模块（DAG）。解析 YAML 配置，动态加载因子节点并在 Tick/K线/定时器触发下计算输出信号。
- `kline`：K 线聚合与落盘。基于 Tick 生成 1m K 线，并级联生成 1h/1d，写入 mmap 文件。
- `kline`（`kline_parquet_replay_module.cpp`）：Parquet 日线回放，按批读取数据并发布 `EVENT_KLINE`。
- `monitor`：监控与转发。通过 ZMQ / WebSocket 对外发布行情、回报、持仓、资金等事件。
- `monitor`（`signal_csv_module.cpp`）：信号落盘为 CSV，支持环形队列缓冲与批量刷盘。
- `order`：订单管理与去重中心。生成本地订单 ID、拦截报单/撤单、处理原始回报并转发规范化事件。
- `position`：持仓与资金维护。定时查询持仓/资金，处理持仓回报并周期性落盘为 JSON。
- `replay`：行情回放。读取 mmap Tick 数据流并发布 `EVENT_MARKET_DATA`，用于回测/仿真。
- `risk`：风控模块。当前实现为下单频率限制（Max Orders/Sec），超限直接拒单。
- `strategy`：策略树插件容器。解析 YAML 动态加载策略节点并透传事件；支持并行策略树版本。
- `sweep_trader`：扫单/TWAP 交易插件。监控目录中的订单 CSV，支持时间分片执行与自动定价。
- `test`：测试辅助插件。按配置订阅事件并以 JSON 打印，用于单元测试与排查。
- `test_harness`：测试编排插件。基于 JSON 描述预期事件并验证结果，可与外部测试框架联动。
- `trade`：交易网关集合。
- `trade`（`sim_trade_module.cpp`）：模拟成交引擎（回测），处理 `EVENT_ORDER_SEND`/`EVENT_CANCEL_SEND` 并回报成交/订单。
- `trade`（`simple_trade.cpp`）：简单示例网关，接收下单请求并打印。
- `trade`（`ccapi_binance_usds_trade.cpp`）：Binance USDS 期货交易网关（CCAPI），订阅私有通道并支持下单/撤单、查询资金与持仓。

## 代码位置

- 模块入口文件位于 `modules/<name>/*_module.cpp` 或同目录的功能实现文件（如 `simple_trade.cpp`）。
- 相关设计文档在 `docs/` 下，例如：`docs/因子插件设计_factor.md`、`docs/并行策略树插件设计_parallel_strategy_tree.md`、`docs/扫单交易插件设计_sweep_trader.md`。

