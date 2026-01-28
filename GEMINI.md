# HFT Event-Driven Architecture (HFT-EDA)

## 1. 核心设计哲学
- **Zero Copy**: 事件在总线上传递时仅传递指针（`void*`），杜绝内存拷贝。
- **Lock Free**: 在关键路径（Hot Path）上采用单线程同步调用，避免互斥锁竞争。
- **IPC RingBuffer (New)**: 采用 **Mmap + Atomic Cursor** 实现跨进程/跨模块的无锁极低延迟数据传输。
- **Plugin Architecture**: 基于动态库（`.so`）的插件系统，支持运行时加载和配置。
- **Risk First**: 风控模块作为交易的强制前置过滤器。

## 2. 系统拓扑 (Updated 2026-01-28)

```mermaid
graph TD
    %% 核心引擎
    Engine[HftEngine<br/>Plugin Loader]
    EventBus{EventBus<br/>同步分发器}

    %% 外部进程/工具
    Recorder(hft_recorder<br/>独立进程)
    DataFile[(Mmap File<br/>.dat + .meta)]

    %% 插件层
    subgraph Modules [Plugins]
        MD[libmod_ctp.so<br/>Market Sim]
        Replay[libmod_replay.so<br/>DataFeed Replay]
        Strategy[libmod_strategy.so<br/>Simple Strategy]
        Risk[libmod_risk.so<br/>Risk Control]
        PosMgr[libmod_position.so<br/>Position Mgr]
        Trade[libmod_trade.so<br/>Simple Trade]
        CTP_Trade[libmod_ctp_real.so<br/>CTP Real Trade]
    end

    %% 数据流 (Hot Path)
    Recorder -->|Write (Mmap)| DataFile
    DataFile -->|Read (Mmap)| Replay
    
    Replay -->|EVENT_MARKET_DATA| EventBus
    MD -->|EVENT_MARKET_DATA| EventBus
    
    EventBus -->|dispatch| Strategy
    EventBus -->|dispatch| Risk

    Strategy -->|EVENT_ORDER_REQ| EventBus
    EventBus -->|dispatch| Trade
    EventBus -->|dispatch| Risk
    
    Risk -->|EVENT_ORDER_SEND| EventBus
    EventBus -->|dispatch| CTP_Trade

    CTP_Trade -->|EVENT_RTN_TRADE| EventBus
    Trade -->|EVENT_RTN_TRADE| EventBus
    EventBus -->|dispatch| PosMgr

    PosMgr -->|EVENT_POS_UPDATE| EventBus
    EventBus -->|dispatch| Strategy
```

## 3. 核心组件

### A. Infrastructure (基础设施层)
- **HftEngine**: 管理插件生命周期。
- **EventBus**: 同步事件分发器。
- **Core Lib**: `core/include/`，包含 `protocol.h`, `ring_buffer.h`, `mmap_util.h` (IPC 核心) 等共享组件。

### B. Protocol (`framework.h` & `protocol.h`)
- `TickRecord` (Mmap IPC & EventBus): 全字段高精度行情结构。
- `EVENT_MARKET_DATA`: 行情事件。
- `EVENT_ORDER_REQ`: 策略发出的报单请求。
- `EVENT_ORDER_SEND`: 经风控批准后的报单指令。
- `EVENT_RTN_TRADE` / `EVENT_RTN_ORDER`: 成交及状态回报。

### C. 模块清单

#### 1. Replay Module (`modules/replay`)
- **功能**: 高性能 DataFeed，支持回测与实时旁路。
- **逻辑**: 通过 `MmapReader` 读取录制器生成的 `.dat` 和 `.meta` 文件。
- **特性**: 采用 `_mm_pause()` 进行无锁超低延迟轮询，直接将 `TickRecord` 注入总线，实现 Zero Copy。

#### 2. Recorder (`hft_md`)
- **位置**: `hft_eb/hft_md`
- **功能**: 独立进程，连接 CTP 并通过 `MmapWriter` 预分配写入行情数据。
- **特性**: 自动维护 `.meta` 游标，支持 Crash-Safe 断点续传。

#### 3. Strategy Module (`modules/strategy`)
- **功能**: 策略逻辑实现。
- **逻辑**: 监听 `MARKET_DATA`，发布 `ORDER_REQ`。

#### 4. Risk Module (`modules/risk`)
- **功能**: 交易前风控。
- **逻辑**: 拦截 `ORDER_REQ`，审核后发布 `ORDER_SEND`。

#### 5. Trade Module (`modules/trade`)
- **功能**: 模拟交易执行。
- **逻辑**: 监听 `ORDER_REQ`，模拟成交并返回 `RTN_TRADE`。

#### 6. CTP Real Module (`modules/ctp_real`)
- **功能**: 实盘交易执行（生产环境）。
- **逻辑**: 监听 `ORDER_SEND`，对接 CTP API。

#### 7. Position Module (`modules/position`)
- **功能**: 持仓账本。
- **逻辑**: 监听 `RTN_TRADE`，实时维护多空持仓与盈亏。

## 4. 目录结构 (Updated)

```
hft_eb/
├── core/                    # 公共核心库 (Mmap, Protocol, IPC)
│   └── include/
├── include/                 # 引擎接口 (framework.h, engine.h)
├── src/                     # 引擎实现
├── modules/                 # 插件模块
│   ├── replay/              # DataFeed 回放
│   ├── risk/
│   ├── strategy/
│   └── ...
├── hft_md/                  # 行情录制子项目 (Independent Process)
│   ├── src/
│   └── tools/               # 数据工具 (reader, test)
├── conf/
│   ├── config_full.json     # 全功能配置
│   └── config_replay.json   # 回测配置
├── data/                    # 行情数据落地 (Mmap files)
└── bin/                     # 编译产出
```

## 5. 构建与运行

### 编译
```bash
# 编译引擎及所有插件
bash build_release.sh

# 编译录制器
cd hft_md && bash build.sh
```

### 运行回测/回放
```bash
cd bin
./hft_engine ../conf/config_replay.json
```
*(需确保 `conf/config_replay.json` 中的 `data_file` 指向真实存在的 mmap 文件路径)*