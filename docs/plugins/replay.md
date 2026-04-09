# Replay

**插件名称**：`Replay`  
**类型**：Module  
**入口类**：`ReplayModule`

| name | type | required | default | range | effect | hot_path | example |
|---|---|---|---|---|---|---|---|
| `data_file` | `string` | yes | `none` | `mmap基路径` | `指定回放数据源；未配置时模块无法正常工作` | `no` | `"data/market_data_20260319_night"` |
| `debug` | `bool` | no | `false` | `true/false/1/0` | `打印首批或采样 Tick 与阶段耗时，便于排查` | `yes` | `false` |
| `max_capacity` | `int` | no | `0` | `>=0` | `限制 mmap reader 的最大容量；0 表示沿用 meta 文件中的容量` | `no` | `1048576` |
| `idle_stop_sec` | `int` | no | `0` | `>=0` | `空闲超时后发布 `EVENT_ENGINE_STOP`，适合回测自动收尾` | `no` | `5` |
