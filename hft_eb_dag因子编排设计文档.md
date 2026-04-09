# hft_eb 中基于配置文件构建 DAG 的因子编排设计文档

## 1. 文档目的

本文档用于定义在现有 `hft_eb` 高频交易核心架构下，引入基于配置文件构建有向无环图（DAG）的因子编排与执行机制。目标是在不破坏现有事件总线、插件化、配置驱动、流式处理架构的前提下，提供一套可扩展、可复用、可低延迟运行的因子执行框架。

本文档重点回答以下问题：

- 如何从配置文件构建因子 DAG
- DAG 在 `hft_eb` 架构中的角色与边界
- DAG 如何与现有 pub/sub 事件总线集成
- 因子节点、算子、运行时状态如何组织
- 如何兼顾工程可维护性与低延迟性能
- 当前示例配置应如何演进为生产可用方案

---

## 2. 背景与现状

现有系统具备以下核心特征：

- 高频交易核心为 `hft_eb`
- 系统通过插件化加载业务模块
- 系统通过配置文件组织交易流
- 事件通过 pub/sub 机制传播
- 上游模块通过函数指针注册到事件总线，供下游调用

在该架构下，简单因子可以直接作为插件挂接到事件总线上执行。但随着因子数量增加，会出现以下问题：

1. 因子之间存在依赖关系，手工组织执行顺序复杂
2. 多个因子共享相同中间结果，容易重复计算
3. 节点初始化、状态管理、输出发布逻辑分散
4. 配置文件仅能表达模块启停，难以表达因子图依赖
5. 后续扩展截面因子、组合因子、复杂派生信号时，人工维护成本高

因此需要引入 DAG 机制，将因子依赖关系显式化，并由统一运行时负责规划与执行。

---

## 3. 设计目标

### 3.1 功能目标

1. 支持从配置文件构建因子 DAG
2. 支持因子节点依赖关系描述
3. 支持在启动阶段进行图校验与拓扑排序
4. 支持按事件触发增量执行 DAG
5. 支持输出节点结果发布到事件总线
6. 支持节点 warmup、ready、publish 策略控制
7. 支持后续扩展多标的、多触发域、截面节点

### 3.2 非功能目标

1. 不破坏现有 `hft_eb` 事件总线模型
2. 热路径执行尽量平铺，避免深层递归
3. 避免大量小对象与虚函数造成 cache miss
4. 状态存储连续化，便于低延迟执行
5. 配置应清晰表达逻辑依赖，而非脚本式执行顺序
6. 插件接口稳定，便于扩展与部署

### 3.3 非目标

本文档不追求实现一个完全通用的图数据库或分布式 DAG 引擎，设计范围限定为：

- 本地单进程
- 流式因子计算
- 事件驱动执行
- 高频交易/回放/研究一体化运行时

---

## 4. 总体设计原则

### 4.1 不新建平行框架，DAG 是 `hft_eb` 之上的派生流编排层

DAG 不应被设计成独立于事件总线之外的第二套运行时，而应作为 `hft_eb` 之上的一层编排与执行机制。

换言之：

- `hft_eb` 负责事件路由与 handler 注册
- `FactorDAG` 负责读取配置、构图、编译执行计划、运行时增量更新
- 策略与下游模块继续通过 topic 订阅因子输出

### 4.2 DAG 表达依赖关系，不直接等价于执行脚本

配置中的边表示“数据依赖”，不是“脚本顺序”。

例如：

- `LAST_PRICE -> SMA_20` 表示 `SMA_20` 依赖 `LAST_PRICE`
- 运行时可根据拓扑序、触发域、ready 状态决定具体执行

### 4.3 热路径以 Push 为主，冷路径可 Lazy

在流式高频架构下，主路径必须是事件触发的增量更新：

- tick 到来时更新基础节点与热点派生节点
- 常用因子尽量在事件到来时直接 materialize
- 少量不常用指标可以 dirty 标记并延迟计算

### 4.4 插件更适合作为算子模块，而不是每个因子一个 so

短期验证阶段可以接受“每个节点一个 so”，但长期建议演进为：

- 模块级插件提供算子
- DAG 配置实例化算子节点
- Runtime 统一管理状态与依赖

