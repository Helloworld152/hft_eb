# 扫单交易插件设计 (SweepTrader Module)

## 1. 背景与目标
本插件旨在提供一个非侵入式的接口，通过扫描文件系统目录实现外部订单的自动导入、动态定价及执行（包括立即执行和 TWAP 算法）。

## 2. 核心逻辑
1. **目录监控**：插件定期扫描 `order_dir` 目录下的 `.csv` 文件。
2. **文件解析**：提取标的、方向、参考价、总量、时间窗口及执行算法。
3. **有效性校验**：仅在指定的 `[start_time, end_time]` 时间窗口内执行。
4. **行情对齐 (High Performance)**：
    - 插件不再通过 `EventBus` 订阅和缓存行情。
    - **静态查询**：直接调用全局高性能接口 `Market::get_tick(symbol_id)` 获取当前市场快照。
    - **优势**：消除订阅延迟、节省内存副本、实现纳秒级定价响应。
5. **自动定价与参考价**：
    - **参考价 (price)**：从 CSV 中读取，仅作为日志记录及滑点偏差分析的基准。
    - **执行定价**：报单瞬时，通过静态接口获取最新 Tick，并根据全局策略（如“对手价”）自动计算报单价格。
6. **执行算法**：
    - **Direct**：进入时间窗口后，参照当前行情立即全量报单。
    - **TWAP**：在时间窗口内，按固定频率（`interval_sec`）分批发出报单，每批次实时定价。
7. **文件处理**：完成后将文件移入 `processed/` 目录。

## 3. 文件格式定义 (CSV)
内容示例：
```csv
symbol,direction,offset,price,volume,account_id,start_time,end_time,algo,interval_sec
au2606,B,O,450.5,100,888888,09:00:00,10:00:00,twap,60
rb2505,S,C,3500.0,2,888888,13:30:00,15:00:00,direct,0
```
字段说明：
- `symbol`: 合约代码。
- `direction`: 'B' (买入), 'S' (卖出)。
- `offset`: 'O' (开仓), 'C' (平仓), 'T' (平今)。
- `price`: **参考价**（仅作参考，不作为硬性的执行限价）。
- `volume`: 总报单数量。
- `account_id`: 交易账号。
- `start_time`: 执行起始时间 (HH:MM:SS)。
- `end_time`: 执行截止时间 (HH:MM:SS)。
- `algo`: `direct` (立即执行) 或 `twap` (时间加权)。
- `interval_sec`: TWAP 模式下的报单间隔秒数。

## 4. 插件配置 (`config.yaml`)
```yaml
- name: libmod_sweep_trader.so
  order_dir: "../data/orders"      # 监控目录
  default_price_strategy: "opp"      # 自动定价策略: "opp"(对手价), "last"(最新价)
  scan_interval_ms: 500             # 目录扫描频率
  default_account: "888888"        # 默认账号
```

## 5. 详细设计

### 5.1 自动定价逻辑
插件在报单瞬间调用静态接口 `Market::get_tick(symbol_id)`：
- **对手价 (opp)**：买入用 Ask1，卖出用 Bid1。确保快速扫掉对手盘。
- **最新价 (last)**：使用上次成交价，适合冲击较小的平稳市场。
- **异常处理**：若静态快照中的行情时间戳已失效或价格为 0，则该笔报单挂起并记录警告。

### 5.2 TWAP 状态管理
- 插件在内存中维护 `TwapTask`，记录每个文件的 `total_volume` 和 `executed_volume`。
- 任务在到达 `end_time` 或完成 `total_volume` 后结束。

### 5.3 目录监控策略
- 采用 `ITimerService` 轮询。
- 仅处理 `.csv` 后缀文件。
- 处理成功移至 `processed/`，失败（如格式错）移至 `error/`。

---
"Talk is cheap. Show me the code."