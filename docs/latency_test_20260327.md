# 延迟测试总结（2026-03-27）

## 目标
- 评估并降低 `hft_engine` / `FactorDAG` / `Replay` 延迟尾部。
- 评估行情录制器（`hft_md/hft_recorder`）从回调到写盘的端到端时延，并定位尾部来源。

## 关键改动（已落地）
### 1) 计时框架（TSC + ns 输出）
- `include/latency_stats.h`
  - 默认使用 TSC 计时。
  - 日志输出改为 `p50_ns/p99_ns/p999_ns/max_ns`。
  - 日志节流也使用 TSC 时间源。

### 2) FactorDAG 去除字符串拷贝热点
- `modules/factor/factor_dag_module.cpp`
  - 缓存 `source_id/factor_name/symbol`，减少 `__strncpy_avx2`。

### 3) FactorDAG 计算路径缓冲优化
- 初版：递归共享 `input_pool_`（后修复）。
- 最终版：**每个节点私有 `inputs_cache`**，避免递归层共享导致分配/抖动。
  - `FactorNodeHandle` 增加 `inputs_cache`。
  - `eval_node` 复用 `inputs_cache`，仅在需要时 `reserve`。

### 4) Replay 计时补全
- `modules/replay/replay_module.cpp`
  - 修复计时缺参，改用 `EVENT_MARKET_DATA`。
  - 增加 `tick_latency_ / last_tick_log_ / log_interval_ms_`。

### 5) 录制器端到端时延
- `hft_md/include/Recorder.h`
  - `TickEnvelope` 增加 `recv_ticks / enqueue_ticks`。
  - `record_latency_` 容量增大（65536）。
  - 增加 `slow_log_threshold_ns_`。
- `hft_md/src/Recorder.cpp`
  - 回调处记录 `recv_ticks` / `enqueue_ticks`。
  - writer 侧拆分 `queue_ns` / `write_ns`，并在超阈值时打印。
- `hft_md/conf/config.yaml`
  - `prefault: true`
  - `slow_log_threshold_us: 2000`

### 6) Mmap 预触页（降低极端尾部）
- `core/include/mmap_util.h`
  - `MmapWriter` 新增 `prefault` 参数并支持预触页。
- `hft_md/src/Recorder.cpp`
  - 传入 `prefault_`。

### 7) 录制器队列改为批量 RingBuffer
- `hft_md/include/Recorder.h`
  - `RingBuffer` → `BatchRingBuffer`。
- `hft_md/src/Recorder.cpp`
  - writer 侧使用 `peek/advance` 批量消费（默认批次 256）。

## 关键测试结论
### A. FactorDAG / Replay
- **FactorDAG max 大幅下降**：从毫秒级下降到 **~0.19ms**。
- `p50` 在 **~0.4–0.6 µs**，`p99` 在 **<1 µs**。
- 主要瓶颈转为 **Factor 计算路径**（`eval_and_publish` + 各因子 `compute`）。

### B. 录制器端到端时延
- 常态 `p50` 约 **1–4 µs**，`p99` 多为 **几十到百微秒**。
- 尾部尖峰来源已定位：
  - **大多数尖峰来自排队**：`queue_ns ≈ total_ns`，`write_ns` 很小。
  - 少量尖峰来自写盘抖动（如 `write_ns` 出现 10ms+）。
- `prefault` 有效：**极端 50ms 级尖峰显著减少**，但仍有偶发 10ms+。

## 证据样例
- 队列导致尖峰：
  - `total_ns=4,944,379 queue_ns=4,936,709 write_ns=408`
- 写盘导致尖峰：
  - `total_ns=10,794,131 queue_ns=898 write_ns=10,791,633`

## 主要结论
1. **FactorDAG 优化有效**：内存分配抖动显著降低。
2. **Recorder 尾部主要是排队抖动**，不是写盘主导。
3. **写盘极端抖动**仍会偶发出现，但频率明显下降。

## 后续建议（可选）
1. **进一步压 p99**
   - 重点优化队列排队：
     - 更稳定的 writer 线程调度（绑核/优先级）。
     - 继续评估批量写效果与批次大小。
2. **减少写端统计开销**
   - 改为采样统计（每 N 条采样一次）。
   - 仅在超阈值时打印细节。
3. **持续观测**
   - 运行更长时间验证尾部稳定性。

## 相关命令（示例）
- perf 采样（用户态）：
  - `scripts/perf_record.sh 10 -u -o perf.data -- ./bin/hft_engine conf/config_factor_dag.yaml`
- perf 读取：
  - `perf report --stdio -f -i perf.data`

