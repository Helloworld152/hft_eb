# 极速行情录制器设计 (Tick Recorder)

## 1. 设计目标
`hft_md` 的核心职责是将总线上的实时 Tick 数据以极低延迟持久化到磁盘，为后续的回测和分析提供标准数据源。
- **Zero-Copy**: 使用 `mmap` 直接操作磁盘映射内存。
- **Lock-Free**: 生产者（总线回调）与消费者（落盘线程）之间通过无锁队列同步。
- **Sequential Write**: 保证数据在磁盘上的物理连续性，优化读取性能。

## 2. 核心组件

### 2.1 行情录制器 (hft_md)
- **职责**: 独立进程，直接对接柜台 API (如 CTP)。
- **逻辑**: 
    1. 收到 API 回报后，压入内部 `RingBuffer<TickRecord>` 缓冲。
    2. 写入线程弹出数据，通过 `MmapWriter` 写入映射区域。
    3. 写入的是 `TickRecord` 结构。

### 2.2 存储格式 (Binary)
采用定长二进制格式，拒绝 CSV/JSON 以节省空间并消除序列化开销。

```cpp
// header (64 bytes aligned)
struct FileHeader {
    uint32_t version;
    char symbol[32];
    uint64_t date; // YYYYMMDD
    uint32_t count;
    char reserved[12];
};

// data body
// 直接存储 protocol.h 中的 TickRecord 结构体
```

## 3. 文件管理
- **One File Per Day/Symbol**: 每个合约每天生成一个 `.dat` 文件，路径规则：`data/tick/YYYYMMDD/SYMBOL.dat`。
- **Pre-allocation**: 程序启动时预分配 50MB 空间（约 100w 行情），避免运行时动态扩容导致的系统调用。