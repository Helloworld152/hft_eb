import sys

from hft_backtest import BacktestEngine


def main():
    if len(sys.argv) < 2:
        print("Usage: hft-backtest-run-config <config.yaml>")
        return 1

    engine = BacktestEngine()
    ok = engine.load_config(sys.argv[1])
    if not ok:
        print(f"Failed to load config: {sys.argv[1]}")
        return 1
    engine.run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
