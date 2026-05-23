# SimTrade 保守版 Tick 模拟撮合方案

本文档记录 `SimTrade` 在只有 tick 快照、没有逐笔委托和逐笔成交数据时，优先采用的简单保守撮合方案。

目标不是还原真实交易所撮合，而是先构造一个**不容易高估成交、不容易欺骗回测结果、工程上容易落地**的模拟撮合模型。

---

## 1. 背景

当前 `SimTrade` 主要依赖 `TickMatchingEngine`，在行情 tick 到来时，用当前盘口价格判断工作订单是否可以成交。

在只有 tick 快照的情况下，我们缺少以下关键信息：

- tick 区间内逐笔成交顺序；
- 某一价位真实主动买 / 主动卖成交量；
- 订单前方真实排队量；
- 盘口减少是成交、撤单，还是二者混合；
- 策略订单进入市场后的真实延迟；
- 策略订单是否会改变历史盘口。

因此，简单版本不做复杂 queue model，不根据 `last_price == order_price` 直接判定成交，而是采用更保守的盘口穿价规则。

---

## 2. 设计目标

第一阶段目标：

```text
1. 避免明显的未来函数。
2. 避免同一 tick 盘口量被重复消费。
3. 避免 last_price 触价即成交导致的乐观偏差。
4. 支持主动单吃盘口。
5. 被动单只在盘口穿过订单价格时成交。
6. 保留现有 SimTrade 的订单回报、成交回报、账户更新链路。
```

不追求：

```text
1. 完整重建 LOB。
2. 模拟真实排队位置。
3. 区分撤单和成交导致的盘口变化。
4. 高精度估计主动买 / 主动卖流。
5. 让策略订单改变历史盘口。
```

---

## 3. 核心撮合规则

一句话版本：

```text
订单至少延迟一个 tick 生效；
买单只在卖一价 <= 买价时成交；
卖单只在买一价 >= 卖价时成交；
同一个 tick 的盘口量成交后要扣减；
不处理 last_price 触价成交。
```

### 3.1 买单规则

对于买单：

```text
if ask1 > 0 and ask_vol1 > 0 and order.price >= ask1:
    fill_price = ask1
    fill_qty = min(order.remaining, ask_vol1_remaining)
else:
    no fill
```

含义：

- 市价买单吃 `ask1`；
- 限价买单只有在 `limit_price >= ask1` 时成交；
- 如果只是 `last_price == limit_price`，但 `ask1 > limit_price`，不成交；
- 这会低估部分排队成交，但能避免触价即成交的乐观偏差。

示例：

```text
bid1 = 100
ask1 = 101
last = 100

挂 buy 100：不成交

后续 tick:
bid1 = 99
ask1 = 100

buy 100：成交
```

### 3.2 卖单规则

对于卖单：

```text
if bid1 > 0 and bid_vol1 > 0 and order.price <= bid1:
    fill_price = bid1
    fill_qty = min(order.remaining, bid_vol1_remaining)
else:
    no fill
```

含义：

- 市价卖单吃 `bid1`；
- 限价卖单只有在 `limit_price <= bid1` 时成交；
- 如果只是 `last_price == limit_price`，但 `bid1 < limit_price`，不成交。

---

## 4. 事件时序

为了避免当前 tick 产生信号、当前 tick 立即成交的未来函数，建议采用如下时序：

```text
on_market_data(tick[i]):
    1. 激活上一 tick 之后收到的 pending orders。
    2. 用 tick[i] 撮合已经 working 的订单。
    3. 发布成交回报、订单回报和账户更新。
    4. 更新 latest_tick。
    5. 策略基于 tick[i] 产生新订单。

on_order_send(req):
    1. 分配模拟系统单号。
    2. 记录订单。
    3. 发布“已报”回报。
    4. 放入 pending_orders。
    5. 不允许立刻用当前 tick 撮合。
```

最简化版本可以直接规定：

```text
订单在 tick[i] 之后发出，最早只能在 tick[i+1] 撮合。
```

---

## 5. 同一 tick 盘口量必须扣减

当前 tick 的盘口量不能被多个模拟订单重复消费。

错误行为：

```text
ask1 = 100.2
ask_vol1 = 10

多个买单都看到 ask_vol1 = 10，分别成交 10 手。
最终模拟成交量 > 真实盘口量。
```

正确做法：

```cpp
int bid_remain[5];
int ask_remain[5];

for (int i = 0; i < 5; ++i) {
    bid_remain[i] = tick.bid_volume[i];
    ask_remain[i] = tick.ask_volume[i];
}

// 买单成交后扣 ask_remain[level]
// 卖单成交后扣 bid_remain[level]
```

成交示意：

```cpp
int qty = std::min(order.remaining, ask_remain[level]);
ask_remain[level] -= qty;
```

---

## 6. 最小代码改造建议

### 6.1 SimTradeModule 增加 pending / working 订单

可以先用简单容器：

```cpp
std::vector<uint64_t> pending_order_ids_;
std::vector<uint64_t> working_order_ids_;
```

