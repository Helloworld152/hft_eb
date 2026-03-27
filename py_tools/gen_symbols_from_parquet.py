#!/usr/bin/env python3
import argparse
import pyarrow.parquet as pq


def main():
    parser = argparse.ArgumentParser(description="Generate symbols mapping from parquet file")
    parser.add_argument("parquet", help="Input parquet file")
    parser.add_argument("-o", "--output", default="/home/rying/hft_eb/conf/symbols_a_share.txt", help="Output symbols file")
    parser.add_argument("--start-id", type=int, default=10000000, help="Start symbol id")
    parser.add_argument("--multiplier", type=float, default=1.0, help="Default multiplier")
    args = parser.parse_args()

    table = pq.read_table(args.parquet, columns=["symbol"])
    symbols = table.column("symbol").to_pylist()
    uniq = sorted({s for s in symbols if s})

    with open(args.output, "w", encoding="utf-8") as f:
        for i, sym in enumerate(uniq):
            f.write(f"{args.start_id + i}:{sym}:{args.multiplier}\n")

    print(f"Wrote {len(uniq)} symbols to {args.output}")


if __name__ == "__main__":
    main()
