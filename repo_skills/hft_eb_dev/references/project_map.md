# 项目地图

这个文件用于快速建立 `hft_eb` 的目录心智模型。只有在需要导航仓库、判断改动落点、补充设计依据时再读。

## 目录分层

- `src/`：引擎主入口与生命周期控制
- `include/`：框架接口、事件协议、插件抽象、对外头文件
- `core/`：底层公共能力，偏 IPC、共享状态、底层结构
- `modules/`：插件实现，按功能拆分为行情、交易、风控、策略、监控等
- `conf/`：运行配置入口，决定加载哪些插件及其参数
- `tests/`：当前以轻量性能/结构验证为主，不是完整单测体系
- `docs/`：模块设计、链路设计、插件设计、架构和路线图
- `py_tools/`：Python 侧工具与策略辅助
- `rust_tools/`：Rust 小工具
- `hft_md/`、`hft_ba_md/`：旁路或子系统目录，不要和主引擎混淆

## 推荐入口文件

- 系统概览：`README.md`
- 引擎启动与配置注入：`src/engine.cpp`、`include/engine.h`
- 事件协议与框架抽象：`include/framework.h`、`core/include/protocol.h`
- 插件职责总览：`docs/modules_overview.md`
- 总体架构与 DAG/执行器方向：`架构.md`

## modules 目录速览

- `replay`：行情回放
- `risk`：交易前风控
- `order`：订单管理与规范化
- `trade` / `ctp_real`：交易网关与实盘接入
- `position` / `portfolio` / `account`：持仓、组合、资金状态
- `strategy` / `factor` / `py_strategy`：策略树、因子 DAG、Python 策略
- `monitor`：对外监控、转发、信号落盘
- `kline`：K 线聚合与回放
- `test` / `test_harness`：测试辅助插件
- `sweep_trader`：扫单/TWAP 类执行模块

## 文档索引

- 模块总览：`docs/modules_overview.md`
- 路线图：`docs/开发路线图_roadmap.md`
- 并发架构：`docs/并发架构设计_concurrency_design.md`
- 订单管理：`docs/订单管理设计_order_manager.md`
- 持仓管理：`docs/持仓管理设计_position.md`
- 风控：`docs/风控模块设计_risk.md`
- 行情流：`docs/行情流设计_data_stream.md`
- 共享内存快照：`docs/共享内存快照设计_shm_snapshot.md`
- 因子 DAG：`docs/DAG模块设计_factor_dag.md`
- 策略树：`docs/策略树设计_strategy_tree.md`
- 并行策略树：`docs/并行策略树插件设计_parallel_strategy_tree.md`
- 扫单插件：`docs/扫单交易插件设计_sweep_trader.md`
- Python 回测与策略：`docs/Python回测设计_py_backtest.md`、`docs/py_strategy_design.md`

## 导航建议

- 想找“某个功能在哪里实现”，先在 `modules/` 和 `docs/modules_overview.md` 里定位。
- 想找“某个配置是怎么注入的”，从 `conf/*.yaml` 回溯到 `src/engine.cpp`。
- 想找“某个事件从哪里发出、被谁消费”，先搜事件常量，再看相邻模块订阅关系。
- 想判断改动该落在引擎还是插件，先确认它是通用能力还是某个业务链路专属逻辑。
