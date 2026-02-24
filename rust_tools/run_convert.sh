#!/bin/bash
# 1. 确保输出目录存在
mkdir -p ../data/parquet

# 2. 编译
cargo build --release

# 3. 运行转换 (默认 20260210)
DATE=${1:-20260210}
echo "Converting data for ${DATE}..."
./target/release/hft_reader ../data/market_data_${DATE} --parquet -o ../data/parquet/market_data_${DATE}.parquet
