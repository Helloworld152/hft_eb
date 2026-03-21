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
3. 数据源发布 `EVENT_EOD`（含当日 Universe 快照）。
4. 截面决策插件在收到 `EVENT_EOD` 后消费当日信号并生成日度调仓目标。
5. 回测插件撮合、计费、更新组合并输出指标。

屏障语义：
- 截面计算在“当日有效 Universe 的 Kline 数据到齐”后才触发。
- 数据源在处理完当天最后一个标的后发出日终屏障标记。
- 当日有效 Universe 由 `universe_roll` 规则与过滤器共同决定。

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

Universe 与屏障触发：
- 数据源读取“权威 `universe_roll` 映射”来确定当日有效 Universe，并在 `EVENT_EOD` 中广播。
- 决策模块仅使用 `EVENT_EOD` 中的 Universe，避免配置不一致。
- 当日预期 Universe 未到齐时，记录缺失列表后仍触发日终屏障。

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
- `signal_contract_rule`（见“合约映射规则”）
- `trade_contract_rule`（见“合约映射规则”）
- `max_gross`, `max_net`, `max_per_symbol`
- `tie_break`（默认 `lexicographic`）
- `on_missing_signal`（`exclude` / `neutral`）

权重到手数映射（统一口径）：
- 目标名义规模：`target_notional = weight * nav * gross_factor`。
- 目标手数：`target_volume = round(target_notional / (exec_price * contract_multiplier))`。
- `round` 由 `volume_rounding` 控制，`exec_price` 使用“执行日 + price_basis”。 
- `OrderReq.volume = target_volume - current_volume`。

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
- `fee_model`（`ad_valorem` / `per_contract`）
- `fee_per_contract`（当 `fee_model=per_contract`）
- `slippage_ticks`
- `price_basis`（`open` / `close`）
- `trade_lag`（默认 1）
- `roll_rule`
- `tick_size` 与 `contract_multiplier`（每个合约元数据）
- `margin_rate` 或 `initial_margin`
- `max_leverage`
- `volume_rounding`（`floor` / `round` / `ceil`）
- `deleveraging_rule`（`scale_down_proportional` / `trim_smallest`）

成交价格定义：
- 执行日：`execution_day = signal_day + trade_lag`，默认 `trade_lag = 1`。
- `open` 指执行日开盘价。
- `close` 指执行日收盘价（非结算价，除非明确指定）。

## 数据接口
- 输入：`.pq` 日频数据 → `KlineRecord`。
- 信号：`SignalRecord` 通过 `EVENT_SIGNAL`。
- 日终屏障：`EVENT_EOD`（包含 `date` 与当日 Universe 列表）。
- 调仓：`OrderReq` 作为调仓意图。

`OrderReq` 作为调仓意图的约定：
- 必填字段：`symbol`, `direction`, `volume`。
- `volume` 表示“当日需要成交的手数（delta）”，不直接表达权重。
- `price` 在回测调仓中不参与定价（以回测引擎的 `price_basis` 为准）。

## 成本模型（MVP）
- 费率模型 `ad_valorem`：`fee = |qty| * price * contract_multiplier * fee_rate`
- 费率模型 `per_contract`：`fee = |qty| * fee_per_contract`
- 固定滑点：`price_adj = price +/- slippage_ticks * tick_size`。

## 调仓逻辑（日度）
- 每日全标的数据到齐后进行一次截面选择。
- 组合估值在日期 T 收盘计算。
- 调仓默认在 `T + trade_lag` 执行（可配置）。
- MVP 假设满额成交。

## 指标（MVP）
- 净值曲线
- 日收益
- 最大回撤
- 年化收益
- 换手率
- 多头收益 / 空头收益
- 总敞口 / 净敞口

口径说明（简版）：
- 年化收益：`(1 + total_return)^(252 / n_days) - 1`
- 最大回撤：净值曲线峰值到谷值的最大回撤比例
- 换手率：日内成交额 / 组合净值（或等价定义）

## 合约映射规则（信号与成交）
为避免 roll 跨日错配，提供显式配置：
- 规则 A：信号与成交均使用日期 D 的主力合约（成交在 D+1 开盘价）。
- 规则 B：信号使用日期 D 主力，成交使用日期 D+1 主力（含自动换合约）。
- 规则 C：信号与成交均使用日期 D+1 主力（需前移映射）。
默认建议：规则 B，并在配置中明确。

## 换月与已有持仓处理
- `auto_close_on_roll` 默认开启：在 roll 日对旧合约进行平仓，再按目标合约开仓。
- 平仓与开仓均计入手续费与滑点，PnL 在平仓时结算。
- 若信号合约与成交合约不一致，遵循 `trade_contract_rule`，旧合约先平后开新合约。

## 资金与保证金模型（MVP）
- 每合约保证金占用：`margin = |position| * price * contract_multiplier * margin_rate`。
- 可用资金：`available = cash - total_margin`。
- 下单前校验 `available` 与 `max_leverage`，不足则执行减仓规则。
- `scale_down_proportional`：按权重等比例缩放至满足约束。
- `trim_smallest`：从最小权重开始逐个剔除直至满足约束。

## `.pq` 数据 schema（最小示例）
字段建议：`date`, `symbol`, `open`, `high`, `low`, `close`, `volume`, `open_interest`, `settle`（可选）。
缺失字段策略：`settle` 缺失时禁止使用 `price_basis=settle`。

## 交易日历与非交易日处理
- 配置 `trading_calendar`（如 `SHFE`, `DCE`, `CFFEX`）。
- 非交易日跳过；连续缺失超过阈值记录告警。

## 因子 warmup 规则
- 增加 `warmup_days` 或 `min_history`。
- 未满足时信号行为由 `on_missing_signal` 统一控制。

## 合约元数据
- `instrument_meta` 输入文件：`symbol`, `tick_size`, `contract_multiplier`, `margin_rate` 等。
- 如元数据按日期变化，可支持带日期版本的映射表。

## 风险与缓解
- **Kline 时间戳与信号时序错位**：固定为“收盘计算、次日执行”。
- **主力切换日边界问题**：使用明确 roll 日历；缺失数据时跳过并记录。
- **`OrderReq` 语义混淆**：在模块 README 与配置中明确“仅回测调仓意图”。
- **换月当日缺数据**：约定先平旧合约，次日开新合约，缺失则跳过。
- **资金与保证金约束缺失**：强制启用 `margin_rate` 与 `max_leverage` 校验。

## 未决问题
- `.pq` 数据具体 schema（字段名、symbol 格式）。
- 主力切换映射格式与维护方式。
- `price_basis: close` 是否使用结算价。
- `instrument_meta` 的来源与格式细节。
- 是否需要输出逐笔交易日志。

## 下一步
- 明确 `.pq` schema 与 roll 映射格式。
- 定义每插件的配置模板。
- 在小样本上做回测验证。
- 验收标准：固定样本的 NAV 回归测试。
- 验收标准：roll 逻辑单测。
- 验收标准：成本模型单测。

## 配置门禁
- 顶层配置要求 `mode: backtest_daily`，避免误触发实盘链路。
- 示例配置中明确关闭所有实盘插件。
