# 极速行情流设计 (High-Speed Data Stream)

## 1. 设计目标
在高频交易中，Tick 数据的存储和读取速度直接影响回测精度和实时因子的计算。
- **Zero-Copy**: 使用 `mmap` 技术实现进程间共享内存，避免内核态文件 I/O 拷贝。
- **Lock-Free**: 基于 **Atomic Cursor + Store Fence** 实现无锁的单生产者多消费者 (SPMC) 模型。
- **Batch Processing**: 支持批量读写以分摊内存屏障（Memory Fence）开销。

## 2. 核心组件

### 2.1 行情录制器 (hft_md)
- **职责**: 独立进程，直接对接柜台 API (如 CTP)。
- **缓冲**: 内部使用 `BatchRingBuffer<TickRecord>` 进行平滑处理，应对突发流量。
- **持久化**: 
    1. 写入线程批量从 RingBuffer 获取数据。
    2. 通过 `MmapWriter` 直接写入磁盘映射区域 (`.dat` 文件)。
    3. 执行 `release` 屏障并原子更新 `.meta` 文件中的 `write_cursor`。

### 2.2 IPC 机制 (Inter-Process Communication)

系统支持两种 IPC 模式，分别针对不同的消费场景：

#### A. 线性日志模式 (Linear Append Log)
- **实现**: `core/include/mmap_util.h`
- **特点**: 全量历史记录，支持回放。
- **结构**: 
    - `.dat`: 连续存储的 Tick 数组。
    - `.meta`: 4KB 对齐的元数据，包含 `write_cursor` 和 `capacity`。
- **适用**: 策略回测、K线生成、盘后分析。

#### B. 实时快照模式 (Real-Time Snapshot)
- **实现**: `core/include/shm_snapshot.h` (详见 `docs/共享内存快照设计_shm_snapshot.md`)
- **特点**: 仅保留每个合约的**最新一帧**数据，基于 SeqLock 实现无锁读写。
- **适用**: 预风控 (Pre-Trade Risk)、实时监控 GUI。

### 2.3 Replay 模块 (libmod_replay.so)
- **职责**: 作为“消费者”，从 Mmap Log 文件中提取行情并注入 `EventBus`。
- **模式**: 
    - **Ultra Low Latency**: 使用 `_mm_pause()` 轮询 `.meta` 文件的游标变化。
    - **Zero Context Switch**: 整个读取过程无需任何系统调用。
    - **Prefetch**: 支持 `__builtin_prefetch` 预取下一条 Tick，掩盖内存延迟。

## 3. 关键数据结构

### 3.1 TickRecord (Aligned)
定义于 `core/include/protocol.h`，强制 **64字节对齐** 以适配 Cache Line。

```cpp
struct alignas(64) TickRecord {
    char symbol[32];
    uint64_t symbol_id;
    uint32_t trading_day;
    uint64_t update_time;
    double last_price;
    int volume;
    double turnover;
    double open_interest;
    // ... (五档行情 & 统计数据)
};
```

### 3.2 BatchRingBuffer
定义于 `core/include/ring_buffer.h`。
- **Shadow Index**: 缓存消费者/生产者的游标，减少对共享原子变量的争用（False Sharing 防护）。
- **Reserve-Commit**: 支持两阶段提交，允许直接在 Buffer 内存上构造对象，避免中间拷贝。

## 4. 优势
- **Crash-Safe**: 游标原子更新，系统崩溃后可根据 `.meta` 游标实现断点续传。
- **极速回测**: 回测引擎通过 `MmapReader` 直接挂载历史数据文件，访问速度等同于本地内存。
- **解耦**: 录制与交易进程完全隔离，互不干扰。