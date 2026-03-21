# 插件参数说明 (SmaFactor)

**插件名称**：`SmaFactor`  
**类型**：FactorNode  
**入口类**：`SmaFactor`

| name | type | required | default | range | effect | hot_path | example |
|---|---|---|---|---|---|---|---|
| `window` | `int` | no | `5` | `>=1` | SMA 窗口长度 | yes | `20` |
| `window_size` | `int` | no | `5` | `>=1` | `window` 的别名 | yes | `20` |
| `debug` | `bool` | no | `false` | true/false | 打印每次计算结果 | no | `false` |
| `symbol` | `string` | no | `none` | 合约名 | 指定合约过滤 | yes | `"rb2405"` |
