import math


def _balance_series(accounts, initial_balance):
    if not accounts:
        return [float(initial_balance)]
    series = []
    for row in accounts:
        try:
            series.append(float(row.get("balance", initial_balance)))
        except (TypeError, ValueError):
            continue
    return series or [float(initial_balance)]


def calculate_statistics(accounts, trades, initial_balance):
    initial_balance = float(initial_balance)
    balances = _balance_series(accounts, initial_balance)
    final_balance = balances[-1]
    total_return = 0.0 if initial_balance == 0 else final_balance / initial_balance - 1.0

    peak = balances[0]
    max_drawdown = 0.0
    returns = []
    prev = balances[0]
    for balance in balances[1:]:
        if balance > peak:
            peak = balance
        if peak > 0:
            max_drawdown = max(max_drawdown, (peak - balance) / peak)
        if prev != 0:
            returns.append(balance / prev - 1.0)
        prev = balance

    if returns:
        mean = sum(returns) / len(returns)
        var = sum((r - mean) ** 2 for r in returns) / len(returns)
        std = math.sqrt(var)
        sharpe = 0.0 if std < 1e-12 else math.sqrt(len(returns)) * mean / std
    else:
        sharpe = 0.0

    turnover = 0.0
    for trade in trades:
        try:
            turnover += float(trade.get("price", 0.0)) * int(trade.get("volume", 0))
        except (TypeError, ValueError):
            continue

    return {
        "initial_balance": initial_balance,
        "final_balance": final_balance,
        "total_return": total_return,
        "max_drawdown": max_drawdown,
        "sharpe": sharpe,
        "trade_count": len(trades),
        "turnover": turnover,
        "account_points": len(balances),
    }