即从“因子插件编排器”演进为“算子编排器”。

---

## 5. 架构设计

### 5.1 逻辑架构

```text
行情源/回放源插件
    -> hft_eb 事件总线
        -> FactorDAG 插件
            -> DAG Planner
            -> DAG Runtime
            -> 因子输出发布
        -> 策略插件/记录插件/OMS/Risk
```

### 5.2 模块划分

系统新增或涉及的核心模块如下：

#### 5.2.1 FactorDAG 插件

职责：

- 读取 DAG 配置
- 构建节点与依赖图
- 完成校验与拓扑排序
- 初始化节点算子与状态
- 向 `hft_eb` 注册事件处理入口
- 在运行时按触发计划执行节点
- 将输出节点结果发布到 bus

#### 5.2.2 DAG Planner

职责：

- 解析配置
- 建立 `node_id -> index` 映射
- 生成邻接表与入度表
- 检测环
- 生成拓扑序
- 校验节点参数和输入数量
- 编译触发计划、发布计划、状态布局

#### 5.2.3 DAG Runtime

职责：

- 管理 source 节点值
- 管理节点 current value / version / ready / dirty
- 管理状态内存 arena
- 按触发计划执行节点
- 控制 publish 行为

#### 5.2.4 Operator Registry（长期演进方向）

职责：

- 注册算子实现
- 通过 `op` 名称返回算子 vtable 或工厂
- 将实现来源与 DAG 配置解耦

#### 5.2.5 输出消费模块

包括：

- 策略插件
- CSV 记录插件
- 调试监控插件
- 研究分析模块

这些模块不直接感知 DAG 内部结构，只通过 bus topic 订阅结果。

---

## 6. 配置文件设计

### 6.1 当前配置示例的问题

当前示例配置可以表达图，但存在以下不足：

1. 节点以 `library` 绑定实现，粒度过细
2. 多输入节点的输入顺序不明确
3. `SPREAD`、`IMBALANCE` 等派生节点未显式表达其基础依赖
4. `symbol` 在每个节点重复出现，冗余且易错
5. trigger 仅全局声明，难以支持节点级差异
6. 未表达 warmup / ready / publish_mode

### 6.2 推荐配置风格

推荐从“节点 + 边”形式逐步演进为“节点内嵌 inputs”形式。示例：

```yaml
trading_hours:
  start: "09:00:00"
  end: "23:00:00"

snapshot:
  type: local

plugins:
  - name: Replay
    library: "bin/libmod_replay.so"
    enabled: true
    config:
      data_file: "data/market_data_20260320_night"

  - name: FactorDAG
    library: "bin/libmod_factor_dag.so"
    enabled: true
    config:
      scope:
        symbols: ["rb2405"]

      trigger:
        default: on_tick
        timer_interval_sec: 1

      nodes:
        - id: LAST_PRICE
          op: last_price
          params: {}

        - id: SPREAD
          op: spread
          params: {}

        - id: IMBALANCE
          op: imbalance_l1
          params: {}

        - id: SMA_20
          op: sma
          inputs: [LAST_PRICE]
          params:
            window: 20
          warmup: 20

        - id: VOL_20
          op: volatility
          inputs: [LAST_PRICE]
          params:
            window: 20
          warmup: 20

        - id: COMBO
          op: weighted_sum
          inputs: [SMA_20, SPREAD, IMBALANCE, VOL_20]
          params:
            weights: [0.25, 0.25, 0.25, 0.25]

      outputs:
        - node: SMA_20
          topic: "factor.SMA_20"
          publish_mode: on_ready
        - node: SPREAD
          topic: "factor.SPREAD"
          publish_mode: always
        - node: IMBALANCE
          topic: "factor.IMBALANCE"
          publish_mode: always
        - node: VOL_20
          topic: "factor.VOL_20"
          publish_mode: on_ready
        - node: COMBO
          topic: "factor.COMBO"
          publish_mode: on_ready
```

### 6.3 若保留 `edges` 风格，必须补充的规则

若短期仍使用 `nodes + edges` 风格，则必须额外定义：

1. 节点 ID 唯一
2. 边的两端节点必须存在
3. 图中不得有环
4. 多输入节点必须定义 slot 或固定输入顺序
5. 输出节点必须存在
6. 节点输入数量必须与算子要求一致

