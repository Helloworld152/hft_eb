from .objects import BarData, OrderData, TickData, TradeData


class BaseStrategy:
    """策略基类，所有策略必须继承"""

    def __init__(self, config=None):
        self.config = dict(config or {})
        self._send_order = None    # C++ 构造后注入
        self._cancel_order = None  # C++ 构造后注入
        self.strategy_name = self.config.get("strategy_name", self.__class__.__name__)
        self.vt_symbol = self.config.get("symbol", "")

    def on_init(self):
        pass

    def on_start(self):
        pass

    def on_tick(self, tick: TickData):
        pass

    def on_bar(self, bar: BarData):
        pass

    def on_order(self, order: OrderData):
        pass

    def on_trade(self, trade: TradeData):
        pass

    def on_stop(self):
        pass

    def handle_init(self):
        self.on_init()

    def handle_start(self):
        self.on_start()

    def handle_tick(self, tick):
        self.on_tick(TickData.from_dict(tick))

    def handle_bar(self, bar):
        self.on_bar(BarData.from_dict(bar))

    def handle_order(self, order):
        self.on_order(OrderData.from_dict(order))

    def handle_trade(self, trade):
        self.on_trade(TradeData.from_dict(trade))

    def handle_stop(self):
        self.on_stop()

    def write_log(self, msg):
        print(f"[{self.strategy_name}] {msg}")

    def send_order(self, symbol, direction, offset, price, volume, account_id=""):
        if self._send_order is None:
            raise RuntimeError("send_order callback is not bound")
        return self._send_order(
            symbol=symbol,
            direction=direction,
            offset=offset,
            price=float(price),
            volume=int(volume),
            account_id=account_id,
        )

    def cancel_order(self, client_id, symbol=None, account_id=""):
        if self._cancel_order is None:
            raise RuntimeError("cancel_order callback is not bound")
        self._cancel_order(
            client_id=int(client_id),
            symbol=symbol or self.vt_symbol,
            account_id=account_id,
        )

    def buy(self, price, volume, symbol=None, account_id=""):
        return self.send_order(symbol or self.vt_symbol, "B", "O", price, volume, account_id)

    def sell(self, price, volume, symbol=None, account_id=""):
        return self.send_order(symbol or self.vt_symbol, "S", "C", price, volume, account_id)

    def short(self, price, volume, symbol=None, account_id=""):
        return self.send_order(symbol or self.vt_symbol, "S", "O", price, volume, account_id)

    def cover(self, price, volume, symbol=None, account_id=""):
        return self.send_order(symbol or self.vt_symbol, "B", "C", price, volume, account_id)
