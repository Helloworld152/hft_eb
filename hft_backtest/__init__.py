__version__ = "0.1.0"

from .engine import BacktestEngine
from .objects import AccountData, BacktestResult, BarData, OrderData, TickData, TradeData
from .stats import calculate_statistics
from .template import CtaTemplate

__all__ = [
    "AccountData",
    "BacktestEngine",
    "BacktestResult",
    "BarData",
    "CtaTemplate",
    "OrderData",
    "TickData",
    "TradeData",
    "calculate_statistics",
]
