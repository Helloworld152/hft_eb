from hft_backtest import BacktestEngine, BaseStrategy

class MyStrategy(BaseStrategy):
    def __init__(self, config=None):
        super().__init__(config)
        self.bought = False
        self.order_count = 0
        self.trade_count = 0
        self.buy_price = 991.28
        self.sell_price = 991.38
        self.volume = 5

    def on_init(self):
        self.write_log(f"策略初始化, vt_symbol={self.vt_symbol}")

    def on_start(self):
        self.write_log("策略启动")

    def on_tick(self, tick):
        if tick.symbol != self.vt_symbol:
            return

        if not self.bought and tick.last_price <= 991.3:
            self.order_count += 1
            self.write_log(f"[下单#{self.order_count}] BUY @{self.buy_price} vol={self.volume}")
            self.buy(price=self.buy_price, volume=self.volume)
            self.bought = True

        elif self.bought and tick.last_price >= 991.35:
            self.order_count += 1
            self.write_log(f"[下单#{self.order_count}] SELL @{self.sell_price} vol={self.volume}    ")
            self.sell(price=self.sell_price, volume=self.volume)
            self.bought = False

    def on_order(self, order):
        status_map = {'0': '全部成交', '1': '部分成交', '3': '排队中', '5': '已撤单'}
        st = status_map.get(order.status, order.status)
        self.write_log(
            f"[回报] {order.order_sys_id} {order.direction}{order.offset} "
            f"@{order.price} 成交{order.volume_traded}/{order.volume_total} [{st}]"
        )

    def on_trade(self, trade):
        self.trade_count += 1
        self.write_log(
            f"[成交#{self.trade_count}] {trade.direction}{trade.offset} "
            f"@{trade.price} vol={trade.volume} "
            f"trade_id={trade.trade_id} sys_id={trade.order_sys_id}"
        )

    def on_stop(self):
        self.write_log(f"策略停止, 共下单{self.order_count}笔 成交{self.trade_count}笔")


engine = BacktestEngine()
engine.set_parameters(
    data_mode="tick",
    data_file="data/market_data_20260523_night",
    symbols_file="conf/symbols.txt",
    initial_balance=1_000_000,
    output_dir="/home/rying/hft_eb/hft_backtest/results/au2606_test",
)
engine.add_strategy(MyStrategy, {"symbol": "au2606"})
result = engine.run_backtesting()
print(result.statistics)
