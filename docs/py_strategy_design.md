# Python 策略插件设计（嵌入式 Python）

## 目标
- 支持用 Python 编写策略，同时复用现有 C++ 引擎事件链路。
- 回测行为尽量贴近引擎真实路径（Replay -> Strategy -> Risk -> Order -> SimTrade）。
- 通过嵌入式 Python 获取较低延迟。
- 支持 Replay 空闲超时自动停止回测。

## 非目标
- 首版不做多进程 IPC 策略（ZMQ/Socket）。
- 不替换现有风控/下单/成交模块。
- 不实现全功能撮合引擎。

## 总体架构
- 新插件：`mod_py_strategy`（C++）
- 插件内嵌 Python，加载用户策略类。
- 插件订阅事件（主要是 `EVENT_MARKET_DATA`）。
- Python 策略通过 `send_order(...)` 发单。
- 订单经 `EVENT_ORDER_REQ` 进入既有链路。
- Replay 增加空闲超时触发引擎停止。

```
Replay -> PyStrategy -> Risk -> Order -> Position/Account -> SimTrade
```

## 插件生命周期
- **init**
  - 读取 YAML 配置（模块/类/路径、过滤、采样）。
  - 初始化 Python 运行时。
  - 加载策略类并实例化。
  - 向 Python 注入 `send_order` 回调。
  - 订阅 `EVENT_MARKET_DATA`。
- **start**
  - 无特殊动作。
- **stop**
  - 释放 Python 引用。
  - 不主动 `Py_Finalize`（进程级别资源）。

## Python 策略接口

### 必需
- `__init__(self, config: dict, send_order: callable)` 或
- `init(self, config: dict, send_order: callable)`

### 事件回调
- `on_tick(self, tick: dict)`

可选：
- `on_kline(self, kline: dict)`
- `on_pos_update(self, pos: dict)`
- `on_order(self, order: dict)`
- `on_trade(self, trade: dict)`

### send_order 签名
```
send_order(
    symbol: str,
    direction: str,   # 'B' 或 'S'
    offset: str,      # 'O' / 'C' / 'T'
    price: float,
    volume: int,
    account_id: str = ""
)
```

## 配置（YAML）
示例：
```yaml
- name: PyStrategy
  library: "libmod_py_strategy.so"
  enabled: true
  config:
    py_module: "py_tools.strategies.sample_strategy"
    py_class: "SampleStrategy"
    py_path: "."
    symbol_filter: "au2606,rb2405"
    sample_every: 1
    error_policy: "disable"   # ignore | disable | stop
```

### 配置字段
- `py_module`: Python 模块导入路径
- `py_class`: 策略类名
- `py_path`: 追加到 `sys.path` 的路径（可选）
- `symbol_filter`: 逗号分隔的品种过滤（可选）
- `sample_every`: 每 N 条 tick 调用一次（可选）
- `error_policy`: Python 异常处理策略
  - `ignore`: 记录日志，继续
  - `disable`: 禁用策略回调
  - `stop`: 发布 `EVENT_ENGINE_STOP`

## 数据映射
- `TickRecord` -> Python dict 字段：
  - `symbol`, `symbol_id`, `last_price`, `volume`, `turnover`, `open_interest`
  - `bid_price1..5`, `bid_volume1..5`
  - `ask_price1..5`, `ask_volume1..5`
  - `trading_day`, `update_time`

## 线程与性能
- 每次回调使用 `PyGILState_Ensure`。
- `symbol_filter` 与 `sample_every` 可降低 Python 调用频率。
- Python 回调是同步的，处于引擎事件路径内。

## 回测结束（Replay 空闲超时）
- Replay 新增 `idle_stop_sec` 配置。
- 超过该时间无新数据则发布 `EVENT_ENGINE_STOP`。
- 引擎订阅 `EVENT_ENGINE_STOP` 后安全退出。

## 构建与依赖
- 依赖 Python3 Development 头文件/库。
- CMake 使用 `find_package(Python3 COMPONENTS Development REQUIRED)`。

## 示例运行
```bash
./bin/hft_engine conf/config_py_backtest.yaml
```

## 涉及文件
- `modules/py_strategy/py_strategy_module.cpp`（新增）
- `include/framework.h`（新增 `EVENT_ENGINE_STOP`）
- `src/engine.cpp` + `include/engine.h`（订阅 stop 事件）
- `modules/replay/replay_module.cpp`（空闲停止）
- `CMakeLists.txt`（Python 依赖 + 插件目标）
- `conf/config_py_backtest.yaml`（示例配置）
- `py_tools/strategies/sample_strategy.py`（示例策略）
- `hft_backtest/scripts/run_config.py`（可选，安装后可通过 `hft-backtest-run-config` 调用）
- `hft_backtest/tests/test_sdk.py`（包内最小自测）
