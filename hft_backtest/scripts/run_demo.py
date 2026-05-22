from hft_backtest import BacktestEngine
from hft_backtest.examples.demo_strategy import DemoStrategy


def main():
    engine = BacktestEngine()
    engine.set_parameters(
        data_mode="tick",
        data_file="data/market_data_20260319_night",
        symbols_file="conf/symbols.txt",
        initial_balance=1_000_000,
        slippage_ticks=0.01,
        tick_size=1.0,
        extra_py_config={"sample_every": "1"},
        replay_config={"idle_stop_sec": 5},
    )
    engine.add_strategy(DemoStrategy, {"symbol": "au2606"})
    result = engine.run_backtesting()
    print(result.statistics)


if __name__ == "__main__":
    main()
