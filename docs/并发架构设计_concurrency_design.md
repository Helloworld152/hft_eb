# HFT High-Concurrency Architecture Design (HFT-HCA) v2.0

> **Status**: Approved
> **Author**: Linus (AI Persona)
> **Date**: 2026-02-02
> **Philosophy**: Decentralized Shared Memory, Pull-Based, Zero Copy.

## 1. 核心理念：去中心化总线 (Decentralized Bus)

放弃传统的“中央总线”模式，采用 **“每个人只管好自己的一亩三分地” (Publisher-Centric)** 的设计。

-   **写入 (Publish)**: 每个模块（进程）只负责**写入自己拥有的那块共享内存**。例如，行情录制器只写 `hft_md.shm`，风控模块只写 `hft_risk.shm`。
-   **读取 (Subscribe)**: 如果模块 A 需要模块 B 的数据，模块 A 主动去 `attach` 模块 B 的共享内存，并维护自己的读取游标。

这种 **M x N** 的网状拓扑消除了中央瓶颈，实现了真正的解耦。

## 2. 系统拓扑 (System Topology)

```mermaid
graph TD
    %% Shared Memory Blocks (Data Sources)
    subgraph Shared_Memory [Shared Memory Layer]
        SHM_MD[hft_md.shm<br/>(TickRecord)]
        SHM_STRAT_A[hft_strat_a.shm<br/>(OrderReq)]
        SHM_STRAT_B[hft_strat_b.shm<br/>(OrderReq)]
        SHM_RISK[hft_risk.shm<br/>(OrderSend)]
        SHM_TRADE[hft_trade.shm<br/>(TradeRtn)]
    end

    %% Processes
    subgraph Proc_Recorder [Process: Recorder]
        Writer_MD((Writer)) -->|Write| SHM_MD
    end

    subgraph Proc_Strategy_A [Process: Strategy A]
        Reader_MD_A((Reader)) -.->|Read| SHM_MD
        Reader_Trade_A((Reader)) -.->|Read| SHM_TRADE
        
        Logic_A[Strategy Logic]
        
        Writer_Strat_A((Writer)) -->|Write| SHM_STRAT_A
        
        Reader_MD_A --> Logic_A
        Reader_Trade_A --> Logic_A
        Logic_A --> Writer_Strat_A
    end

    subgraph Proc_Strategy_B [Process: Strategy B]
        Reader_MD_B((Reader)) -.->|Read| SHM_MD
        Logic_B[Strategy Logic]
        Writer_Strat_B((Writer)) -->|Write| SHM_STRAT_B
        
        Reader_MD_B --> Logic_B
        Logic_B --> Writer_Strat_B
    end

    subgraph Proc_Risk [Process: Risk Manager]
        Reader_Strat_A((Reader)) -.->|Read| SHM_STRAT_A
        Reader_Strat_B((Reader)) -.->|Read| SHM_STRAT_B
        
        Risk_Engine[Risk Check]
        
        Writer_Risk((Writer)) -->|Write| SHM_RISK
        
        Reader_Strat_A --> Risk_Engine
        Reader_Strat_B --> Risk_Engine
        Risk_Engine --> Writer_Risk
    end

    subgraph Proc_Trade [Process: Trade Gateway]
        Reader_Risk((Reader)) -.->|Read| SHM_RISK
        Writer_Trade((Writer)) -->|Write| SHM_TRADE
        
        CTP_API[CTP Trader API]
        
        Reader_Risk --> CTP_API
        CTP_API --> Writer_Trade
    end
```

## 3. 关键组件设计

### 3.1 IPC Channel (One-Way Pipe)

每个共享内存文件本质上是一个 **SPSC (Single Producer, Multi-Consumer)** 的广播管道。

-   **Writer**: 拥有对该 `.shm` 文件的写权限。维护 `write_cursor`。
-   **Reader(s)**: 只读映射。每个 Reader 进程在自己的内存空间内维护 `read_cursor`。无需在共享内存中同步读游标（彻底消除读写竞争）。

### 3.2 配置文件结构 (`config.yaml`)

不再需要复杂的 EventBus 配置，而是配置“输入源”和“输出源”。

```yaml
# Strategy A Configuration
module_name: "strategy_a"

ipc:
  # 我负责写入的地方
  output:
    path: "/dev/shm/hft_strat_a.shm"
    size: 1048576 # 1MB

  # 我需要监听的来源
  inputs:
    - name: "market_data"
      path: "/dev/shm/hft_md.shm"
      type: "TICK"
    - name: "trade_return"
      path: "/dev/shm/hft_trade.shm"
      type: "TRADE"
```

### 3.3 聚合器 (The Aggregator)

在进程内部，需要一个 `Poller` 线程来轮询所有 `inputs`。

```cpp
void poller_loop() {
    while (running) {
        bool idle = true;
        
        // 轮询所有输入源
        for (auto& reader : readers) {
            while (auto msg = reader->pop()) {
                // 收到消息，注入进程内 EventBus
                internal_bus->publish(msg->type, msg->data);
                idle = false;
            }
        }

        if (idle) {
            _mm_pause(); // 只有当所有源都空闲时才 Pause
        }
    }
}
```

## 4. 优势总结

1.  **极致解耦**: 增加一个新的策略进程不需要修改风控或行情的任何代码，只需要告诉风控“多监听一个文件”。
2.  **故障隔离**: 策略 A 挂了，它的 SHM 文件还在，风控读不到新数据而已，不会崩溃。
3.  **调试友好**: 可以随时写一个小工具 `dump_shm` 查看任何一个环节的实时数据流，就像 Wireshark 抓包一样。
4.  **无锁**: 全程无锁。Writer 独占写，Reader 独占读游标。

## 5. 开发路线

1.  **Refactor MmapUtil**: 升级 `core/include/mmap_util.h`，使其支持通用的环形队列头结构，不仅仅是 TickRecord。
2.  **Implement IPC Connector**: 实现通用的 `IpcWriter` 和 `IpcReader`。
3.  **Engine Integration**: 在 `HftEngine` 启动时，根据配置自动创建 Reader 线程，将外部 SHM 数据桥接到内部 EventBus。

This is the Way.