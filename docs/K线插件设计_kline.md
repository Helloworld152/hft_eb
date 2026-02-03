# K 线合成插件设计 (K-Line Synthesis Plugin)

## 1. 设计目标
实现高性能、多周期（分钟、小时、日）的 K 线合成，为策略和因子提供标准化的时序数据。

## 2. 核心架构

### 2.1 级联合成 (Hierarchical Synthesis)
为了减少重复计算并保证数据一致性，采用级联式合成架构：
1.  **Tick-to-1M**: 订阅 `EVENT_MARKET_DATA`，将原始 Tick 聚合为 1 分钟线。
2.  **1M-to-1H**: 订阅 1 分钟线的闭合事件，聚合为 1 小时线。
3.  **1H-to-1D**: 订阅 1 小时线的闭合事件，聚合为日线。

### 2.2 事件流向
-   **Input**: `EVENT_MARKET_DATA` (TickRecord*)
-   **Output**: `EVENT_KLINE` (KlineRecord*)

## 3. 数据结构定义 (`protocol.h`)

```cpp
enum KlineInterval {
    K_1M = 1,
    K_5M = 5,
    K_15M = 15,
    K_1H = 60,
    K_1D = 1440
};

struct KlineRecord {
    char symbol[32];
    uint32_t trading_day; // 交易日
    uint64_t start_time;  // 周期起始时间 (HHMMSSmmm)
    double open;
    double high;
    double low;
    double close;
    int volume;           // 周期内成交量增量
    double turnover;      // 周期内成交额增量
    double open_interest; // 周期末持仓量
    KlineInterval interval;
};
```

## 4. 关键逻辑实现

### 4.1 时间对齐 (Time Alignment)
-   使用 `TickRecord::update_time` (交易所时间) 进行对齐。
-   **分钟线对齐**: `bar_start_time = (current_ms / 60000) * 60000`。
-   **跨日处理**: 日线的切割点依据 `trading_day` 的切换或特定交易时间段（如 21:00 夜盘开始）。

### 4.2 闭合触发机制 (Closure Mechanism)
采用 **Next-Tick Trigger (下包触发)** 模式：
1.  当收到一个新的 Tick/Bar，其时间戳跨越了当前周期的边界时，判定当前周期 Bar 已经“闭合”。
2.  **动作**:
    -   标记 `is_closed = true`。
    -   发布 `EVENT_KLINE` 事件。
    -   初始化下一个周期的 Bar。
3.  **注意**: 在系统停止（`stop()`）时，必须强制推送到目前为止尚未闭合的所有 Bar。

### 4.3 状态管理
插件内部为每个合约维护一个状态机 `SymbolContext`：
```cpp
struct SymbolContext {
    KlineRecord current_1m;
    KlineRecord current_1h;
    KlineRecord current_1d;
    
    // 增量计算辅助
    int last_tick_volume;
    double last_tick_turnover;
};
```

## 5. 性能优化
1.  **Zero Copy**: `EventBus` 分发 `KlineRecord` 的指针，不进行内存拷贝。
2.  **预分配**: 插件启动时根据配置预分配所有合约的 `SymbolContext` 内存，避免运行期 `std::unordered_map` 的频繁扩容。
3.  **无锁逻辑**: 插件运行在引擎主线程中，通过同步回调处理，无需互斥锁。

## 7. 持久化设计 (Persistence)

为了支持回测和盘后分析，K 线数据必须持久化。

### 7.1 Mmap 存储
采用与 Tick 录制相同的 `MmapWriter` 机制：
-   **文件格式**: `.dat` (原始数据) + `.meta` (原子游标)。
-   **存储路径**: `data/kline_1M_YYYYMMDD.dat`, `data/kline_1H_YYYYMMDD.dat` 等。
-   **写入时机**: 仅在 Bar 闭合（`is_closed == true`）时执行写入。

### 7.2 写入流程
1.  Bar 闭合触发 `EVENT_KLINE`。
2.  `KlineModule` 内部对应的 `MmapWriter<KlineRecord>` 调用 `write()`。
3.  由于 K 线频率较低（最高 1 分钟一次），同步写入对主线程性能影响极小。

### 7.3 配置项扩展
```json
{
    "persistence": {
        "enabled": true,
        "path": "../data/",
        "auto_create": true
    }
}
```