`onOrder()` 中：

```cpp
tracked_orders_[sys_id] = tracked;
client_to_sys_[req->client_id] = sys_id;
pending_order_ids_.push_back(sys_id);

publish_order_rtn(tracked_orders_[sys_id], '3'); // 已报
```

不要在 `onOrder()` 里立即调用撮合逻辑。

### 6.2 onTick 中先激活 pending，再撮合 working

```cpp
void onTick(TickRecord* tick) {
    if (!tick || tick->symbol[0] == '\0') return;

    // 1. 上一轮 pending 进入 working
    for (auto id : pending_order_ids_) {
        working_order_ids_.push_back(id);
    }
    pending_order_ids_.clear();

    // 2. 为当前 tick 创建可消耗盘口副本
    int bid_remain[5];
    int ask_remain[5];
    for (int i = 0; i < 5; ++i) {
        bid_remain[i] = tick->bid_volume[i];
        ask_remain[i] = tick->ask_volume[i];
    }

    // 3. 遍历 working orders，按保守规则撮合
    // 4. 成交后调用 process_trade(...)
    // 5. 未完成订单继续保留在 working_order_ids_
}
```

注意：如果严格要求“tick[i] 后发出的订单只能 tick[i+1] 成交”，需要保证策略发单发生在 `onTick` 撮合之后，或者在事件框架层明确事件顺序。

### 6.3 简单成交函数

```cpp
bool try_simple_fill(
    const TrackedOrder& o,
    const TickRecord& tick,
    int* bid_remain,
    int* ask_remain,
    int& fill_qty,
    double& fill_price
) {
    fill_qty = 0;
    fill_price = 0.0;

    int remain = o.volume_total - o.volume_traded;
    if (remain <= 0) return false;

    if (o.direction == 'B') {
        for (int i = 0; i < 5; ++i) {
            double ask_px = tick.ask_price[i];
            int& ask_qty = ask_remain[i];
            if (ask_px <= 0.0 || ask_qty <= 0) continue;

            if (o.is_market || o.limit_price >= ask_px) {
                fill_qty = std::min(remain, ask_qty);
                fill_price = ask_px;
                ask_qty -= fill_qty;
                return fill_qty > 0;
            }
            break;
        }
        return false;
    }

    if (o.direction == 'S') {
        for (int i = 0; i < 5; ++i) {
            double bid_px = tick.bid_price[i];
            int& bid_qty = bid_remain[i];
            if (bid_px <= 0.0 || bid_qty <= 0) continue;

            if (o.is_market || o.limit_price <= bid_px) {
                fill_qty = std::min(remain, bid_qty);
                fill_price = bid_px;
                bid_qty -= fill_qty;
                return fill_qty > 0;
            }
            break;
        }
        return false;
    }

    return false;
}
```

---

## 7. 撤单处理

简单版本中，撤单只需要从 `pending_order_ids_` 或 `working_order_ids_` 中移除对应订单，并发布已撤回报。

建议语义：

```text
pending order:
    撤单成功，不会参与未来撮合。

working order:
    撤单成功，从 working 队列移除。
```

暂时不模拟：

```text
1. 撤单延迟；
2. 撤单失败；
3. 撤单过程中部分成交；
4. exchange cancel reject。
```

这些可以留到第二阶段。

---

## 8. 这个模型的优缺点

优点：

```text
1. 实现简单。
2. 不需要逐笔数据。
3. 不会触价即成交。
4. 不会重复消费盘口量。
5. 对挂单策略偏保守。
6. 回测收益不容易被虚假成交放大。
```

缺点：

```text
1. 会低估真实排队成交概率。
2. 不区分主动成交和撤单。
3. 不估计 queue position。
4. 对 inside spread 报价处理保守。
5. 不能模拟自身订单对盘口的影响。
```

---

## 9. 后续增强方向

保守版跑通之后，可以逐步增加：

```text
1. fill_mode: conservative / neutral / optimistic。
2. queue_ahead：记录订单前方排队量。
3. prev_tick -> curr_tick 的 delta_volume / delta_turnover 流量估计。
4. 触价时通过 queue model 判断是否成交。
5. latency_us / latency_ticks。
6. 撤单延迟和撤单失败。
7. 手续费、滑点、保证金和资金冻结。
8. PositionLot 成本队列，支持部分开平仓。
```

建议不要一开始就做复杂模型。先保证保守版稳定，再通过实盘成交回报或更细粒度数据逐步校准。

---

## 10. 判断标准

如果策略只在以下条件下赚钱：

```text
零延迟、触价成交、盘口量可重复消费
```

则回测结果基本不可信。

如果策略在保守撮合下仍然有稳定收益，后续再加入 queue model 和中性撮合模型后仍然表现稳定，可信度会明显更高。

因此，第一阶段的 SimTrade 应优先成为一个“不会骗自己”的保守回测执行器，而不是一个看起来复杂但参数不可控的伪真实撮合器。
