#!/bin/bash

# 获取脚本所在目录
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
BIN_PATH="${SCRIPT_DIR}/bin/hft_recorder"
CONFIG_PATH="${SCRIPT_DIR}/conf/config.json"
CTP_LIB_PATH="${SCRIPT_DIR}/../third_party/ctp/lib"

# 检查二进制文件是否存在
if [ ! -f "$BIN_PATH" ]; then
    echo "Error: Binary not found at $BIN_PATH. Please run ./build.sh first."
    exit 1
fi

# 设置动态库路径，确保能找到 CTP SDK
export LD_LIBRARY_PATH=$CTP_LIB_PATH:$LD_LIBRARY_PATH

echo ">>> Starting OmniQuant Market Data Recorder..."
echo ">>> Binary: $BIN_PATH"
echo ">>> Config: $CONFIG_PATH"
echo "----------------------------------------"

# 运行程序
mkdir -p "${SCRIPT_DIR}/log"
"${SCRIPT_DIR}/bin/hft_recorder" "$CONFIG_PATH"
