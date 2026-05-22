from hft_backtest import CtaTemplate


class DemoStrategy(CtaTemplate):
    author = "codex"

    def on_init(self):
        self.write_log("strategy initialized")

    def on_start(self):
        self.write_log("strategy started")

    def on_tick(self, tick):
        if tick.symbol != self.vt_symbol:
            return
        if tick.last_price <= 0:
            return
        if tick.last_price > 100:
            self.buy(price=tick.last_price, volume=1)

    def on_order(self, order):
        self.write_log(f"order status={order.status} ref={order.order_ref}")

    def on_trade(self, trade):
        self.write_log(f"trade id={trade.trade_id} price={trade.price} volume={trade.volume}")

    def on_stop(self):
        self.write_log("strategy stopped")
