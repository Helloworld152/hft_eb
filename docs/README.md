# 文档导航

[`docs/README.md`](README.md) 是 `docs/` 目录的总入口，目标是帮助新人和维护者快速找到“先看什么、问题归哪类、改动该参考哪些文档”。

## 推荐阅读顺序

1. 顶层系统说明：[README.md](../README.md)
2. 模块职责总览：[docs/modules_overview.md](modules_overview.md)
3. 并发与进程边界：[docs/并发架构设计_concurrency_design.md](并发架构设计_concurrency_design.md)
4. 交易 gateway 进程化方案：[docs/交易网关进程化设计_gateway_process.md](交易网关进程化设计_gateway_process.md)
5. 订单与持仓核心状态：
   - [docs/订单管理设计_order_manager.md](订单管理设计_order_manager.md)
   - [docs/持仓管理设计_position.md](持仓管理设计_position.md)

## 按主题导航

### 1. 架构与并发

- [docs/并发架构设计_concurrency_design.md](并发架构设计_concurrency_design.md)：共享内存、进程拓扑、去中心化总线思路。
- [docs/共享内存快照设计_shm_snapshot.md](共享内存快照设计_shm_snapshot.md)：最新行情快照的 SHM 与 Seqlock 设计。
- [docs/中央脉搏调度设计_centralized_timer.md](中央脉搏调度设计_centralized_timer.md)：统一定时调度与系统节拍。
- [docs/交易网关进程化设计_gateway_process.md](交易网关进程化设计_gateway_process.md)：交易柜台从进程内插件改造为独立 gateway 进程的设计方案。

### 2. 交易链路与 OMS

- [docs/订单管理设计_order_manager.md](订单管理设计_order_manager.md)：订单 ID、映射关系、订单生命周期。
- [docs/持仓管理设计_position.md](持仓管理设计_position.md)：持仓状态、Core 化与无锁读写。
- [docs/风控模块设计_risk.md](风控模块设计_risk.md)：交易前风控职责与链路位置。
- [docs/portfolio_module.md](portfolio_module.md)：信号聚合、仓位裁剪与下单请求生成。
- [docs/plugins/portfolio.md](plugins/portfolio.md)：`Portfolio` 参数速查。
- [docs/扫单交易插件设计_sweep_trader.md](扫单交易插件设计_sweep_trader.md)：执行型交易模块设计。

### 3. 行情与快照

- [docs/行情流设计_data_stream.md](行情流设计_data_stream.md)：行情数据流与消费模型。
- [docs/K线插件设计_kline.md](K线插件设计_kline.md)：K 线生成、聚合与回放。
- [docs/共享内存快照设计_shm_snapshot.md](共享内存快照设计_shm_snapshot.md)：预风控与监控读路径。

### 4. 策略、因子、DAG

- [docs/因子插件设计_factor.md](因子插件设计_factor.md)：因子插件基础抽象。
- [docs/DAG模块设计_factor_dag.md](DAG模块设计_factor_dag.md)：因子 DAG 组织方式。
- [docs/策略树设计_strategy_tree.md](策略树设计_strategy_tree.md)：策略树主设计。
- [docs/并行策略树插件设计_parallel_strategy_tree.md](并行策略树插件设计_parallel_strategy_tree.md)：并行策略树设计。
- [docs/py_strategy_design.md](py_strategy_design.md)：Python 策略接入说明。
- [docs/plugins/py_strategy.md](plugins/py_strategy.md)：`PyStrategy` 配置参数速查。
- [docs/Python回测设计_py_backtest.md](Python回测设计_py_backtest.md)：Python 回测设计。

### 5. 监控与协议

- [docs/监控模块设计_monitor.md](监控模块设计_monitor.md)：监控模块职责与输出。
- [docs/websocket_protocol.md](websocket_protocol.md)：WebSocket 协议说明。
- [docs/plugins/signal_csv.md](plugins/signal_csv.md)：信号 CSV 输出插件说明。
- [docs/plugins/replay.md](plugins/replay.md)：`Replay` 回放插件参数说明。
- [docs/plugins/gateway_poll.md](plugins/gateway_poll.md)：独立 gateway 桥接插件参数说明。

