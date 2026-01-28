# HFT Market Data Recorder (hft_md)

## 1. 核心设计哲学
- **Do One Thing and Do It Well**: 只负责行情落地，不处理业务逻辑。
- **IPC over Mmap**: 采用预分配的 Mmap 文件作为持久化存储和跨进程通信的媒介。
- **Zero Copy Writer**: 直接将 `TickRecord` 写入内存映射区域，消除内核态拷贝。
- **Crash Safe**: 通过独立的 `.meta` 文件原子更新 `write_cursor`，支持断点续传。

## 2. 模块职责
- **输入**: 监听 CTP 行情接口。
- **输出**:
    - 数据文件: `market_data_YYYYMMDD.dat` (默认预分配 1GB/500万条)
    - 元数据文件: `market_data_YYYYMMDD.meta` (4KB 对齐头信息)
- **同步机制**:
    1. 写入数据到 `.dat` 对应偏移位置。
    2. 执行 `std::atomic_thread_fence(std::memory_order_release)`：确保数据写入先于游标更新对其他进程可见。
    3. 原子递增 `.meta` 中的 `write_cursor`。

## 3. 架构优势
- **极低延迟**: Reader (如 `hft_engine` 中的 `ReplayModule`) 通过 `_mm_pause()` 轮询 `.meta` 实现亚微秒级的行情获取，完全跳过系统调用。
- **解耦**: 录制进程与交易进程完全隔离，录制崩溃不影响交易，反之亦然。
- **持久化**: 数据天然落盘，无需额外的归档步骤。

## 4. 运行
```bash
# 启动录制器 (需配置 conf/config.json)
./run.sh
```
---
