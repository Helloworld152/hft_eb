---
name: hft_eb_dev
description: 项目级 skill：用于在 hft_eb 仓库中进行导航、开发、验证与排障
---

# hft_eb 开发 Skill

当用户要在 `hft_eb` 仓库里做开发、排查问题、补文档、改模块、加配置或梳理架构时，使用这个 skill。

这个 skill 的目标不是讲通用 HFT 原理，而是让 Codex 先快速对齐本仓库的真实结构、开发入口、验证方式和风险边界，再做最小修改。

## 使用边界

- 只针对当前仓库 `hft_eb`
- 优先做最小修改，不主动扩散到无关模块
- 先读现有实现和设计文档，再改代码
- 若改动将跨越多个模块或涉及架构设计，先整理计划再执行

## 首次进入仓库时先做什么

1. 先读 [README.md](../../README.md)，确认系统定位、主目录和插件分层。
2. 再读 [references/project_map.md](references/project_map.md)，建立源码目录和文档索引。
3. 根据任务类型加载对应引用：
   - 改业务模块或配置：读 [references/workflows.md](references/workflows.md)
   - 需要找设计依据：按 [references/project_map.md](references/project_map.md) 中列出的文档继续深读
   - 构建失败、运行异常、链路不通：读 [references/troubleshooting.md](references/troubleshooting.md)

## 仓库工作准则

- 把 `src/`、`include/`、`core/` 视为引擎主干；把 `modules/` 视为插件实现；把 `conf/` 视为运行入口配置。
- 先确认修改属于哪一层，再定位最小落点，不要同时改引擎、插件、配置，除非任务明确要求。
- 插件能力和数据流优先以现有文档为准，不凭文件名猜行为。
- 新增或修改配置时，优先复用现有 `conf/*.yaml` 模式，不另造结构。
- 修改前先看调用点和配置样例；修改后至少做一次和任务匹配的最小验证。

## 默认开发流程

1. 用 `rg` 定位目标类型、事件名、模块名、配置名。
2. 读取目标文件和相邻实现，确认事件流、配置注入方式和已有约束。
3. 如涉及插件行为，再读对应设计文档或模块总览。
4. 先决定验证方式，再做原子修改：
   - 纯构建类改动：至少保证相关目标能编译
   - 单模块逻辑改动：优先跑最小化构建或对应测试
   - 配置/脚本改动：校验路径、文件名、启动命令和依赖关系
5. 汇报时说明影响范围、验证方式和未覆盖风险。

更细的构建、验证和排障入口见 [references/workflows.md](references/workflows.md) 与 [references/troubleshooting.md](references/troubleshooting.md)。

## 仓库特有注意事项

- `run.sh` 会先 `pkill hft_engine`，再启动 `conf/config_real_test.yaml`；除非用户明确要求，否则不要直接执行。
- `build_release.sh` 会清理部分环境变量并在顶层 `build/` 里做 Release 构建，适合整仓构建，不适合轻量探测。
- 顶层 `CMakeLists.txt` 会通过 `FetchContent` 拉取 `ccapi`；网络受限或离线环境下，整仓构建可能失败。
- 当前仓库常有未提交改动；新增改动时避免顺手整理无关文件。

## 什么时候扩展阅读

- 涉及事件总线、引擎生命周期、插件装载：优先读 `README.md`、`src/engine.cpp`、`include/engine.h`
- 涉及插件分工：优先读 [docs/modules_overview.md](../../docs/modules_overview.md)
- 涉及策略树、因子 DAG、并行模型：优先读 `架构.md` 及 `docs/` 下对应设计文档
- 涉及性能或编译选项：优先读 [docs/编译优化指南_compile_optimization.md](../../docs/编译优化指南_compile_optimization.md)

## 最小使用方式

在后续对话里可以直接说：

- “使用 `hft_eb_dev` skill，帮我定位某个模块的入口”
- “使用 `hft_eb_dev` skill，给 `risk` 模块加一个最小修复”
- “使用 `hft_eb_dev` skill，解释某个配置文件怎么走到插件里”

如果任务同时涉及通用 HFT 性能优化，再叠加使用现有的 `hft_trade_system` skill。
