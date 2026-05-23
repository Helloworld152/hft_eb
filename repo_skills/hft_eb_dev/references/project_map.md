# 项目地图

## 目录

| 目录 | 内容 | 何时读 |
|------|------|--------|
| `core/include/` | protocol.h, core_state.h, order_manager.h | 改数据协议、事件结构 |
| `core/src/` | PositionService/OrderService 实现 | 持仓/订单状态追踪 |
| `infra/include/` | intrusive_pool.h, static_delegate.h, tick_matching_engine.h, simple_matching_engine.h | 通用组件、撮合引擎 |
| `engine/` | engine.h, framework.h (IModule, EventBus, StaticDelegate) | 改框架接口 |
| `modules/py_strategy/` | Python 桥接 | 改策略接口、下单/撤单 |
| `modules/trade/` | sim_trade, lob_sim_trade | 改撮合、模拟账户 |
| `modules/risk/` | risk_module.cpp | 改风控 |
| `modules/replay/` | 行情回放 | 改数据源 |
| `modules/monitor/` | backtest_recorder (CSV输出) | 改输出格式 |
| `hft_backtest/` | template.py, objects.py, engine.py | 改 Python 策略基类、数据结构、回测入口 |
| `conf/` | YAML 配置 | 改运行参数 |

## 文档速查

| 想看什么 | 文件 |
|----------|------|
| 策略怎么用 | `hft_backtest/策略接口文档.md` |
| 回测输出怎么读 | `docs/回测使用指南.md` |
| 回测架构和陷阱 | `references/hft_backtest.md` |
| 设计决策 | `docs/` 下对应模块文档 |

## 关键入口

- 事件定义：`engine/include/framework.h` (EventType enum, IModule, ConfigMap)
- 数据协议：`core/include/protocol.h` (OrderReq, TradeRtn, TickRecord)
- 核心状态：`core/include/core_state.h` (PositionStore, OrderStore, AccountStore)
- 订单 ID：`core/include/order_manager.h` (OrderIDGenerator)
- 撮合引擎：`infra/include/tick_matching_engine.h`、`infra/include/simple_matching_engine.h`
