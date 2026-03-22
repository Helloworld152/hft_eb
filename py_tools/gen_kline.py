import polars as pl
import sys
import os

if len(sys.argv) < 2:
    print("Usage: python3 gen_kline.py <input.parquet> [interval]")
    sys.exit(1)

file_path = sys.argv[1]
interval = sys.argv[2] if len(sys.argv) > 2 else "1m"

# 1. 读取数据
df = pl.read_parquet(file_path)

# 2. 原始时间戳转换 (YYYYMMDD + HHMMSSmmm)
df = df.with_columns(
    (pl.col("3").cast(pl.Utf8) + pl.col("4").cast(pl.Utf8).str.rjust(9, "0"))
    .str.to_datetime("%Y%m%d%H%M%S%3f")
    .alias("ts")
).sort("ts")

# 3. 聚合成 K 线 (兼容性参数)
try:
    gb = df.groupby_dynamic(index_column="ts", every=interval, by="1")
except:
    gb = df.group_by_dynamic(index_column="ts", every=interval, group_by="1")

kline = gb.agg([
    pl.col("5").first().alias("open"),
    pl.col("5").max().alias("high"),
    pl.col("5").min().alias("low"),
    pl.col("5").last().alias("close"),
    (pl.col("6").last() - pl.col("6").first()).alias("vol"),
    pl.col("2").first().alias("symbol_id"),
    pl.col("3").first().alias("day")
])

# 4. 自动生成输出并保存
os.makedirs("data/kline", exist_ok=True)
base_name = os.path.basename(file_path).replace(".parquet", "")
output_path = f"data/kline/{base_name}_{interval}.parquet"

kline.write_parquet(output_path)
print(f"Success. Reverted to simple version. Saved to: {output_path}")
print(f"Total bars: {len(kline)}")
