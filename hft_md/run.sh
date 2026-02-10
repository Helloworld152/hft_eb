#!/bin/bash
cd $(dirname $0)
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
BIN_PATH="${SCRIPT_DIR}/bin/hft_recorder"
CONFIG_PATH="${SCRIPT_DIR}/conf/config.yaml"
CTP_LIB_PATH="${SCRIPT_DIR}/../third_party/ctp/lib"

# 日期参数：默认今天，或使用参数 YYYYMMDD
TRADING_DAY="${1:-$(date +%Y%m%d)}"

# 更新配置文件中的 trading_day
sed -i "s/^trading_day:.*/trading_day: '${TRADING_DAY}'/" "$CONFIG_PATH"

# 检查二进制文件
if [ ! -f "$BIN_PATH" ]; then
    echo "Error: Binary not found. Please run ./build.sh first."
    exit 1
fi

# 设置动态库路径
export LD_LIBRARY_PATH=$CTP_LIB_PATH:$LD_LIBRARY_PATH

echo ">>> Starting Market Data Recorder..."
echo ">>> Trading Day: $TRADING_DAY"
echo "----------------------------------------"

mkdir -p "${SCRIPT_DIR}/log"
nohup "$BIN_PATH" "$CONFIG_PATH" &
