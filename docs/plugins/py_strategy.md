# PyStrategy

**插件名称**：`PyStrategy`  
**类型**：Module  
**入口类**：`PyStrategyModule`

| name | type | required | default | range | effect | hot_path | example |
|---|---|---|---|---|---|---|---|
| `py_module` | `string` | no | `py_tools.strategies.sample_strategy` | `python import path` | `指定待加载的 Python 模块` | `no` | `"py_tools.strategies.sample_strategy"` |
| `py_class` | `string` | no | `SampleStrategy` | `class name` | `指定策略类名；必须可实例化` | `no` | `"SampleStrategy"` |
| `py_path` | `string` | no | `""` | `path` | `追加到 `sys.path`，便于加载仓库外或相对路径策略` | `no` | `"."` |
| `default_account` | `string` | no | `""` | `account id` | `Python 侧未显式传账户时作为默认下单账户` | `no` | `"sim"` |
| `symbol_filter` | `string` | no | `none` | `逗号分隔品种列表` | `只让指定 symbol 的 Tick/K 线进入 Python 回调，降低调用频率` | `yes` | `"au2606,rb2405"` |
| `sample_every` | `int` | no | `1` | `>=1` | `每 N 条 Tick/K 线调用一次 Python 策略` | `yes` | `10` |
| `error_policy` | `enum` | no | `disable` | `ignore/disable/stop` | `Python 回调异常后的处理策略：忽略、禁用策略或停止引擎` | `no` | `"disable"` |
