# SignalCsv

**插件名称**：`SignalCsv`  
**类型**：Module  
**入口类**：`SignalCsvModule`

| name | type | required | default | range | effect | hot_path | example |
|---|---|---|---|---|---|---|---|
| `output_path` | `string` | no | `../log/signal.csv` | `path` | `输出CSV文件路径` | `no` | `"../log/signal.csv"` |
| `capacity` | `int` | no | `2^20` | `>=2` | `队列容量，过小可能导致阻塞` | `yes` | `1048576` |
| `flush_every` | `int` | no | `5000` | `>=0` | `每写入N行flush一次，0表示不强制` | `no` | `10000` |
| `include_header` | `bool` | no | `true` | `true/false` | `是否输出CSV表头` | `no` | `true` |
| `log_interval_ms` | `int` | no | `1000` | `>=0` | `状态日志输出间隔` | `no` | `2000` |

