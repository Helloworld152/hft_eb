#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import polars as pl
import argparse
import os

# 数字协议映射表 (用于展示验证)
ID_TO_NAME = {
    "1": "symbol",
    "2": "symbol_id",
    "3": "trading_day",
    "4": "update_time",
    "5": "last_price",
    "6": "volume",
    "7": "turnover",
    "8": "open_interest",
    "9": "upper_limit",
    "10": "lower_limit",
    "11": "open_price",
    "12": "highest_price",
    "13": "lowest_price",
    "14": "pre_close_price",
    "15": "bid_price",
    "16": "bid_volume",
    "17": "ask_price",
    "18": "ask_volume"
}

def verify(file_path):
    if not os.path.exists(file_path):
        print(f"Error: File {file_path} not found.")
        return

    print(f"Loading {file_path}...")
    df = pl.read_parquet(file_path)

    print("\n--- Schema Check ---")
    cols = df.columns
    is_numeric = all(c.isdigit() for c in cols)
    print(f"Columns: {cols}")
    print(f"Numeric Protocol Verified: {'PASS' if is_numeric else 'FAIL'}")

    print("\n--- Data Preview (First 5 rows) ---")
    # 为了展示方便，我们将数字 ID 映射回名称
    rename_dict = {k: v for k, v in ID_TO_NAME.items() if k in df.columns}
    display_df = df.head(5).rename(rename_dict)
    
    # 打印前 5 行，列出主要字段
    print(display_df.select(["symbol", "symbol_id", "trading_day", "update_time", "last_price", "volume"]))

    print("\n--- Statistics ---")
    print(f"Total Rows: {len(df)}")
    
    if "1" in df.columns:
        unique_symbols = df["1"].n_unique()
        print(f"Unique Symbols: {unique_symbols}")

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('file', nargs='?', default='data/parquet/market_data_20260210.parquet', help='Parquet file to verify')
    args = parser.parse_args()
    
    verify(args.file)