例如：

```yaml
edges:
  - from: SMA_20
    to: COMBO
    slot: 0
  - from: SPREAD
    to: COMBO
    slot: 1
  - from: IMBALANCE
    to: COMBO
    slot: 2
  - from: VOL_20
    to: COMBO
    slot: 3
```

---

## 7. 数据模型设计

### 7.1 NodeMeta

`NodeMeta` 用于描述图节点的静态信息：

- node_id
- op_id 或 library handle
- 输入数量
- 输入索引起始位置
- 状态内存偏移
- 输出值偏移
- trigger mask
- warmup 配置
- flags

示意：

```cpp
struct NodeMeta {
    uint32_t node_id;
    uint16_t op_id;
    uint16_t input_count;
    uint32_t first_input_idx;
    uint32_t state_offset;
    uint32_t value_offset;
    uint32_t warmup;
    uint32_t flags;
};
```

### 7.2 Runtime State

每个节点在运行时具有以下状态：

- 当前值 `current_value`
- 当前版本 `version`
- 是否就绪 `ready`
- 是否脏 `dirty`
- 累计样本数 `sample_count`

### 7.3 State Arena

运行时状态内存建议由统一 arena 分配，避免节点各自 `new` 堆对象，减少内存碎片与 cache miss。

示意：

```cpp
struct DagRuntime {
    NodeMeta* nodes;
    uint32_t node_count;

    Value* values;
    uint64_t* versions;
    uint8_t* ready_flags;
    uint8_t* dirty_flags;

    uint8_t* state_arena;
};
```

### 7.4 TriggerPlan

每种 trigger 预编译为一份执行计划。典型包括：

- on_tick
- on_trade
- on_timer
- on_kline
- on_snapshot_ready

示意：

```cpp
struct TriggerPlan {
    TriggerType trigger;
    uint32_t* exec_nodes;
    uint32_t exec_count;
    uint32_t* publish_nodes;
    uint32_t publish_count;
};
```

---

## 8. 生命周期设计

### 8.1 启动阶段

`FactorDAG` 插件初始化流程如下：

1. 读取配置文件
2. 构建节点定义表
3. 建立依赖关系
4. 检测环
5. 拓扑排序
6. 校验参数与输入数量
7. 加载算子实现或节点插件
8. 计算状态内存布局
9. 初始化节点状态
10. 编译 trigger plan
11. 向 `hft_eb` 注册 handler

### 8.2 运行阶段

以 `on_tick` 为例，执行过程如下：

1. 回放源或行情源向 bus 发布 tick
2. `FactorDAG` 的 `on_tick` handler 被调用
3. Runtime 更新 source 节点值
4. 按 `on_tick` 的拓扑执行序列更新节点
5. 节点 ready 且满足 publish 条件时，向 bus 发布对应 topic
6. 策略插件或其他模块消费输出

### 8.3 停止阶段

系统停止时：

1. 取消事件总线订阅
2. 释放状态 arena
3. 卸载插件或释放算子资源

---

## 9. 图构建与校验设计

### 9.1 建图

从配置中读取：

- 节点列表
- 输入依赖或边列表
- 输出定义

构建：

- `node_name -> index`
- `inputs[index]`
- `outputs[index]`
- `indegree[index]`

### 9.2 环检测

采用拓扑排序检测环。若排序结果节点数小于总节点数，则图中存在环，应拒绝启动。

### 9.3 输入数量校验

每个算子在注册时声明其输入数量约束：

- 固定 0 输入，如 field / last_price
- 固定 1 输入，如 sma / ema / volatility
- 固定 2 输入，如 sub / div
- 可变输入，如 weighted_sum

启动阶段必须校验输入数匹配。

### 9.4 参数校验

每个算子应声明参数要求。示例：

- `sma.window > 0`
- `volatility.window > 1`
- `weighted_sum.weights.size == inputs.size`

### 9.5 输出校验

输出中声明的节点必须已存在且可产出值。

---

## 10. 运行时执行设计

### 10.1 执行模式

主执行模式采用预编译后的平铺执行链：

- 不在热路径中递归 DFS
- 不在热路径中频繁查 map
- 尽量使用整型 node index 访问