### 6. 性能优化与实验

- [docs/性能优化_StaticDelegate.md](性能优化_StaticDelegate.md)：委托与调用链优化。
- [docs/编译优化指南_compile_optimization.md](编译优化指南_compile_optimization.md)：构建与编译优化建议。
- [docs/latency_test_20260327.md](latency_test_20260327.md)：阶段性延迟测试记录。
- [docs/orderbook_refactor.md](orderbook_refactor.md)：OrderBook 相关重构记录。

### 7. 路线图与历史记录

- [docs/开发路线图_roadmap.md](开发路线图_roadmap.md)：系统阶段目标与优先级。
- [docs/热重载设计_hot_reload.md](热重载设计_hot_reload.md)：热重载方向记录。
- [docs/portfolio_module.md](portfolio_module.md)：组合层设计记录。
- [docs/plugins/README.md](plugins/README.md)：插件文档子目录索引。

## 按角色导航

### 新人入门

1. [README.md](../README.md)
2. [docs/README.md](README.md)
3. [docs/modules_overview.md](modules_overview.md)
4. [docs/开发路线图_roadmap.md](开发路线图_roadmap.md)
5. [docs/plugins/README.md](plugins/README.md)

### 做交易链路改造

1. [docs/交易网关进程化设计_gateway_process.md](交易网关进程化设计_gateway_process.md)
2. [docs/订单管理设计_order_manager.md](订单管理设计_order_manager.md)
3. [docs/持仓管理设计_position.md](持仓管理设计_position.md)
4. [docs/并发架构设计_concurrency_design.md](并发架构设计_concurrency_design.md)
5. [docs/plugins/gateway_poll.md](plugins/gateway_poll.md)
6. [docs/plugins/portfolio.md](plugins/portfolio.md)

### 做策略 / 因子开发

1. [docs/因子插件设计_factor.md](因子插件设计_factor.md)
2. [docs/DAG模块设计_factor_dag.md](DAG模块设计_factor_dag.md)
3. [docs/策略树设计_strategy_tree.md](策略树设计_strategy_tree.md)
4. [docs/并行策略树插件设计_parallel_strategy_tree.md](并行策略树插件设计_parallel_strategy_tree.md)
5. [docs/plugins/py_strategy.md](plugins/py_strategy.md)

### 做监控 / UI / 对外接口

1. [docs/监控模块设计_monitor.md](监控模块设计_monitor.md)
2. [docs/websocket_protocol.md](websocket_protocol.md)
3. [docs/共享内存快照设计_shm_snapshot.md](共享内存快照设计_shm_snapshot.md)

### 做性能优化

1. [docs/性能优化_StaticDelegate.md](性能优化_StaticDelegate.md)
2. [docs/编译优化指南_compile_optimization.md](编译优化指南_compile_optimization.md)
3. [docs/latency_test_20260327.md](latency_test_20260327.md)

## 文档状态说明

- 主设计文档：用于指导实现与重构，优先参考。
- 实现说明：围绕具体模块或协议，和代码一起看。
- 实验 / 记录：压测、重构草案、阶段性结论，不一定代表当前最终实现。

当前建议优先视为“实验 / 记录”的文档：

- [docs/latency_test_20260327.md](latency_test_20260327.md)
- [docs/orderbook_refactor.md](orderbook_refactor.md)
- [docs/热重载设计_hot_reload.md](热重载设计_hot_reload.md)

如果后续需要继续整理文档，优先保持“新增索引和交叉引用”，尽量避免大规模重命名，减少对已有链接和团队习惯的影响。

## 当前补齐说明

- `docs/` 下的设计文档用于解释模块职责、链路和架构边界。
- `docs/plugins/` 下的文档用于快速查询插件参数和最小配置语义。
- 本轮优先补齐代码中已存在、但参数说明缺失的常用模块，未对历史设计文档做大规模重写。
