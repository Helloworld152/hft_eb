"""
日线多因子示例：动量（相对均线） + 成交量相对强度，线性组合得分后发单。
需配合 Kline_Parquet_Replay + PyStrategy(on_kline)，见 conf/config_py_stock_mf_backtest.yaml。
"""
import math
from collections import defaultdict, deque


class StockMultiFactorStrategy:
    def __init__(self, config, send_order):
        self.init(config, send_order)

    def init(self, config, send_order):
        self.send_order = send_order
        self.window = int(config.get("window", "20"))
        self.w_mom = float(config.get("w_mom", "0.6"))
        self.w_vol = float(config.get("w_vol", "0.4"))
        self.buy_thresh = float(config.get("buy_thresh", "0.02"))
        self.sell_thresh = float(config.get("sell_thresh", "-0.015"))
        self.order_volume = int(config.get("order_volume", "1000"))
        # 与 core/include/protocol.h 中 K_1D 一致
        self.daily_interval = int(config.get("daily_interval", "1440"))

        maxlen = self.window + 5
        self._closes = defaultdict(lambda: deque(maxlen=maxlen))
        self._volumes = defaultdict(lambda: deque(maxlen=maxlen))
        self._position = {}

    def on_kline(self, kline):
        if int(kline.get("interval", 0)) != self.daily_interval:
            return
        sym = kline.get("symbol")
        if not sym:
            return
        close = float(kline.get("close", 0.0))
        vol = int(kline.get("volume", 0))
        if close <= 0:
            return

        c = self._closes[sym]
        v = self._volumes[sym]
        c.append(close)
        v.append(max(vol, 1))

        if len(c) < self.window:
            return

        sma = sum(c) / float(len(c))
        mom = (close / sma - 1.0) if sma > 0 else 0.0

        log_v = [math.log(float(x)) for x in v]
        avg_lv = sum(log_v) / float(len(log_v))
        cur_lv = math.log(float(max(vol, 1)))
        vol_z = cur_lv - avg_lv

        score = self.w_mom * mom + self.w_vol * vol_z

        pos = self._position.get(sym)
        price = close

        if score > self.buy_thresh and pos != "long":
            self.send_order(sym, "B", "O", price, self.order_volume)
            self._position[sym] = "long"
        elif score < self.sell_thresh and pos == "long":
            self.send_order(sym, "S", "O", price, self.order_volume)
            self._position[sym] = None
