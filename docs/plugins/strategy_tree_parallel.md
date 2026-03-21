# StrategyTreeParallel

**插件名称**：`StrategyTreeParallel`  
**类型**：Module  
**入口类**：`ParallelStrategyTreeModule`

| name | type | required | default | range | effect | hot_path | example |
|---|---|---|---|---|---|---|---|
| `parallel` | `bool` | no | `true` | `true/false` | `启用并行分片处理` | `no` | `true` |
| `shard_count` | `int` | no | `hardware_concurrency` | `>=1` | `分片线程数量，影响吞吐和顺序隔离` | `yes` | `8` |
| `queue_capacity` | `int` | no | `4096` | `>=2` | `每个分片队列容量，过小可能导致阻塞` | `yes` | `8192` |
| `shard_by` | `enum` | no | `symbol_id` | `symbol_id/symbol` | `路由键选择，影响同一symbol顺序保证` | `yes` | `symbol` |
| `publish_signals` | `bool` | no | `true` | `true/false` | `是否向全局总线发布信号` | `no` | `true` |

