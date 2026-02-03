# Real-Time Market Data Snapshot Design (SHM)

## 1. 概述 (Overview)
本设计文档定义了用于存储**实时最新行情 (Latest Snapshot)** 的共享内存结构。
与追加模式的 `Mmap Log` 不同，本模块仅保存每个合约的**最新一帧**数据，供风控、策略监控、GUI 等对实时性要求极高但不需要历史 Tick 的组件使用。

## 2. 核心需求
- **极低延迟 (Ultra Low Latency)**: 读取最新价格不应涉及系统调用或复杂的查找算法。
- **高并发 (Concurrency)**: 支持“一写多读” (SPMC)，写操作不应阻塞读操作。
- **数据一致性 (Consistency)**: 读者必须读到完整的 Tick，不能读到“撕裂”的数据 (Torn Read)。

## 3. 存储架构 (Storage Architecture)

### 3.1 文件映射
- **文件路径**: `/dev/shm/hft_snapshot.dat` (直接位于内存文件系统，重启丢失，无需持久化)
- **大小**: `MAX_SYMBOLS * sizeof(SnapshotSlot)`

### 3.2 内存布局 (Memory Layout)
整个 SHM 是一个定长数组，直接通过下标索引。

```cpp
// 槽位结构 (Cache Line Aligned)
struct alignas(128) SnapshotSlot {
    std::atomic<uint32_t> seq;  // 版本号 (SeqLock)
    uint32_t padding;           // 内存对齐填充
    TickRecord tick;            // 实际行情数据
    char _pad[...];             // 填充至 128/256 字节，防止 False Sharing
};

// 整体结构
struct SnapshotShm {
    uint64_t magic_num;         // 0xAABBCCDDEEFF
    uint64_t symbol_count;      // 实际生效的合约数
    SnapshotSlot slots[MAX_SYMBOLS]; 
};
```

## 4. 读写协议 (SeqLock Protocol)
采用 **Sequence Lock (SeqLock)** 实现无锁一致性读写。

### 4.1 写入流程 (Writer)
仅 `hft_md` (行情录制/转发进程) 可写。

```cpp
void update(int index, const TickRecord& new_tick) {
    SnapshotSlot& slot = shm->slots[index];
    
    // 1. 版本号 + 1 (变为奇数)，标记正在写入
    // memory_order_release 确保后续写入不会重排到此之前
    uint32_t seq = slot.seq.load(std::memory_order_relaxed);
    slot.seq.store(seq + 1, std::memory_order_release);

    // 2. 写入数据
    slot.tick = new_tick;

    // 3. 版本号 + 1 (变为偶数)，标记写入完成
    // memory_order_release 确保数据写入先于版本号更新
    slot.seq.store(seq + 2, std::memory_order_release);
}
```

### 4.2 读取流程 (Reader)
策略、风控等进程只读。

```cpp
bool read(int index, TickRecord& out_tick) {
    SnapshotSlot& slot = shm->slots[index];
    uint32_t seq1, seq2;
    
    do {
        // 1. 读取开始前的版本号
        seq1 = slot.seq.load(std::memory_order_acquire);
        
        // 如果是奇数，说明正在写，自旋等待或立即重试
        if (seq1 & 1) {
            _mm_pause(); // 优化 CPU 流水线
            continue;
        }

        // 2. 拷贝数据 (Optimistic Read)
        out_tick = slot.tick;
        
        // 3. 内存屏障，防止乱序
        std::atomic_thread_fence(std::memory_order_acquire);

        // 4. 读取结束后的版本号
        seq2 = slot.seq.load(std::memory_order_acquire);

    } while (seq1 != seq2); // 如果版本号变化，说明读的过程中发生了写，重试

    return true;
}
```

## 5. 索引映射 (Index Mapping)
为了实现 O(1) 访问，需要将 `Symbol ID` 映射为数组下标 `0 ~ N`。

- **方案**: `index = SymbolID - BASE_ID`
- **配置**: `conf/symbols.txt` 中 ID 连续 (e.g. 10000001 ... 10000862)。
- **BASE_ID**: `10000000` (硬编码或配置读取)

例如: `AP603` (ID 10000001) -> Index 1.

## 6. 应用场景对比

| 特性 | Snapshot SHM (本设计) | Mmap Log (原设计) |
| :--- | :--- | :--- |
| **数据内容** | 仅最新一条 Tick | 全量历史 Tick 流 |
| **访问方式** | 随机访问 (Random Access by ID) | 顺序扫描 (Linear Scan) |
| **典型用途** | 预风控(Pre-Trade Risk), GUI监控 | 策略回测, 信号计算, 盘后分析 |
| **延迟** | 纳秒级 (Direct Memory Access) | 微秒级 (需遍历或轮询) |
| **持久化** | 否 (/dev/shm, 重启即失) | 是 (落盘保存) |

