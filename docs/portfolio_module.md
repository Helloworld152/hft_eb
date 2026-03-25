# PortfolioModule 设计说明

版本: v1.0
日期: 2026-03-24
作者: Codex

## 1. 模块定位与边界

PortfolioModule 位于 Alpha 与 Execution 之间，负责把多策略信号与账户/持仓状态统一成“目标增量”，并直接输出 `EVENT_ORDER_REQ`。它不负责交易接入、回报一致性与成交确认。

职责:
- 汇总多策略信号，计算目标增量
- 结合账户可用资金、持仓限制与风控约束裁剪目标
- 将目标增量转化为订单请求

不负责:
- 报单通道/撮合/成交
- 订单回报一致性管理
- 盘口定价与执行优化

## 2. 输入与输出事件

输入:
- `EVENT_SIGNAL` (SignalRecord)
- `EVENT_POS_UPDATE` (PositionDetail)
- `EVENT_ACC_UPDATE` (AccountDetail)

输出:
- `EVENT_ORDER_REQ` (OrderReq)

相关结构体字段参考:
- `core/include/protocol.h` 中的 `SignalRecord`、`PositionDetail`
- `core/include/protocol.h` 中的 `AccountDetail`
- `core/include/protocol.h` 中的 `OrderReq`

## 3. 内部数据与状态

建议内部缓存:
- `signal_cache`: key = (source_id, symbol_id) 或 (source_id, symbol)
- `position_cache`: key = (account_id, symbol_id)
- `account_cache`: key = account_id

缓存策略:
- `signal_cache` 需要 TTL 过滤 (`signal_ttl_ms`)
- `position_cache` 与 `account_cache` 以最新回报为准

## 4. 信号聚合与目标增量

### 4.1 多策略聚合

设单策略信号为 `value_i`，权重为 `weight_i`:

```text
raw_i = value_i * weight_i
raw_sum = sum(raw_i)
```

### 4.2 阈值与缩放

```text
if abs(raw_sum) < min_signal_threshold:
    target_delta = 0
else:
    target_delta = round(raw_sum * signal_scale)
```

含义:
- `target_delta > 0` 表示增持
- `target_delta < 0` 表示减持
- 单位: 手

## 5. 约束裁剪与优先级

裁剪顺序固定:
1. 账户约束
2. 持仓约束
3. 风控约束

### 5.1 账户约束

- 依据 `account.available` 与 `margin` 计算最大可开仓手数
- 若无法计算(无行情/无合约乘数)，则只做最小裁剪(不超过 `max_order_size`)

### 5.2 持仓约束

```text
new_net = current_net + target_delta
if abs(new_net) > max_abs_pos:
    target_delta = clamp(target_delta, -max_abs_pos - current_net, max_abs_pos - current_net)
```

### 5.3 风控约束

- `max_order_size`: 单笔最大手数
- `max_notional`: 最大名义金额
- 若任一约束触发，则裁剪 `target_delta`

## 6. 订单生成规则

### 6.1 基本规则

```text
if target_delta > 0:
    direction = 'B'
    offset_flag = 'O'
elif target_delta < 0:
    direction = 'S'
    offset_flag = prefer_close_first ? 'C' : 'O'
else:
    no order
```

- `price = 0` (由 Execution/Trade 层定价)
- `volume = abs(target_delta)`
- `account_id` 默认使用 `default_account`，若信号中携带账户信息则优先采用

### 6.2 平仓优先策略

- `prefer_close_first = true` 时，卖出默认用平仓，若无可平仓位可降级为开仓
- 若需更细粒度平今/平昨，Execution 层可二次拆分

## 7. 配置项定义

建议配置字段:
- `default_account`: 默认账户
- `strategy_weights`: 策略权重表 (strategy_id -> weight)
- `signal_scale`: 信号缩放系数
- `min_signal_threshold`: 最小触发阈值
- `max_abs_pos`: 最大净仓
- `max_order_size`: 单笔最大手数
- `max_notional`: 最大名义金额
- `prefer_close_first`: 平仓优先
- `signal_ttl_ms`: 信号有效期

示例:
```yaml
strategy_weights:
  SMA: 1.0
  IMB: 0.7
signal_scale: 10
min_signal_threshold: 0.2
max_abs_pos: 50
max_order_size: 5
max_notional: 200000
prefer_close_first: true
signal_ttl_ms: 2000
```

## 8. 运行时序与事件流

```text
EVENT_SIGNAL
    -> PortfolioModule (聚合+裁剪)
        -> EVENT_ORDER_REQ
            -> RiskModule
                -> OrderManager
                    -> Trade/Gateway
                        -> EVENT_RTN_RAW_ORDER / EVENT_RTN_RAW_TRADE
                            -> OrderManager
                                -> EVENT_RTN_ORDER / EVENT_RTN_TRADE
                                    -> Position/Account 更新
```

## 9. 异常与边界处理

- 无持仓/无账户: 不下单或只允许最小订单
- 信号过期: 丢弃
- 资金不足: 裁剪或拒绝
- 目标增量为 0: 不发单

## 10. 测试用例清单

- 单一信号触发订单
- 多策略权重聚合正确
- 阈值过滤生效
- 持仓上限裁剪正确
- 账户资金不足裁剪正确
- 无账户/无持仓安全降级
- 平仓优先逻辑正确
