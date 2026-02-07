# HFT 引擎中央脉搏与调度架构设计 (Centralized Timer & Scheduling Design)

## 1. 现状评估 (Current State)
目前系统采用“碎片化守护”模式，每个模块（如 CTP、Position、Monitor）为了执行低频维护任务，各自维护独立的 `std::thread`。
- **线程膨胀**：即便只有 3 个账号，系统线程数也高达 18 个。
- **管理混乱**：存在大量重复的 `while(running_)` 和 `sleep_for` 逻辑，且极易引发 `join()` 死锁或资源竞争。
- **不可测性**：各模块巡检频率不一，且相互独立，难以进行统一的状态监控和性能调试。

## 2. 目标架构：中央脉搏 (Target: Centralized Heartbeat)
核心思想是**“收归调度权”**。引擎引入一个统一的高精度定时器（脉搏），同步驱动所有模块的辅助逻辑。

### 2.1 架构层次
- **热路径 (Hot Path - 单线程全同步)**：
    - 链路：`Replay/MD API` -> `Strategy` -> `Risk` -> `Trade/CTP API`。
    - 驱动：由行情源线程直接贯通，不经过任何线程切换。
- **辅助路径 (Cold Path - 中央定时器驱动)**：
    - 链路：`Engine Timer` -> `IModule::on_timer()`。
    - 职责：重连逻辑、资金/持仓巡检、日志/数据持久化、心跳广播。

## 3. 核心修改方案

### 3.1 接口变更 (`include/framework.h`)
在 `IModule` 基类中增加 `on_timer()` 虚函数，默认实现为空：
```cpp
class IModule {
public:
    virtual void on_timer() {} // 每秒由引擎定时器触发一次
    // ... 其他接口
};
```

### 3.2 引擎内核变更 (`src/engine.cpp`)
在 `HftEngine::run` 的主循环中，将原有的简单 `sleep(10)` 细化为每秒一次的调度器：
- **逻辑**：每秒唤醒一次，遍历所有已加载插件，调用其 `on_timer()`。

### 3.3 模块重构 (以 `CtpRealModule` 为例)
- **删除**：`monitor_thread_`, `monitor_loop()`, `reconnecting_` 原子量。
- **实现**：
```cpp
void CtpRealModule::on_timer() {
    if (!logged_in_.load() && is_in_reconnect_time()) {
        auto now = std::chrono::steady_clock::now();
        if (now - last_reconnect_attempt_ >= std::chrono::seconds(reconnect_delay_sec_)) {
            do_connect(); // 执行 API 重启
            last_reconnect_attempt_ = now;
        }
    }
}
```

## 4. 预期收益 (Expected Benefits)
1. **线程精简**：预计可消灭 5-8 个冗余线程，系统总线程数降至 10 个左右。
2. **逻辑内聚**：业务逻辑不再夹杂线程生命周期管理代码，代码行数预计减少 20%。
3. **死锁免疫**：由于辅助逻辑在引擎主循环中串行执行，彻底规避了回调线程与守护线程相互 `join` 的死锁风险。
4. **确定性**：所有模块的巡检动作均对齐到秒级脉搏，极大地提升了系统的可观测性。

## 5. 实施路线图
1. **Phase 1**: 修改 `IModule` 接口及 `HftEngine` 调度器。
2. **Phase 2**: 重构 `CtpRealModule`，将其守护线程逻辑迁移至 `on_timer`。
3. **Phase 3**: 依次重构 `PositionModule` (资金查询) 和 `MonitorModule` (数据外送)。
