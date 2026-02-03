# Python HFT Backtest Framework Design

## 1. 核心目标
构建一个基于 `HftEngine` C++ 核心的 Python 回测框架，兼顾 C++ 的高性能事件分发与 Python 的灵活性。支持在 Jupyter Notebook 中进行策略研究、回放历史 Mmap 数据，并验证风控与交易逻辑。

## 2. 架构概览

采用 **Python C++ Extension (pybind11)** 模式。Python 作为宿主环境（Host），加载 C++ 编译的扩展模块 `hft_backtest`。

```mermaid
graph TD
    subgraph Python Space
        UserStrat[UserStrategy<br/>(inherits hft.Strategy)]
        PyEngine[hft.Engine]
        Jupyter[Jupyter/Script]
    end

    subgraph C++ Space (hft_backtest.so)
        Trampoline[PyStrategyTrampoline<br/>(inherits IModule)]
        CppEngine[HftEngine]
        EventBus[EventBus]
        Replay[ReplayModule]
    end

    Jupyter -->|1. Init| PyEngine
    PyEngine -->|2. Create| CppEngine
    UserStrat -.->|3. Register| Trampoline
    Trampoline -->|4. Subscribe| EventBus
    
    Replay -->|5. Tick| EventBus
    EventBus -->|6. Dispatch| Trampoline
    Trampoline -->|7. Callback (GIL)| UserStrat
    
    UserStrat -->|8. Order| Trampoline
    Trampoline -->|9. OrderReq| EventBus
```

## 3. 关键组件设计

### 3.1 Python 扩展模块 (`hft_backtest`)
通过 `pybind11` 将核心 C++ 类导出为 Python 类。

| C++ 类 | Python 类 | 职责 |
| :--- | :--- | :--- |
| `HftEngine` | `hft_backtest.Engine` | 管理插件加载、配置解析、主循环控制。 |
| `IModule` | `hft_backtest.Strategy` | 策略基类（Trampoline），用户继承此类实现策略逻辑。 |
| `TickRecord` | `hft_backtest.Tick` | 只读属性访问，提供高精度行情数据。 |
| `OrderReq` | `hft_backtest.Order` | 报单请求结构。 |

### 3.2 数据流向 (Data Flow)

#### 行情推送 (C++ -> Python)
1. `ReplayModule` (C++) 读取 Mmap，发布 `EVENT_MARKET_DATA`。
2. `EventBus` 分发给所有订阅者，包括 `PyStrategyTrampoline`。
3. `Trampoline` 获取 GIL (Global Interpreter Lock)。
4. 将 `TickRecord` 转换为 Python `Tick` 对象（或直接传递指针封装）。
5. 调用 Python 侧 `on_tick(tick)`。

#### 交易指令 (Python -> C++)
1. 用户在 `on_tick` 中调用 `self.send_order(...)`。
2. 调用进入 C++ `Trampoline::send_order`。
3. 封装 `OrderReq` 结构体。
4. `EventBus->publish(EVENT_ORDER_REQ, &req)`。
5. 后续流程由 C++ 的 `RiskModule` 和 `TradeModule` 处理。

### 3.3 内存管理与性能
*   **Zero Copy (Partial)**: `TickRecord` 在 Python 侧应尽量以 `py::class_` 的引用方式暴露，避免每次 Tick 都进行深拷贝。
*   **GIL 管理**: 
    *   C++ 回调 Python 时必须 `py::gil_scoped_acquire`。
    *   `Engine.run()` 为阻塞调用，需确保在耗时操作（如 `_mm_pause` 等待）时考虑释放 GIL（但在单线程回测模型中，通常一直持有 GIL 运行即可，或者在 Sleep 时释放）。
*   **生命周期**: Python 侧的 Strategy 对象在 C++ 侧通过 `std::shared_ptr` 持有，防止过早被 GC 回收。

## 4. 接口定义 (Python Pseudocode)

```python
import hft_backtest as hft

class GridStrategy(hft.Strategy):
    def init(self):
        print("Strategy Initialized")
        self.pos = 0

    def on_tick(self, tick):
        # tick is a C++ binding object
        if tick.last_price > 100.0 and self.pos == 0:
            print(f"Buy at {tick.last_price}")
            # send_order(symbol, direction, price, volume)
            self.send_order(tick.symbol, hft.Direction.Buy, tick.last_price, 1)

# Main Setup
engine = hft.Engine()
engine.load_config("conf/config_replay.json")

# 添加自定义策略
strat = GridStrategy()
engine.add_strategy(strat) # C++ 侧注册模块

engine.run() # 开始回放
```

## 5. 开发计划

1.  **CMake配置**: 集成 `pybind11` (Done)。
2.  **核心结构导出**: 绑定 `TickRecord`, `OrderReq` 等 POD 结构。
3.  **策略基类绑定**: 实现 `PyStrategyTrampoline`，支持 Python 继承并重写虚函数。
4.  **引擎绑定增强**: 增加 `add_strategy(Strategy*)` 接口，支持动态注入 Python 定义的策略模块。
5.  **验证**: 使用 Jupyter Notebook 跑通回测 Demo。
