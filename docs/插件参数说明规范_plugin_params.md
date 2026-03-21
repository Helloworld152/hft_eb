# 插件参数说明规范 (Plugin Parameter Spec)

目标：保证每个插件的参数**可读、可查、可追溯**，降低误配风险，便于审计与排障。

## 1. 适用范围
- 所有 `IModule` 插件
- 所有 `IStrategyNode` 策略/因子节点

## 2. 必须提供的参数说明
每个插件必须提供一份参数清单，包含以下字段：
1. `name`：参数名（与配置键一致）
2. `type`：类型（string/int/float/bool/enum/array/object）
3. `required`：是否必填
4. `default`：默认值（无则写 `none`）
5. `range`：取值范围或枚举（无则写 `none`）
6. `effect`：对功能/性能/延迟的影响
7. `hot_path`：是否影响热路径（yes/no）
8. `example`：最小示例值

## 3. 输出格式要求
- 放在插件文档中，或统一收敛到 `docs/plugins/` 下的单独文件
- 建议使用表格，便于检索
- 如参数较多，可按功能分组，但禁止省略字段

## 4. 模板
以下为推荐模板（可直接复制）：

**插件名称**：`<plugin_id>`
**类型**：Module / StrategyNode
**入口类**：`<class_name>`

| name | type | required | default | range | effect | hot_path | example |
|---|---|---|---|---|---|---|---|
| `<param>` | `string` | `yes` | `none` | `A/B/C` | `影响...` | `yes/no` | `"xxx"` |

## 5. 最低要求
- 新增插件必须在同一提交中补齐参数说明
- 修改参数语义必须同步更新参数说明
- 删除参数必须在文档中注明删除时间和替代方案

## 6. 例外
- 仅用于调试的临时参数，允许标注为 `debug_only`
- 但仍需写清楚默认值和是否影响热路径