### 10.2 Source Node 更新

source 节点用于接收来自事件总线的原始输入，例如：

- last_price
- bid1
- ask1
- bidvol1
- askvol1

这些节点在 handler 入口被更新。

### 10.3 派生节点更新

节点按拓扑序执行，执行时读取输入节点值，调用对应算子更新当前节点值与内部状态。

### 10.4 Warmup 与 Ready

对于 SMA、VOL 等窗口型算子，在样本数不足时节点未 ready。

规则：

- 未 ready 节点不进入下游有效输出
- 下游节点的 ready 状态依赖其全部必要输入是否 ready
- `publish_mode = on_ready` 时，仅在 ready 后发布

### 10.5 Publish 策略

建议支持以下模式：

- `always`：每次触发都发布
- `on_ready`：仅节点 ready 后发布
- `on_change`：值发生变化时发布

### 10.6 Version / Dirty 机制

节点可维护 `version` 用于一致性管理，必要时维护 `dirty` 标记用于 lazy 节点延迟物化。

在第一阶段实现中，可优先使用版本号与 ready 状态，dirty 可作为后续扩展。

---

## 11. 与 `hft_eb` 的集成方式

### 11.1 事件总线注册

`FactorDAG` 插件向 bus 注册少量总入口：

- `on_tick`
- `on_timer`
- `on_kline`

而不是为每个节点都单独在 bus 层注册 handler。

这样可将图调度逻辑封装在 DAG Runtime 内部，避免 bus 层被 DAG 污染。

### 11.2 输出 Topic

DAG 节点输出通过 bus topic 发布，例如：

- `factor.SMA_20`
- `factor.SPREAD`
- `factor.IMBALANCE`
- `factor.COMBO`

策略层继续沿用原有订阅模式：

- 策略插件不感知 DAG 内部细节
- DAG 可独立演进
- 系统整体架构保持统一

### 11.3 多模块协同

以下模块可以自然接入：

- SignalCsv：订阅 factor topic 落盘
- 策略模块：订阅因子 topic 生成信号
- 调试模块：订阅特定因子监控实时值

---

## 12. 性能设计要点

### 12.1 避免热路径递归求值

事件驱动高频架构中，不适合将每次因子请求都变成递归拉取。推荐在触发时直接按执行链更新。

### 12.2 避免大量分散对象与虚函数

低延迟系统中，以下设计应尽量避免：

- 每节点一个堆对象
- 每次执行虚函数多态跳转
- 每步执行频繁访问哈希表

应尽量使用：

- 连续数组
- 整型索引
- 预编译执行顺序
- 统一状态 arena

### 12.3 公共子图复用

共享中间节点必须统一构建、统一维护状态。例如多个因子依赖 `LAST_PRICE -> SMA_20` 时，应只维护一份 `SMA_20` 状态。

### 12.4 按触发域分组

后续扩展时，应将图按 trigger domain 分组：

- tick 域
- trade 域
- timer 域
- kline/bar 域
- snapshot barrier 域

避免单一大图在所有事件上全量扫描。

### 12.5 多线程建议

若 `hft_eb` 采用按 symbol / shard owning 的线程模型，则 DAG Runtime 也应保持一致：

- 每个 shard 独占本 shard 节点状态
- 跨线程仅传事件，不共享写状态
- 尽量避免在节点级别加锁

---

## 13. 算子接口建议

### 13.1 短期兼容方案：节点级插件

短期内可兼容当前设计，通过 `library` 加载节点插件。

插件需提供统一接口，例如：

```cpp
struct FactorNodeApi {
    bool (*init)(void* state, const FactorInitCtx* ctx);
    bool (*update)(void* state, const FactorExecCtx* ctx, Value* out);
    size_t (*state_size)();
    uint32_t (*required_inputs)();
};
```

### 13.2 长期演进方案：算子注册接口

长期建议改为算子注册：

```cpp
struct OpVTable {
    bool (*init)(void* state, const OpInitCtx* ctx);
    bool (*update)(void* state, const OpExecCtx* ctx, Value* out);
    size_t state_size;
    uint32_t min_inputs;
    uint32_t max_inputs;
    TriggerMask trigger_mask;
};
```

