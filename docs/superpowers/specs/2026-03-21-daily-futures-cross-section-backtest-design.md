# Daily Futures Cross-Section Backtest (Design)

Date: 2026-03-21

## Summary
Build a daily-frequency, cross-sectional futures backtest pipeline that reuses the existing `EVENT_KLINE` event type, supports rolling main-contract universe switches, and runs in backtest-only mode. The design follows a four-plugin split (data source, factor, decision, backtest) to maximize modularity and factor/strategy reuse.

## Scope
### In scope
- Daily-frequency futures data from `.pq` files.
- Rolling universe (main-contract switching).
- Daily rebalancing.
- Backtest-only execution with fixed fee + fixed slippage cost model.
- Event-driven pipeline using existing EventBus and `EVENT_KLINE`.

### Out of scope
- Live trading / real-time market data ingestion.
- Intraday or high-frequency signals.
- Advanced cost models (variable slippage, impact curves) beyond fixed fee + fixed slippage.

## Goals
- Reuse existing event system and module lifecycle.
- Clean separation of responsibilities to support swapping factors and decision logic.
- Minimal invasiveness to existing codebase.

## Non-goals
- Over-optimizing performance beyond daily-frequency needs.
- Building a general backtest UI or web dashboard.

## Architecture Overview
Event flow (daily):

1. `.pq` Kline Source publishes `EVENT_KLINE`.
2. Factor plugin subscribes `EVENT_KLINE` and publishes `EVENT_SIGNAL`.
3. Cross-section decision plugin consumes signals and produces daily target orders.
4. Backtest plugin simulates fills, applies costs, updates portfolio, outputs metrics.

## Plugin Breakdown
### 1) `mod_pq_kline_source`
Responsibilities:
- Load daily futures data from `.pq`.
- Emit `KlineRecord` per symbol/day on `EVENT_KLINE`.

Key config:
- `data_path` (directory or file list)
- `date_start`, `date_end`
- `symbol_map` or `symbols_file`

### 2) `mod_factor_daily`
Responsibilities:
- Subscribe to `EVENT_KLINE`.
- Compute daily factor(s).
- Publish `EVENT_SIGNAL` for each symbol/day.

Key config:
- `factors`: list and parameters (e.g., lookback).
- `signal_namespace` or naming scheme.

### 3) `mod_xs_decision`
Responsibilities:
- Subscribe to `EVENT_SIGNAL`.
- Rank/sort signals across the day’s universe.
- Generate target orders (long/short or long-only).

Output format:
- Use `OrderReq` as “rebalance intent” (per-symbol target orders) to minimize new types.

Key config:
- `top_n` / `bottom_n`
- `long_short` or `long_only`
- `weighting` (equal weight / score weight)
- `universe_roll` rules

### 4) `mod_backtest_daily`
Responsibilities:
- Consume daily order intents.
- Simulate execution (price basis + fixed slippage).
- Apply fixed fee rate.
- Update positions, cash, PnL.
- Output daily NAV and metrics.

Key config:
- `initial_cash`
- `fee_rate`
- `slippage_ticks`
- `price_basis` (`open` or `close`)
- `roll_rule` (apply on rebalancing day)

## Data Interfaces
- **Input**: `.pq` daily bars to `KlineRecord`.
- **Signals**: `SignalRecord` using `EVENT_SIGNAL`.
- **Orders**: `OrderReq` as rebalance instructions.

## Cost Model (MVP)
- Fixed fee rate on notional (configurable).
- Fixed slippage in ticks, applied on trade price.

## Rebalancing Logic (Daily)
- Once per day after all symbols for the day are ingested.
- Target selection based on cross-sectional rank.
- Generate `OrderReq` to move toward target weights.

## Metrics (MVP)
- NAV curve
- Daily returns
- Max drawdown
- Annualized return
- Turnover

## Risks & Mitigations
- **Misalignment between Kline timestamps and signal timing**: enforce “close-to-close” evaluation and explicit trade time.
- **Universe roll edge cases**: define clear roll calendar or external map.
- **OrderReq semantic overload**: document clearly in module README and config to avoid confusion with live trading.

## Open Questions
- Exact `.pq` schema (columns, symbol formats).
- Universe roll rule specification (main-contract mapping format).
- Whether to output detailed trade logs vs summary metrics only.

## Next Steps
- Confirm `.pq` schema and roll mapping format.
- Implement per-plugin config definitions and minimal MVP.
- Validate against a small sample dataset.
