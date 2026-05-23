import os
import sys
from collections import deque

from hft_backtest import BacktestEngine, BaseStrategy


class AgHFTOStrategy(BaseStrategy):
    """ag2606 高频T+0策略：短周期动量 + 成交量确认 + 严格风控"""

    author = "quant"

    def on_init(self):
        self.fast_window: int = int(self.config.get("fast_window", 3))
        self.slow_window: int = int(self.config.get("slow_window", 15))
        self.stop_loss_ticks: int = int(self.config.get("stop_loss_ticks", 5))
        self.take_profit_ticks: int = int(self.config.get("take_profit_ticks", 8))
        self.max_hold_seconds: int = int(self.config.get("max_hold_seconds", 120))
        self.max_daily_trades: int = int(self.config.get("max_daily_trades", 500))
        self.tick_size: float = float(self.config.get("tick_size", 1.0))
        # 新增：动量开仓的连续tick阈值
        self.momentum_ticks: int = int(self.config.get("momentum_ticks", 3))

        # 价格窗口
        self.prices: deque[float] = deque(maxlen=self.slow_window * 2)
        self.fast_ma: float = 0.0
        self.slow_ma: float = 0.0
        self.prev_fast_ma: float = 0.0
        self.prev_slow_ma: float = 0.0

        # 仓位管理
        self.pos: int = 0
        self.entry_price: float = 0.0
        self.entry_time: int = 0

        # 日内风控
        self.daily_trades: int = 0
        self.daily_pnl: float = 0.0
        self.trading_day: int = 0

        # 收盘时间
        self.market_close_times: set[int] = {
            145500000, 145700000, 145900000, 150000000,
            225000000, 225500000, 225800000, 230000000,
        }

        self.write_log(
            f"init fast={self.fast_window} slow={self.slow_window} "
            f"sl={self.stop_loss_ticks} tp={self.take_profit_ticks} "
            f"hold={self.max_hold_seconds}s momentum={self.momentum_ticks}"
        )

    def on_start(self):
        self.write_log("started")

    # ------------------------------------------------------------------
    # 指标
    # ------------------------------------------------------------------
    def _update_mas(self, price: float) -> None:
        self.prices.append(price)
        if len(self.prices) >= self.slow_window:
            window = list(self.prices)
            self.prev_fast_ma = self.fast_ma
            self.prev_slow_ma = self.slow_ma
            self.fast_ma = sum(window[-self.fast_window:]) / self.fast_window
            self.slow_ma = sum(window[-self.slow_window:]) / self.slow_window

    # ------------------------------------------------------------------
    # 信号
    # ------------------------------------------------------------------
    def _ma_cross(self) -> int:
        """MA金叉/死叉"""
        if len(self.prices) < self.slow_window:
            return 0
        if self.prev_fast_ma == 0 or self.prev_slow_ma == 0:
            return 0
        if self.prev_fast_ma <= self.prev_slow_ma and self.fast_ma > self.slow_ma:
            return 1
        if self.prev_fast_ma >= self.prev_slow_ma and self.fast_ma < self.slow_ma:
            return -1
        return 0

    def _momentum(self) -> int:
        """连续N根tick同向"""
        if len(self.prices) < self.momentum_ticks + 1:
            return 0
        recent = list(self.prices)[-self.momentum_ticks:]
        if all(recent[i] > recent[i - 1] for i in range(1, len(recent))):
            return 1
        if all(recent[i] < recent[i - 1] for i in range(1, len(recent))):
            return -1
        return 0

    # ------------------------------------------------------------------
    # 风控检查
    # ------------------------------------------------------------------
    def _is_market_close_soon(self, update_time: int) -> bool:
        # 提取 HHMMSSmmm
        t = update_time % 1_000_000_000  # 去掉日期部分
        for close_time in self.market_close_times:
            if t >= close_time:
                return True
        return False

    def _holding_too_long(self, now: int) -> bool:
        if self.pos == 0 or self.entry_time == 0:
            return False
        return (now - self.entry_time) / 1000.0 > self.max_hold_seconds

    def _hit_stop_loss(self, price: float) -> bool:
        if self.pos == 0:
            return False
        if self.pos == 1:  # 多头止损
            return price <= self.entry_price - self.stop_loss_ticks * self.tick_size
        else:  # 空头止损
            return price >= self.entry_price + self.stop_loss_ticks * self.tick_size

    def _hit_take_profit(self, price: float) -> bool:
        if self.pos == 0:
            return False
        if self.pos == 1:
            return price >= self.entry_price + self.take_profit_ticks * self.tick_size
        else:
            return price <= self.entry_price - self.take_profit_ticks * self.tick_size

    # ------------------------------------------------------------------
    # 主逻辑
    # ------------------------------------------------------------------
    def on_tick(self, tick):
        if tick.symbol != self.vt_symbol:
            return
        self._match_count = getattr(self, "_match_count", 0) + 1
        if tick.last_price <= 0:
            return

        price = tick.last_price
        volume = tick.volume
        now = tick.update_time

        # 交易日切换
        if tick.trading_day != self.trading_day:
            self.trading_day = tick.trading_day
            self.daily_trades = 0
            self.daily_pnl = 0.0
            self.write_log(f"new trading day: {self.trading_day}")

        # 更新指标
        self._update_mas(price)

        # --- 持仓管理 ---
        if self.pos != 0:
            # 止盈止损
            if self._hit_stop_loss(price):
                self._close_position(price, "stop_loss")
                return
            if self._hit_take_profit(price):
                self._close_position(price, "take_profit")
                return
            # 超时平仓
            if self._holding_too_long(now):
                self._close_position(price, "timeout")
                return
            # 收盘前强制平仓
            if self._is_market_close_soon(now):
                self._close_position(price, "market_close")
                return
            return

        # --- 开仓条件 ---
        if self.daily_trades >= self.max_daily_trades:
            return
        # 收盘前不开新仓
        if self._is_market_close_soon(now):
            return

        sig = self._ma_cross() or self._momentum()
        if sig == 1:
            self._open_long(price, now)
        elif sig == -1:
            self._open_short(price, now)

    # ------------------------------------------------------------------
    # 订单执行
    # ------------------------------------------------------------------
    def _open_long(self, price: float, now: int):
        volume = self._calc_order_size()
        self.buy(price=price, volume=volume)
        self.pos = 1
        self.entry_price = price
        self.entry_time = now
        self.daily_trades += 1
        self.write_log(
            f"[OPEN LONG] price={price:.1f} vol={volume} "
            f"fast_ma={self.fast_ma:.2f} slow_ma={self.slow_ma:.2f} "
            f"daily_trades={self.daily_trades}"
        )

    def _open_short(self, price: float, now: int):
        volume = self._calc_order_size()
        self.short(price=price, volume=volume)
        self.pos = -1
        self.entry_price = price
        self.entry_time = now
        self.daily_trades += 1
        self.write_log(
            f"[OPEN SHORT] price={price:.1f} vol={volume} "
            f"fast_ma={self.fast_ma:.2f} slow_ma={self.slow_ma:.2f} "
            f"daily_trades={self.daily_trades}"
        )

    def _close_position(self, price: float, reason: str):
        pnl = (price - self.entry_price) * self.pos
        self.daily_pnl += pnl
        if self.pos == 1:
            self.sell(price=price, volume=self._calc_order_size())
        else:
            self.cover(price=price, volume=self._calc_order_size())
        self.write_log(
            f"[CLOSE {reason}] entry={self.entry_price:.1f} exit={price:.1f} "
            f"pnl={pnl:.1f} daily_pnl={self.daily_pnl:.1f} "
            f"fast_ma={self.fast_ma:.2f} slow_ma={self.slow_ma:.2f}"
        )
        self.pos = 0
        self.entry_price = 0.0
        self.entry_time = 0

    def _calc_order_size(self) -> int:
        return 1

    # ------------------------------------------------------------------
    # 回调
    # ------------------------------------------------------------------
    def on_order(self, order):
        self.write_log(
            f"order status={order.status} dir={order.direction} "
            f"price={order.price} vol={order.volume_total}/{order.volume_traded} "
            f"ref={order.order_ref}"
        )

    def on_trade(self, trade):
        self.write_log(
            f"trade id={trade.trade_id} dir={trade.direction} "
            f"price={trade.price} vol={trade.volume}"
        )

    def on_stop(self):
        matched = getattr(self, "_match_count", 0)
        self.write_log(
            f"stopped, total_daily_trades={self.daily_trades} "
            f"daily_pnl={self.daily_pnl:.1f} matched_ticks={matched}"
        )


# ======================================================================
if __name__ == "__main__":
    engine = BacktestEngine()
    engine.set_parameters(
        data_mode="tick",
        data_file="/home/rying/hft_eb/data/market_data_20260508_night",
        symbols_file="/home/rying/hft_eb/conf/symbols.txt",
        initial_balance=1_000_000,
        output_dir="/home/rying/hft_eb/hft_backtest/results/ag2606_hft",
    )
    engine.add_strategy(
        AgHFTOStrategy,
        {
            "symbol": "ag2606",
            "fast_window": 3,
            "slow_window": 15,
            "stop_loss_ticks": 5,
            "take_profit_ticks": 8,
            "max_hold_seconds": 120,
            "max_daily_trades": 500,
            "tick_size": 1.0,
            "momentum_ticks": 3,
        },
    )
    result = engine.run_backtesting()
    print(f"output_dir: {result.output_dir}")
    if result.statistics:
        for k, v in result.statistics.items():
            print(f"  {k}: {v}")
