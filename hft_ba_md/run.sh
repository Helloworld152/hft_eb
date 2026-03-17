#!/bin/bash
set -e
cd $(dirname $0)
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
BIN_PATH="${SCRIPT_DIR}/bin/hft_ba_recorder"
CONFIG_PATH="${SCRIPT_DIR}/conf/config.yaml"

# 日期参数：默认今天 UTC，或使用参数 YYYYMMDD
TRADING_DAY="${1:-$(date -u +%Y%m%d)}"

# 更新配置文件中的 trading_day
sed -i "s/^trading_day:.*/trading_day: '${TRADING_DAY}'/" "$CONFIG_PATH"

# 检查二进制文件
if [ ! -f "$BIN_PATH" ]; then
    echo "Error: Binary not found. Please run ./build.sh first."
    exit 1
fi

echo ">>> Starting Binance Market Data Snapshot..."
echo ">>> Trading Day (UTC): $TRADING_DAY"
echo "----------------------------------------"

mkdir -p "${SCRIPT_DIR}/log"
"$BIN_PATH" "$CONFIG_PATH"
