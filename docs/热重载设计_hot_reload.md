# 热重载与插件管理设计方案 (Hot Reload & Plugin Management)

## 1. 目标 (Goal)
实现交易策略和风控模块的 **零停机 (Zero Downtime)** 更新。
- **热重载 (Hot Reload)**: 在不停止引擎或断开行情/交易连接的情况下，更新策略逻辑（替换 `.so`）。
- **动态卸载/加载 (Dynamic Unload/Load)**: 运行时动态添加或移除策略。
- **状态保留 (State Preservation - 可选)**: 允许策略在重载期间持久化关键状态。

## 2. 现状与变更需求 (Current Architecture vs. Required Changes)

### 现状
- **加载**: `HftEngine` 在启动时根据 `config.yaml` 加载所有插件。
- **EventBus**: `subscribe` 仅接受 `std::function`。无法识别哪个模块注册了哪个回调。
- **生命周期**: `init` -> `start` -> `run` -> `stop`。没有中间状态的停止/启动或卸载机制。

### 变更需求

#### 2.1 EventBus 升级
必须追踪每个订阅的 **所有者 (Owner)**，以便在插件卸载时安全移除它们。

**当前接口:**
```cpp
virtual void subscribe(EventType type, Handler handler) = 0;
```

**建议接口:**
```cpp
// 增加 owner 指针以识别订阅归属
virtual void subscribe(EventType type, void* owner, Handler handler) = 0;
virtual void unsubscribe_all(void* owner) = 0;
```
*注：这需要更新 `framework.h` 并重新编译所有模块。*

#### 2.2 插件管理器 (PluginManager)
将插件管理逻辑从 `HftEngine` 剥离到独立的 `PluginManager` 类中。

**职责:**
- `load_plugin(name, path, config)`
- `unload_plugin(name)`:
    1. 调用 `module->stop()`
    2. `event_bus->unsubscribe_all(module_ptr)`
    3. 销毁 `module` 实例。
    4. `dlclose` 句柄。
- `reload_plugin(name)`: 封装 卸载 + 加载 流程。

#### 2.3 热路径安全 (Hot Path Safety)
- `EventBus::publish` 循环必须防止并发修改（尽管目前是单线程）。
- **修改时机**: 重载只能在 EventBus **空闲** 或安全的“Tick 间隙”进行。
- 由于 `HftEngine` 主循环处于 sleep 或 wait 状态，我们可以在主循环或专用的管理线程中处理“管理指令”。
- **推荐方案**: 在主线程（Tick 之间）处理重载指令，避免在热路径上引入锁。

## 3. 实现细节 (Implementation Details)

### 3.1 框架接口变更 (`framework.h`)

```cpp
class EventBus {
public:
    // ...
    // 新增带 Owner 追踪的订阅接口
    virtual void subscribe(EventType type, void* owner, Handler handler) = 0;
    
    // 移除该 Owner 注册的所有回调
    virtual void unsubscribe(void* owner) = 0;
};
```

### 3.2 动态插件管理器 (Dynamic Plugin Manager)

```cpp
class PluginManager {
public:
    bool load(const std::string& name, const std::string& lib_path, const ConfigMap& conf);
    bool unload(const std::string& name);
    bool reload(const std::string& name);

private:
    struct PluginEntry {
        void* dl_handle;
        std::shared_ptr<IModule> instance;
        std::string lib_path;
        ConfigMap config;
    };
    std::unordered_map<std::string, PluginEntry> plugins_;
    EventBus* bus_;
};
```

### 3.3 指令接口 (Command Interface)
如何触发重载？
- **选项 A**: 文件监控 (监控 `conf/plugins.cmd` 等)。
- **选项 B**: Unix Domain Socket / FIFO (命名管道)。
- **选项 C**: 信号 (如 `SIGUSR1` 触发配置重读)。

**选定方案: 管理管道 (Admin FIFO)**
- 简单，CLI 友好 (例如 `echo "reload strategy_a" > hft.pipe`)。
- 在主循环中进行非阻塞读取。

### 3.4 状态持久化 (Phase 2 - 进阶)
为了避免重载时重置策略状态（如持仓、信号值）：

```cpp
class IModule {
public:
    // ...
    virtual std::string serialize_state() { return ""; }
    virtual void deserialize_state(const std::string& data) {}
};
```
*重载流程:*
1. `old_state = old_module->serialize_state()`
2. `unload(old_module)`
3. `load(new_module)`
4. `new_module->deserialize_state(old_state)`

## 4. 风险与缓解 (Risks & Mitigations)

| 风险 | 缓解措施 |
|------|------------|
| **悬空指针 (Dangling Pointers)** | `EventBus` 必须在模块卸载时激进地清理所有回调。 |
| **ABI 不匹配** | 确保 `framework.h` 版本一致性。在 `create_module` 中增加版本检查。 |
| **资源泄漏** | 确保 `IModule` 析构函数清理线程/文件。`dlclose` 处理静态对象。 |
| **部分更新失败** | 如果新 `.so` 加载失败，尽可能保持旧模块运行，或回退到安全模式。 |

## 5. 实施计划 (Implementation Plan)

1.  **重构 `EventBus`**: 增加 Owner 追踪和 `unsubscribe`。
2.  **更新插件**: 更新所有现有模块，在订阅时传入 `this`。
3.  **实现 `PluginManager`**: 封装加载逻辑。
4.  **实现 `CommandListener`**: 在 `HftEngine` 中增加非阻塞管道读取。
5.  **测试**: 使用模拟策略验证重载功能。
