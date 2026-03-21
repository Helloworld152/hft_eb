# 插件参数说明 (FactorDAGModule)

**插件名称**：`FactorDAGModule`  
**类型**：Module  
**入口类**：`FactorDAGModule`


| name               | type    | required | default | range      | effect      | hot_path | example                   |
| ------------------ | ------- | -------- | ------- | ---------- | ----------- | -------- | ------------------------- |
| `nodes`            | `array` | yes      | none    | 见示例        | 定义因子节点与动态库  | yes      | `[{id, library, params}]` |
| `edges`            | `array` | no       | `[]`    | 见示例        | 定义 DAG 依赖关系 | yes      | `[{from, to}]`            |
| `outputs`          | `array` | yes      | none    | 见示例        | 定义输出信号映射    | yes      | `[{node, factor_name}]`   |
| `trigger.on_tick`  | `bool`  | no       | `true`  | true/false | Tick 触发计算   | yes      | `true`                    |
| `trigger.on_kline` | `bool`  | no       | `false` | true/false | K线触发计算      | yes      | `false`                   |
| `trigger.on_timer` | `bool`  | no       | `false` | true/false | 定时触发计算     | yes      | `false`                   |
| `trigger.timer_interval_sec` | `int` | no | `1` | `>=1` | 定时触发间隔 | yes | `1` |


**节点 params 通用字段**（传入 IFactorNode::init）：

- `symbol`: 目标合约（可选），用于指定该节点只对指定合约更新
