# 插件参数说明 (ImbalanceFactor)

**插件名称**：`ImbalanceFactor`  
**类型**：FactorNode  
**入口类**：`ImbalanceFactor`

| name | type | required | default | range | effect | hot_path | example |
|---|---|---|---|---|---|---|---|
| `debug` | `bool` | no | `false` | true/false | 打印每次计算结果 | no | `false` |
| `symbol` | `string` | no | `none` | 合约名 | 指定合约过滤 | yes | `"rb2405"` |