由 `OperatorRegistry` 根据 `op` 返回实现。

---

## 14. 当前示例配置对应的图说明

给定示例中，图关系如下：

```text
LAST_PRICE -> SMA_20 -> \
LAST_PRICE -> VOL_20 ->  \
SPREAD ------------------> COMBO
IMBALANCE --------------> /
```

这是一个合法 DAG，可在启动时拓扑排序为：

1. LAST_PRICE
2. SPREAD
3. IMBALANCE
4. SMA_20
5. VOL_20
6. COMBO

其中：

- `LAST_PRICE` 为 source 型节点
- `SPREAD`、`IMBALANCE` 当前配置中被视为 source-derived 节点
- `SMA_20`、`VOL_20` 为 stateful 节点
- `COMBO` 为组合节点

该图可作为第一阶段验证原型，但需在后续版本中进一步规范：

- 补全输入关系表达
- 明确节点输入顺序
- 增加 warmup / ready 语义
- 将 `library` 风格演进为 `op` 风格

---

## 15. 实施建议

### 15.1 第一阶段：原型落地

目标：先打通“配置 -> 建图 -> 运行 -> 输出”。

建议：

1. 保留 `FactorDAG` 插件
2. 暂时兼容节点级 `library`
3. 启动时完成：建图、环检测、拓扑排序、参数校验
4. 运行时只支持 `on_tick`
5. 支持 warmup / ready
6. 输出发布到 topic

### 15.2 第二阶段：配置规范化

目标：提高可维护性。

建议：

1. 从 `edges` 风格逐步迁移到 `inputs` 风格
2. 引入 `publish_mode`
3. 引入节点级 trigger
4. 引入 scope / symbols
5. 明确 source 节点与派生节点的配置方式

### 15.3 第三阶段：算子化与性能优化

目标：形成可长期扩展的执行框架。

建议：

1. 引入 `OperatorRegistry`
2. 从“每节点一个 so”迁移到“模块级算子插件”
3. 推进状态 arena 连续化
4. 引入按 trigger domain 的分计划执行
5. 支持截面 barrier 节点

---

## 16. 风险与注意事项

### 16.1 每节点一个 so 的风险

- so 数量膨胀
- 配置复杂度升高
- ABI 演进困难
- 公共子图复用能力差

### 16.2 图表达不完整的风险

若部分节点私自从 bus 取数据、而非通过图依赖表达，则会造成：

- 图与真实依赖不一致
- 难以推理执行顺序
- 难以分析性能热点
- 难以复用公共中间节点

### 16.3 多输入节点顺序不确定的风险

如 `COMBO` 未显式定义输入顺序，会造成权重错配，进而导致结果错误。

### 16.4 warmup 未定义的风险

窗口类节点在未 ready 时若参与输出，会造成启动初期结果污染。

---

## 17. 结论

在现有 `hft_eb + 插件化 + 配置驱动 + pub/sub` 架构下，从配置文件构建因子 DAG 是可行且合理的。

最合适的定位是：

- DAG 不是平行于事件总线的新框架
- DAG 是 `hft_eb` 上的一层派生流编排与执行机制
- `FactorDAG` 插件负责将配置描述的依赖图编译为可执行的触发计划
- Runtime 在 bus 事件到来时增量更新节点，并将输出继续发布到 bus

当前示例配置已经具备原型价值，但若要演进为生产可用方案，建议：

1. 从“每节点一个 so”逐步迁移到“算子插件 + DAG 实例化”
2. 从“nodes + edges”逐步演进到“nodes + inputs”
3. 增加 warmup、ready、publish_mode、scope、节点级 trigger 等语义
4. 用拓扑排序与预编译执行计划代替热路径中的动态依赖遍历
5. 使用统一状态管理与连续内存布局提升低延迟性能

综上，该方案与 `hft_eb` 当前架构高度兼容，适合作为后续因子编排与流式信号生成的统一基础设施。

---

## 18. 后续可扩展方向

1. 支持多标的实例化
2. 支持截面节点与 snapshot barrier
3. 支持图版本热更新
4. 支持图级 profiling
5. 支持回测与实盘共用同一 DAG Runtime
6. 支持策略直接订阅 feature bundle 而非单因子 topic

