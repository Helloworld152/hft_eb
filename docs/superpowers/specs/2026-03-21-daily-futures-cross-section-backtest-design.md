# 日频期货截面回测（设计稿）

日期：2026-03-21

## 摘要
构建一套日频期货截面回测流水线，复用现有 `EVENT_KLINE` 事件类型，支持主力合约滚动切换，仅运行于回测模式。设计采用四插件拆分（数据源 / 因子 / 截面决策 / 回测执行），以最大化模块复用与策略对比能力。

## 范围
### 包含
- `.pq` 日频期货数据。
- 主力合约滚动切换的标的池。
- 日度调仓。
- 固定手续费率 + 固定滑点模型。
- 事件驱动链路，复用 EventBus 与 `EVENT_KLINE`。
- 明确的回测模式门禁配置。
- 明确的交易日时区与日线收盘边界定义。

### 不包含
- 实盘或实时行情接入。
- 日内/高频信号。
- 高阶成本模型（冲击成本、可变滑点等）。

## 目标
- 复用现有事件系统与模块生命周期。
- 模块职责清晰，方便替换因子与决策逻辑。
- 对现有代码侵入最小。

## 非目标
- 过度优化性能（超出日频回测需求）。
- 构建通用可视化回测平台或 Web 看板。

## 架构概览
日频事件流：

1. `.pq` 数据源发布 `EVENT_KLINE`。
2. 因子插件订阅 `EVENT_KLINE` 并发布 `EVENT_SIGNAL`。
3. 截面决策插件消费信号并生成日度调仓目标。
4. 回测插件撮合、计费、更新组合并输出指标。

屏障语义：
- 截面计算在“当日全部标的的 Kline 数据到齐”后才触发。
- 数据源在处理完当天最后一个标的后发出日终屏障标记。

## 插件拆分
### 1) `mod_pq_kline_source`
职责：
- 读取 `.pq` 日频数据。
- 生成 `KlineRecord` 并发布 `EVENT_KLINE`。

关键配置：
- `data_path`（目录或文件列表）
- `date_start`, `date_end`
- `symbol_map` 或 `symbols_file`
- `session_timezone`（例如 `Asia/Shanghai`）
- `bar_end`（例如 `exchange_settlement`）

确定性与校验：
- 事件发布顺序固定：按 `date` → `symbol` 排序。
- 重复条目：保留第一条并记录告警。
- 缺失日期：按配置 `on_missing` 选择 `skip` 或 `error`。
- OHLCV 校验：非正价格或 NaN，按 `on_invalid` 选择 `drop` 或 `error`。

日终屏障：
- 在当日最后一个标的处理完后发出日终屏障标记。

### 2) `mod_factor_daily`
职责：
- 订阅 `EVENT_KLINE`。
- 计算日频因子。
- 发布 `EVENT_SIGNAL`。

关键配置：
- `factors`（因子列表与参数）
- `signal_namespace`（信号命名空间）

避免前视偏差：
- 因子用日期 T 的收盘数据计算。
- 交易默认在 T+1 开盘执行（可配置）。

### 3) `mod_xs_decision`
职责：
- 订阅 `EVENT_SIGNAL`。
- 做截面排序/分桶/筛选。
- 生成调仓目标订单。

输出格式：
- 使用 `OrderReq` 表达调仓意图（减少新增类型）。

关键配置：
- `top_n` / `bottom_n`
- `long_short` 或 `long_only`
- `weighting`（等权 / 按分数加权）
- `universe_roll` 规则
- `max_gross`, `max_net`, `max_per_symbol`
- `tie_break`（默认 `lexicographic`）
- `on_missing_signal`（`exclude` / `neutral`）

主力切换语义：
- 通过 roll 日历定义每个日期的有效主力合约。
- 默认：日期 D 产生信号，日期 D+1 执行交易（可配置）。

### 4) `mod_backtest_daily`
职责：
- 消费调仓意图订单。
- 撮合成交，计费与滑点。
- 更新持仓、现金、PnL。
- 输出净值与绩效指标。

关键配置：
- `initial_cash`
- `fee_rate`
- `slippage_ticks`
- `price_basis`（`open` / `close`）
- `roll_rule`
- `tick_size` 与 `contract_multiplier`（每个合约元数据）

成交价格定义：
- `open` 指下一交易日开盘价。
- `close` 指当日收盘价（非结算价，除非明确指定）。

## 数据接口
- 输入：`.pq` 日频数据 → `KlineRecord`。
- 信号：`SignalRecord` 通过 `EVENT_SIGNAL`。
- 调仓：`OrderReq` 作为调仓意图。

`OrderReq` 作为调仓意图的约定：
- 必填字段：`symbol`, `direction`, `volume`。
- 如需表达目标权重，可使用明确约定的字段编码，并在插件 README 中说明。

## 成本模型（MVP）
- 固定手续费率：`fee = |qty| * price * contract_multiplier * fee_rate`。
- 固定滑点：`price_adj = price +/- slippage_ticks * tick_size`。

## 调仓逻辑（日度）
- 每日全标的数据到齐后进行一次截面选择。
- 组合估值在日期 T 收盘计算。
- 调仓默认在 T+1 开盘执行（可配置）。
- MVP 假设满额成交。

## 指标（MVP）
- 净值曲线
- 日收益
- 最大回撤
- 年化收益
- 换手率
- 多头收益 / 空头收益
- 总敞口 / 净敞口

## 风险与缓解
- **Kline 时间戳与信号时序错位**：固定为“收盘计算、次日执行”。
- **主力切换日边界问题**：使用明确 roll 日历；缺失数据时跳过并记录。
- **`OrderReq` 语义混淆**：在模块 README 与配置中明确“仅回测调仓意图”。
- **换月当日缺数据**：约定先平旧合约，次日开新合约，缺失则跳过。

## 未决问题
- `.pq` 数据具体 schema（字段名、symbol 格式）。
- 主力切换映射格式与维护方式。
- `price_basis: close` 是否使用结算价。
- 是否需要输出逐笔交易日志。

## 下一步
- 明确 `.pq` schema 与 roll 映射格式。
- 定义每插件的配置模板。
- 在小样本上做回测验证。
- 添加验收标准：
  - 固定样本的 NAV 回归测试。
  - roll 逻辑单测。
  - 成本模型单测。

## 配置门禁
- 顶层配置要求 `mode: backtest_daily`，避免误触发实盘链路。
- 示例配置中明确关闭所有实盘插件。
