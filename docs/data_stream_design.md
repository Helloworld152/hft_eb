# 极速行情流设计 (High-Speed Data Stream)

## 1. 设计目标
在高频交易中，Tick 数据的存储和读取速度直接影响回测精度和实时因子的计算。
- **Zero-Copy**: 使用 `mmap` 技术实现进程间共享内存，避免内核态文件 I/O 拷贝。
- **Lock-Free**: 基于 **Atomic Cursor + Store Fence** 实现无锁的单生产者多消费者 (SPMC) 模型。

## 2. 核心组件

### 2.1 行情录制器 (hft_md)
- **职责**: 独立进程，直接对接柜台 API (如 CTP)。
- **逻辑**: 
    1. 收到 API 回报后，压入内部 `RingBuffer<TickRecord>` 缓冲。
    2. 写入线程弹出数据，通过 `MmapWriter` 写入映射区域。
    3. 每写入一条记录，执行 `release` 屏障并原子更新 `.meta` 文件中的 `write_cursor`。

### 2.2 mmap 文件结构
采用 `.dat` (数据) + `.meta` (元数据) 双文件方案，定义于 `core/include/mmap_util.h`。

```cpp
// 元数据头 (4KB 对齐)
struct MetaHeader {
    std::atomic<uint64_t> write_cursor; // 已写入条数
    uint64_t capacity;                  // 总容量 (条数)
    char padding[4096 - 16];            // 补齐
};

// 数据记录 (定义于 protocol.h)
struct TickRecord {
    char symbol[32];
    uint32_t trading_day;
    uint64_t update_time;
    double last_price;
    // ... 包含五档行情等全字段
};
```

### 2.3 Replay 模块 (libmod_replay.so)
- **职责**: 作为“消费者”，从 Mmap 文件中提取行情并注入 `EventBus`。
- **模式**: 
    - **Ultra Low Latency**: 使用 `_mm_pause()` 轮询 `.meta` 文件的游标变化。
    - **Zero Context Switch**: 整个读取过程无需任何系统调用。

## 3. 优势
- **Crash-Safe**: 游标原子更新，系统崩溃后可根据 `.meta` 游标实现断点续传。
- **极速回测**: 回测引擎通过 `MmapReader` 直接挂载历史数据文件，访问速度等同于本地内存。
- **解耦**: 录制与交易进程完全隔离，互不干扰。
