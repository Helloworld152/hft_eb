# hft_backtest

复用 hft_eb C++ 引擎链路的 Python 回测框架，策略接口对齐 vn.py CTA 风格。

## 架构

```
用户策略 (CtaTemplate)
        │
   BacktestEngine (自动生成 YAML、驱动引擎、收集结果)
        │
   _core.so (pybind11)
        │
   HftEngine → Replay → PyStrategy → Risk → SimTrade → Recorder
```

底层插件链路与实盘一致，回测结果可信。

## 文档导航

| 文档 | 说明 |
|------|------|
| [策略接口文档](策略接口文档.md) | 策略可用的全部回调、数据字段、下单方法 |
| [回测使用指南](../docs/回测使用指南.md) | 从使用者角度展示终端输出、CSV 解读、状态码速查 |

## 安装

```bash
python3 setup.py bdist_wheel
pip install hft_backtest/dist/hft_backtest-*.whl
```

安装后会注册两个命令行入口：

```bash
hft-backtest-demo                   # 跑示例策略
hft-backtest-run-config config.yaml  # 手写 YAML 配置模式
```

如果不想安装，仓库内直接跑：

```bash
python3 -m hft_backtest.scripts.run_demo
python3 -m hft_backtest.scripts.run_config conf/config_py_backtest.yaml
```

## 最小示例

```python
from hft_backtest import BacktestEngine, CtaTemplate

class MyStrategy(CtaTemplate):
    def on_tick(self, tick):
        if tick.symbol == self.vt_symbol and tick.last_price > 100:
            self.buy(price=tick.last_price, volume=1)

    def on_trade(self, trade):
        self.write_log(f"成交 {trade.price} {trade.volume}")

engine = BacktestEngine()
engine.set_parameters(
    data_mode="tick",
    data_file="data/market_data_20260319_night",
    symbols_file="conf/symbols.txt",
    initial_balance=1_000_000,
)
engine.add_strategy(MyStrategy, {"symbol": "au2606"})
result = engine.run_backtesting()
print(result.statistics)
# {'total_return': 0.023, 'max_drawdown': 0.008, 'sharpe': 1.42, ...}
```

## API

### BacktestEngine

| 方法 | 说明 |
|---|---|
| `set_parameters(...)` | 设置回测参数（数据模式、文件、资金、滑点等） |
| `add_strategy(cls, setting)` | 注册策略类和配置 |
| `run_backtesting()` | 执行回测，返回 `BacktestResult` |
| `load_config(path)` | 直接加载 YAML 配置（高级模式） |

`set_parameters` 支持的关键参数：

| 参数 | 默认值 | 说明 |
|---|---|---|
| `data_mode` | - | `"tick"` 或 `"bar"` |
| `data_file` | - | 行情数据文件路径 |
| `symbols_file` | - | 品种列表文件 |
| `initial_balance` | 1,000,000 | 初始资金 |
| `slippage_ticks` | 0.0 | 滑点（跳数） |
| `tick_size` | 1.0 | 最小变动价位 |
| `output_dir` | tmp 下自动生成 | 结果输出目录 |
| `replay_config` | {} | 回放插件参数（`idle_stop_sec` 等） |
| `extra_py_config` | {} | 透传给策略的额外配置 |

### CtaTemplate

策略生命周期回调：

```
on_init → on_start → on_tick / on_bar → on_order / on_trade → on_stop
```

下单方法：

```python
self.buy(price, volume, symbol=None)     # 买入开仓
self.sell(price, volume, symbol=None)    # 卖出平仓
self.short(price, volume, symbol=None)   # 卖出开仓
self.cover(price, volume, symbol=None)   # 买入平仓
```

### 数据对象

回调中收到的均为类型化 dataclass，非裸 dict：

- `TickData` — symbol, last_price, volume, turnover, open_interest, trading_day, update_time
- `BarData` — symbol, open/high/low/close, volume, turnover, interval
- `OrderData` — symbol, direction, offset, price, volume_total, volume_traded, insert_time, update_time, status
- `TradeData` — symbol, direction, offset, price, volume, trade_time, trade_id, liquidity_role(`M`/`T`)
- `AccountData` — account_id, balance, available, margin, close_pnl, position_pnl

### BacktestResult

`run_backtesting()` 返回对象包含：

| 字段 | 类型 | 说明 |
|---|---|---|
| `output_dir` | str | 结果目录 |
| `config_path` | str | 生成的 YAML 路径 |
| `orders` | list[dict] | 订单记录 |
| `trades` | list[dict] | 成交记录 |
| `accounts` | list[dict] | 账户快照序列 |
| `statistics` | dict | 绩效统计（total_return, max_drawdown, sharpe, trade_count, turnover） |

## 输出文件

回测结束后 `output_dir` 下生成：

- `backtest.yaml` — 自动生成的引擎配置
- `orders.csv` — 订单流水
- `trades.csv` — 成交流水
- `accounts.csv` — 账户权益序列

## 数据模式

| 模式 | 回放插件 | 专属配置 |
|---|---|---|
| `tick` | Replay | `idle_stop_sec`（无新数据 N 秒后自动终止） |
| `bar` | Kline_Parquet_Replay | `sleep_ms_per_bar`、`start_time_hhmmssmmm`、`stop_on_finish` |

## 编辑限制

- 第一版聚焦单策略 CTA 回测
- Python ≥ 3.10
- 需要预编译 `_core` 及相关插件 `.so`
