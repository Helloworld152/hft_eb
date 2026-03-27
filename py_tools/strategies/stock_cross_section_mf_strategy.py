"""
日线截面多因子：股票池内横截面 z-score，线性合成得分，做多前 n_long；换日时调仓上一日。
股票池来自 universe_file（与 YAML 中 symbol_filter 保持一致）。不设 on_finish 时，数据最后一天可能不调仓。
"""
import math
from collections import defaultdict, deque


def _zscore(vals):
    if len(vals) < 2:
        return [0.0] * len(vals)
    m = sum(vals) / float(len(vals))
    var = sum((x - m) ** 2 for x in vals) / float(len(vals))
    s = math.sqrt(var)
    if s < 1e-12:
        return [0.0] * len(vals)
    return [(x - m) / s for x in vals]


def _load_universe(path):
    out = []
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if line.count(":") >= 2:
                out.append(line.split(":")[1])
            else:
                out.append(line)
    return set(out)


class StockCrossSectionMFStrategy:
    def __init__(self, config, send_order):
        self.init(config, send_order)

    def init(self, config, send_order):
        self.send_order = send_order
        ufile = config.get("universe_file", "conf/cross_section_universe_100.txt")
        self.universe = _load_universe(ufile)
        self.window = int(config.get("window", "20"))
        self.w_mom = float(config.get("w_mom", "0.6"))
        self.w_vol = float(config.get("w_vol", "0.4"))
        self.n_long = int(config.get("n_long", "10"))
        self.min_names = int(config.get("min_names", "30"))
        self.order_volume = int(config.get("order_volume", "1000"))
        self.daily_interval = int(config.get("daily_interval", "1440"))

        self._hist_close = defaultdict(lambda: deque(maxlen=self.window))
        self._day_buffer = {}
        self._last_trading_day = None
        self._holdings = set()

    def on_kline(self, kline):
        if int(kline.get("interval", 0)) != self.daily_interval:
            return
        sym = kline.get("symbol")
        if not sym or sym not in self.universe:
            return
        close = float(kline.get("close", 0.0))
        vol = int(kline.get("volume", 0))
        if close <= 0:
            return
        d = int(kline.get("trading_day", 0))
        if d <= 0:
            return

        if self._last_trading_day is not None and d != self._last_trading_day:
            self._flush_day(self._last_trading_day)

        self._last_trading_day = d
        if d not in self._day_buffer:
            self._day_buffer[d] = {}
        self._day_buffer[d][sym] = {"close": close, "vol": max(vol, 1)}
        self._hist_close[sym].append(close)

    def _flush_day(self, day):
        data = self._day_buffer.pop(day, None)
        if not data:
            return

        syms = []
        moms = []
        logvs = []
        for sym in self.universe:
            if sym not in data:
                continue
            hc = self._hist_close[sym]
            if len(hc) < self.window:
                continue
            close = data[sym]["close"]
            sma = sum(hc) / float(len(hc))
            mom = (close / sma - 1.0) if sma > 0 else 0.0
            lv = math.log(float(data[sym]["vol"]))
            syms.append(sym)
            moms.append(mom)
            logvs.append(lv)

        if len(syms) < self.min_names:
            return

        zm = _zscore(moms)
        zv = _zscore(logvs)
        scores = [self.w_mom * zm[i] + self.w_vol * zv[i] for i in range(len(syms))]
        ranked = sorted(zip(syms, scores), key=lambda x: -x[1])
        n = min(self.n_long, len(ranked))
        target = set(s[0] for s in ranked[:n])

        to_sell = self._holdings - target
        to_buy = target - self._holdings

        for s in to_sell:
            if s in data:
                self.send_order(s, "S", "O", data[s]["close"], self.order_volume)
        for s in to_buy:
            if s in data:
                self.send_order(s, "B", "O", data[s]["close"], self.order_volume)

        self._holdings = set(target)
