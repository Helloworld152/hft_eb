import os
import sys

if __package__ is None or __package__ == "":
    sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from hft_backtest import BacktestEngine
from hft_backtest.examples.demo_strategy import DemoStrategy

engine = BacktestEngine()
engine.set_parameters(
    data_mode="tick",
    data_file="/home/rying/hft_eb/data/market_data_20260508_night",
    symbols_file="/home/rying/hft_eb/conf/symbols.txt",
    initial_balance=1_000_000,
    output_dir="/home/rying/hft_eb/my_backtest_results/ag2606_test",
)
engine.add_strategy(DemoStrategy, {"symbol": "ag2606"})
result = engine.run_backtesting()
print(result.output_dir)
