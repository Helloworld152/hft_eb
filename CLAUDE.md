# hft_eb

高频交易系统。C++17 插件架构，Python 策略层。支持实盘交易（CTP/Binance）和回测。

## 架构

- **实盘交易**：独立 `hft_trade_gateway` 进程，通过共享内存与引擎通信；支持 CTP 和 Binance
- **行情录制**：`hft_md/`（CTP 逐笔数据）、`hft_ba_md/`（Binance WebSocket）
- **回测**：`hft_backtest/` Python 包，复用实盘插件链路
- **事件流**：`ORDER_REQ → Risk → ORDER_SEND → SimTrade → RTN_ORDER/RTN_TRADE`；实盘路径 `GatewayPoll` 替代 `SimTrade`

## 关键目录

| 目录 | 用途 |
|---|---|
| `engine/` | 主进程，插件加载器+事件总线+主循环 |
| `core/` | 协议定义、订单管理、快照 |
| `infra/` | 通用组件（header-only），无锁队列、撮合引擎、内存池 |
| `modules/` | 13 个插件：trade/strategy/factor/risk/portfolio/monitor/replay 等 |
| `trade_gateway/` | 独立交易网关进程，CTP API 对接 |
| `hft_backtest/` | Python 回测框架 |
| `conf/` | YAML 配置（实盘和回测） |

## 技能

使用 `hft_eb_dev` skill 进行导航、开发和验证。

## 关键规则

- 最小修改，不改无关文件
- 大量代码修改后询问是否更新文档
- 不要跑 `./run.sh`
