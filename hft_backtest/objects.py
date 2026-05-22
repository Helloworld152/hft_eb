from dataclasses import dataclass, field
from typing import Dict


def _to_float(value, default=0.0):
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def _to_int(value, default=0):
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


@dataclass
class TickData:
    symbol: str
    symbol_id: int
    last_price: float
    volume: int
    turnover: float
    open_interest: float
    trading_day: int
    update_time: int
    extra: Dict[str, float] = field(default_factory=dict)

    @classmethod
    def from_dict(cls, data):
        return cls(
            symbol=str(data.get("symbol", "")),
            symbol_id=_to_int(data.get("symbol_id")),
            last_price=_to_float(data.get("last_price")),
            volume=_to_int(data.get("volume")),
            turnover=_to_float(data.get("turnover")),
            open_interest=_to_float(data.get("open_interest")),
            trading_day=_to_int(data.get("trading_day")),
            update_time=_to_int(data.get("update_time")),
            extra=dict(data),
        )


@dataclass
class BarData:
    symbol: str
    symbol_id: int
    trading_day: int
    start_time: int
    open_price: float
    high_price: float
    low_price: float
    close_price: float
    volume: int
    turnover: float
    open_interest: float
    interval: int

    @classmethod
    def from_dict(cls, data):
        return cls(
            symbol=str(data.get("symbol", "")),
            symbol_id=_to_int(data.get("symbol_id")),
            trading_day=_to_int(data.get("trading_day")),
            start_time=_to_int(data.get("start_time")),
            open_price=_to_float(data.get("open")),
            high_price=_to_float(data.get("high")),
            low_price=_to_float(data.get("low")),
            close_price=_to_float(data.get("close")),
            volume=_to_int(data.get("volume")),
            turnover=_to_float(data.get("turnover")),
            open_interest=_to_float(data.get("open_interest")),
            interval=_to_int(data.get("interval")),
        )


@dataclass
class OrderData:
    symbol: str
    direction: str
    offset: str
    price: float
    volume_total: int
    volume_traded: int
    status: str
    order_ref: str
    order_sys_id: str

    @classmethod
    def from_dict(cls, data):
        return cls(
            symbol=str(data.get("symbol", "")),
            direction=str(data.get("direction", "")),
            offset=str(data.get("offset_flag", "")),
            price=_to_float(data.get("limit_price")),
            volume_total=_to_int(data.get("volume_total")),
            volume_traded=_to_int(data.get("volume_traded")),
            status=str(data.get("status", "")),
            order_ref=str(data.get("order_ref", "")),
            order_sys_id=str(data.get("order_sys_id", "")),
        )


@dataclass
class TradeData:
    symbol: str
    direction: str
    offset: str
    price: float
    volume: int
    trade_id: str
    order_ref: str

    @classmethod
    def from_dict(cls, data):
        return cls(
            symbol=str(data.get("symbol", "")),
            direction=str(data.get("direction", "")),
            offset=str(data.get("offset_flag", "")),
            price=_to_float(data.get("price")),
            volume=_to_int(data.get("volume")),
            trade_id=str(data.get("trade_id", "")),
            order_ref=str(data.get("order_ref", "")),
        )


@dataclass
class AccountData:
    account_id: str
    balance: float
    available: float
    margin: float
    close_pnl: float
    position_pnl: float

    @classmethod
    def from_dict(cls, data):
        return cls(
            account_id=str(data.get("account_id", "")),
            balance=_to_float(data.get("balance")),
            available=_to_float(data.get("available")),
            margin=_to_float(data.get("margin")),
            close_pnl=_to_float(data.get("close_pnl")),
            position_pnl=_to_float(data.get("position_pnl")),
        )


@dataclass
class BacktestResult:
    output_dir: str = ""
    config_path: str = ""
    returncode: int = 0
    orders: list = field(default_factory=list)
    trades: list = field(default_factory=list)
    accounts: list = field(default_factory=list)
    statistics: Dict[str, float] = field(default_factory=dict)
