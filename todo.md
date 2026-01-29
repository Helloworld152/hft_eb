# HFT-EDA Optimization TODO

## 1. EventBus 性能压榨 (Hot Path)
- [ ] **去 std::function**: 使用 `FastDelegate` (Raw FP + Context) 替换 `std::function`，消除虚函数调用和潜在的堆分配开销。
- [ ] **消除动态内存分配**: 将 `EventBusImpl` 中的 `std::vector` 替换为固定大小的 `std::array` (Fixed-size Subscriber Slot)，消除二重指针跳转。
- [ ] **分支预测优化**: 在 `publish` 的入口检查处添加 `__builtin_expect` 或 `[[likely]]` 指令。
- [ ] **缓存行对齐**: 确保 `EventSlot` 的内存布局对 CPU Cache 友好。

## 2. 核心架构优化
- [ ] **异步日志系统**: 实现无锁 RingBuffer 日志，将 `std::cout` 移出热路径，防止 IO 阻塞驱动线程。
- [ ] **去虚化 (Devirtualization)**: 评估 `EventBus` 接口是否可以改为模板或直接实现，以支持编译器内联。

## 3. 插件适配
- [ ] 按照新的 `EventHandler` 接口重构所有现有插件 (Strategy, Risk, Trade, etc.)。
