# 插件参数说明 (VolatilityFactor)

**插件名称**：`VolatilityFactor`  
**类型**：FactorNode  
**入口类**：`VolatilityFactor`

| name | type | required | default | range | effect | hot_path | example |
|---|---|---|---|---|---|---|---|
| `window` | `int` | no | `20` | `>=1` | 波动率窗口长度 | yes | `20` |
| `window_size` | `int` | no | `20` | `>=1` | `window` 的别名 | yes | `20` |
| `debug` | `bool` | no | `false` | true/false | 打印每次计算结果 | no | `false` |
| `symbol` | `string` | no | `none` | 合约名 | 指定合约过滤 | yes | `"rb2405"` |
