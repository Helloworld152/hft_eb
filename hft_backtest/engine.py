import csv
import os
import tempfile

from .objects import BacktestResult
from .stats import calculate_statistics


class BacktestEngine:
    def __init__(self, repo_root=None, native_engine=None):
        this_dir = os.path.abspath(os.path.dirname(__file__))
        self.package_dir = this_dir
        self.package_lib_dir = os.path.join(this_dir, "lib")
        self.repo_root = os.path.abspath(repo_root or os.path.join(this_dir, ".."))
        self._native = native_engine
        self._parameters = {}
        self._strategy_class = None
        self._strategy_setting = {}

    def _ensure_native(self):
        if self._native is None:
            from ._core import NativeBacktestEngine
            self._native = NativeBacktestEngine()
        return self._native

    def set_parameters(
        self,
        *,
        data_mode,
        data_file,
        symbols_file,
        initial_balance=1000000.0,
        slippage_ticks=0.0,
        tick_size=1.0,
        risk_max_orders_per_second=10000000,
        output_dir=None,
        replay_config=None,
        extra_py_config=None,
    ):
        self._parameters = {
            "data_mode": data_mode,
            "data_file": data_file,
            "symbols_file": symbols_file,
            "initial_balance": initial_balance,
            "slippage_ticks": slippage_ticks,
            "tick_size": tick_size,
            "risk_max_orders_per_second": risk_max_orders_per_second,
            "output_dir": output_dir,
            "replay_config": replay_config or {},
            "extra_py_config": extra_py_config or {},
        }

    def add_strategy(self, strategy_class, setting=None):
        self._strategy_class = strategy_class
        self._strategy_setting = dict(setting or {})

    def load_config(self, config_path, logger_name="hft_backtest"):
        return self._ensure_native().load_config(config_path, logger_name)

    def run(self):
        self._ensure_native().run()

    def start(self):
        self._ensure_native().start()

    def stop(self):
        self._ensure_native().stop()

    @property
    def config_path(self):
        return self._ensure_native().config_path

    def run_backtesting(self):
        if self._strategy_class is None:
            raise RuntimeError("strategy is not set, call add_strategy(...) first")
        if not self._parameters:
            raise RuntimeError("parameters are not set, call set_parameters(...) first")

        output_dir = self._parameters.get("output_dir")
        if output_dir is None:
            output_dir = tempfile.mkdtemp(prefix="hft_backtest_", dir=os.path.join(self.repo_root, "tmp"))
        os.makedirs(output_dir, exist_ok=True)

        config_path = self._write_config(
            strategy_class=self._strategy_class,
            output_dir=output_dir,
            strategy_params=self._strategy_setting,
            data_mode=self._parameters["data_mode"],
            data_file=self._parameters["data_file"],
            symbols_file=self._parameters["symbols_file"],
            initial_balance=self._parameters["initial_balance"],
            slippage_ticks=self._parameters["slippage_ticks"],
            tick_size=self._parameters["tick_size"],
            risk_max_orders_per_second=self._parameters["risk_max_orders_per_second"],
            extra_py_config=self._parameters["extra_py_config"],
            replay_config=self._parameters["replay_config"],
        )

        ok = self.load_config(config_path)
        if not ok:
            raise RuntimeError(f"failed to load config: {config_path}")
        self.run()

        orders = self._read_csv(os.path.join(output_dir, "orders.csv"))
        trades = self._read_csv(os.path.join(output_dir, "trades.csv"))
        accounts = self._read_csv(os.path.join(output_dir, "accounts.csv"))
        stats = calculate_statistics(accounts, trades, initial_balance=self._parameters["initial_balance"])
        return BacktestResult(
            output_dir=output_dir,
            config_path=config_path,
            returncode=0,
            orders=orders,
            trades=trades,
            accounts=accounts,
            statistics=stats,
        )

    def _write_config(
        self,
        *,
        strategy_class,
        output_dir,
        strategy_params,
        data_mode,
        data_file,
        symbols_file,
        initial_balance,
        slippage_ticks,
        tick_size,
        risk_max_orders_per_second=10000000,
        extra_py_config=None,
        replay_config=None,
    ):
        extra_py_config = extra_py_config or {}
        replay_config = replay_config or {}
        py_module = strategy_class.__module__
        py_class = strategy_class.__name__
        config_path = os.path.join(output_dir, "backtest.yaml")
        replay_lib = self._package_library("libmod_replay.so")
        py_strategy_lib = self._package_library("libmod_py_strategy.so")
        risk_lib = self._package_library("libmod_risk.so")
        recorder_lib = self._package_library("libmod_backtest_recorder.so")
        sim_trade_lib = self._package_library("libmod_sim_trade.so")
        kline_replay_lib = self._package_library("libmod_kline_parquet_replay.so")

        lines = [
            f"symbols_file: {symbols_file}",
            "",
            "snapshot:",
            "  type: local",
            "",
            "plugins:",
        ]
        if data_mode == "tick":
            lines.extend([
                "  - name: Replay",
                f"    library: {replay_lib}",
                "    enabled: true",
                "    config:",
                f"      data_file: {data_file}",
                f"      debug: {str(replay_config.get('debug', False)).lower()}",
                f"      idle_stop_sec: {replay_config.get('idle_stop_sec', 5)}",
            ])
        elif data_mode == "bar":
            lines.extend([
                "  - name: Kline_Parquet_Replay",
                f"    library: {kline_replay_lib}",
                "    enabled: true",
                "    config:",
                f"      data_file: {data_file}",
                f"      debug: {str(replay_config.get('debug', False)).lower()}",
                f"      sleep_ms_per_bar: {replay_config.get('sleep_ms_per_bar', 0)}",
                f"      start_time_hhmmssmmm: \"{replay_config.get('start_time_hhmmssmmm', '093000000')}\"",
                f"      stop_on_finish: {str(replay_config.get('stop_on_finish', True)).lower()}",
            ])
        else:
            raise ValueError(f"unsupported data_mode: {data_mode}")

        lines.extend([
            "",
            "  - name: PyStrategy",
            f"    library: {py_strategy_lib}",
            "    enabled: true",
            "    config:",
            f"      py_module: {py_module}",
            f"      py_class: {py_class}",
            "      py_path: .",
            "      error_policy: stop",
            f"      strategy_name: {py_class}",
        ])
        merged = dict(strategy_params)
        merged.update(extra_py_config)
        for key, value in merged.items():
            lines.append(f"      {key}: {value}")

        lines.extend([
            "",
            "  - name: Risk",
            f"    library: {risk_lib}",
            "    enabled: true",
            "    config:",
            f"      max_orders_per_second: {risk_max_orders_per_second}",
            "",
            "  - name: BacktestRecorder",
            f"    library: {recorder_lib}",
            "    enabled: true",
            "    config:",
            f"      output_dir: {output_dir}",
            "",
            "  - name: SimTrade",
            f"    library: {sim_trade_lib}",
            "    enabled: true",
            "    config:",
            "      debug: false",
            f"      slippage_ticks: \"{slippage_ticks}\"",
            f"      tick_size: \"{tick_size}\"",
            f"      initial_balance: \"{initial_balance}\"",
        ])

        with open(config_path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        return config_path

    def _package_library(self, filename):
        path = os.path.join(self.package_lib_dir, filename)
        return os.path.abspath(path)

    def _read_csv(self, path):
        if not os.path.isfile(path):
            return []
        with open(path, "r", encoding="utf-8") as f:
            return list(csv.DictReader(f))
