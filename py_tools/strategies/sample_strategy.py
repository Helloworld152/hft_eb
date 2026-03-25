class SampleStrategy:
    def __init__(self, config, send_order):
        self.init(config, send_order)

    def init(self, config, send_order):
        self.send_order = send_order
        self.symbol = config.get("symbol", "au2606")
        self.buy_thresh = float(config.get("buy_thresh", "1048"))
        self.sell_thresh = float(config.get("sell_thresh", "1050"))
        self.last_side = None

    def on_tick(self, tick):
        if tick.get("symbol") != self.symbol:
            return

        price = float(tick.get("last_price", 0.0))
        if price <= 0:
            return

        if price < self.buy_thresh and self.last_side != "B":
            self.send_order(self.symbol, "B", "O", price, 1)
            self.last_side = "B"
        elif price > self.sell_thresh and self.last_side != "S":
            self.send_order(self.symbol, "S", "O", price, 1)
            self.last_side = "S"
